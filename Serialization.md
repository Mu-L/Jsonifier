# Jsonifier Serialization: Design and Performance

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Jsonifier serializes a reflection-registered C++ object in two passes: an exact-or-over size estimate, one buffer resize, then a single branch-light write pass that never checks capacity. Every byte the schema already knows (key names, quotes, colons, commas, indentation) is folded at compile time into packed integer constants and emitted with fixed-width stores, so the runtime work left is the data itself: strings, numbers and container lengths.

This paper describes that design as it stands in `include/jsonifier-incl/serializing/` (`serializer.hpp`, `serialize_impl.hpp`) and its helpers for strings (`string_utils.hpp`), integers (`i_to_str.hpp`) and floats (`d_to_str.hpp`).

## Architecture

The whole pipeline is resolved at compile time from `jsonifier::core<T>` and `serialize_options`: one sizing walk, one resize, one write walk.

```mermaid
flowchart LR
    A["serializeJson<br/>entry point"] --> B["Size pass<br/>staticSize shortcuts"]
    B --> C["One resize<br/>bound + 64 bytes slack"]
    C --> D["Write pass<br/>fixed-width overstores"]
    D -- "POD values inlined, containers outlined" --> E["Member headers<br/>one packed store each"]
    D --> F["Strings<br/>SIMD, SWAR, scalar"]
    D --> G["Integers<br/>4 digits per store"]
    D --> H["Floats<br/>zmij shortest round-trip"]
```

The `serialize<options>` dispatcher forces inlining (`JSONIFIER_INLINE`) only for POD values and optionals of them (`inline_contained_v`). Objects, maps and vectors go through a plain `inline` function, so the compiler can keep large container bodies out of line instead of expanding every nested type into its caller. Each registered type gets its own `serialize_impl` specialization: objects, maps, vectors, fixed arrays, tuples, strings, chars, enums, numbers, bools, pointers and optionals, raw JSON, null, and variants (via `visit`).

On x86 the whole serializer is compiled once per instruction-set tier, and `serializeJson` picks the tier at run time. CPUID and XCR0 are read once on first use and the result is cached; every call after that compares a cached enum against the compiled tiers in a fold expression and calls that tier's serializer directly. There are no function pointers or virtual calls, and everything described below is compile-time code inside each tier. ARM builds compile one backend.

## Key techniques

### Size first, then write without bounds checks

`get_size_impl` computes an upper bound for the output before a byte is written. Fixed-width types report a `staticSize` (numbers and enums 32, `bool` 5, `char` 8, null 4), so a `vector<double>` is sized as `n × 32` plus separators with no loop. Strings are sized at `6n + 2` by default, the worst case where every byte becomes a `\u00XX` escape. With `prescanStrings` set, each string is scanned instead and sized at its exact escaped length plus 2 for the quotes (see *Pre-scanning strings*). Object keys contribute `key.size() + 3` (or `+ 4` when prettified) as a compile-time constant.

`serializeJson` adds 64 bytes of slack and calls `resize_and_overwrite` when the buffer has it, so no zero-fill happens. The write pass then trusts the bound completely: no growth checks, no reallocation, and every writer may overstore (write 8 bytes, advance 4) into the slack.

### Writing through `resize_and_overwrite`

When the output buffer has C++23's `resize_and_overwrite` (detected by the `has_resize_and_overwrite` concept), the whole write pass runs inside it. `std::string::resize_and_overwrite(n, op)` grows the buffer to at least `n` without initializing it, calls `op(data, n)`, and sets the final size to whatever `op` returns. One call replaces the usual three steps: zero-filling resize, write, shrinking resize.

The `op` Jsonifier passes is `serialize_writer_ro`, a small functor that holds a reference to the object:

```cpp
template<serialize_options options, typename value_type> struct serialize_writer_ro {
	JSONIFIER_INLINE uint64_t operator()(write_buffer_ptr ptrNew, uint64_t) noexcept {
		const write_buffer_ptr bufferPtr = serialize<options>::impl(object, ptrNew, 0);
		return static_cast<uint64_t>(bufferPtr - ptrNew);
	}
	value_type& object;
};
```

- **`operator()` is force-inlined.** `JSONIFIER_INLINE` makes the compiler expand the serializer into the standard library's call site, instead of leaving an out-of-line function that the library calls. The write pass then runs as part of `resize_and_overwrite`'s own body, with the buffer pointer in a register and no extra call frame. Whether `resize_and_overwrite` itself inlines into `serializeJson` is still the compiler's choice.
- **The functor is a reference and a function.** It holds only `value_type& object`, and its copy and move operations are deleted, so passing it costs nothing and the object is never copied.
- **The length comes straight from the pointer.** `operator()` returns the end pointer minus the start, so the final size is set from the write itself, with no second pass and no separate size variable.

Buffers without `resize_and_overwrite` take a fallback: `resize` to the bound if the buffer is smaller, write, then `resize` down to the written length. That path zero-fills on growth.

Single values skip the size pass entirely. A lone number reserves its type's maximum digit count, a lone string `6n + 2` plus one SIMD step, and a lone `bool` 8 bytes, each passed straight to the same `resize_and_overwrite` call.

### Schema text as packed integers

`packed_blitter` turns `"key":` into one constant at compile time. Literals up to 8 bytes become a single 2, 4 or 8-byte integer; longer ones become an array of `uint64_t` rounded up to 16 or a multiple of 32. Writing a member header is one `pow2MemcpyWrapper` store and a pointer bump by the true length.

### Member iteration: CABERIHT

Object members are walked with CABERIHT, Constexpr Aggregated Bases for Efficient Runtime Iteration of Heterogeneous Types. It is CAFBERIHT without the filter; the full pattern, with compile-time filtering, is described in [its own paper](https://nihilai-collective.net/cafberiht). Each member becomes a base class, and one fold expression over those bases writes the whole object in declaration order.

1. **One base per member.** `get_serialize_base` expands an index sequence over the `core<T>::parseValue` tuple and wraps each entry in `json_entity_serialize<options, entity>`. Each base carries its member's `name`, `memberPtr` and `isItLast` as compile-time constants.
2. **Aggregate the bases.** `serialize_map<bases...>` inherits from every one of them, so a type with 12 members gets one type with 12 bases.
3. **Fold over them.** `iterateValues` is a single fold, `((bufferPtr = bases::processIndex(value, bufferPtr, indent)), ...)`. Each `processIndex` is a static function of its own base, so every call is direct and resolved at compile time.

Inside each `processIndex`, nothing about the member is decided at runtime:

- the header `"name":` is that base's own `packed_blitter` constant;
- the value type is known, so `serialize<options>::impl` picks its writer with no dispatch;
- the trailing comma comes from `if constexpr (!isItLast)`, not an index check.

The sizing pass uses the same pattern: `size_getter_map` aggregates `json_entity_size` bases and folds `processIndex` over them, so both passes walk members the same way.

The result is no virtual calls, no `std::tuple` `get<I>` recursion, no runtime member index and no loop over members. A whole object becomes straight-line code: a run of fixed stores for headers and commas, with the value writers in between. The CAFBERIHT paper measures this same fold-over-bases dispatch as zero overhead against a hand-written baseline by assembly diff.

That is the distinction from CAFBERIHT. There, the bases are also filtered at compile time by tag, so only the components that match join the aggregate. Jsonifier's serializer keeps every registered member, so it has no filter step: the aggregate is exactly the members in `core<T>`. Runtime key exclusion (`jsonifierExcludedKeys`) is a separate mechanism, a set lookup inside each `processIndex` (see Limitations).

### Copies have a compile-time power-of-2 size

Nearly every copy on the write path has a power-of-2 size fixed at compile time: 2, 4 or 8 bytes. `pow2MemcpyWrapper<size>` takes the size as a template argument and `static_assert`s `std::has_single_bit(size)`, so a copy of any other size will not compile. A `memcpy` of 2, 4 or 8 bytes known at compile time lowers to a single unaligned load and store, with no call, no length branch and no tail loop.

What this does to the generated code:

| Copy | Typical x86-64 | Typical ARM64 |
| --- | --- | --- |
| `std::memcpy`, runtime length | `call memcpy` (or an inline length dispatch with branches and a tail loop) | `bl memcpy` |
| `jsonifierMemcpy`, runtime length under 64 bytes | one jump on `bit_width(length)`, then two overlapping 1/2/4/8/16/32-byte load–store pairs | the same, with `ldr`/`str` or `ldr q`/`str q` pairs |
| `jsonifierMemcpy`, runtime length of 64 bytes or more | `call memcpy` | `bl memcpy` |
| Compile-time 7 bytes | two overlapping 4-byte moves, or 4 + 2 + 1 | several loads and stores |
| Compile-time 2 / 4 / 8 bytes | one `mov` load, one `mov` store | one `ldrh`/`ldr` and one `strh`/`str` |
| Compile-time 2 / 4 / 8 bytes of constant data | one immediate store, no load (`mov qword ptr [rdi], imm`) | a constant materialized in a register, then one `str` |

- **No call.** A call to `memcpy` clobbers every caller-saved register, so live values have to be spilled and reloaded around it. Without the call, the buffer pointer and the values being written stay in registers for the whole member loop.
- **No length branches.** A runtime `memcpy` has to dispatch on the length before it copies anything. A fixed-size copy has no branch to predict or mispredict.
- **Constants become immediates.** Key headers, `true`/`false` and the empty `{}`/`[]`/`""` literals are compile-time data. Their copies can fold into immediate stores, so writing a member header needs no memory read at all.
- **Runtime lengths stay short.** Copies whose length is only known at run time, such as escape sequences and the runs that Minify and Prettify carry over, go through `jsonifierMemcpy` (`utility.hpp`). It switches on `std::bit_width(length)` and, for anything under 64 bytes, copies the first and the last power-of-2 chunk that covers the length. The two chunks overlap, so any length from 2 to 63 bytes takes exactly two loads and two stores (a single byte takes one) through `pow2MemcpyWrapper`, with no loop and no tail. Only lengths of 64 bytes or more reach `std::memcpy`. `jsonifierMemcpyUpTo<max_bytes>` takes a compile-time upper bound and drops every case above it, so a caller whose length is bounded never pays for the cases it cannot hit.
- **Inlining and scheduling.** Each copy is one or two instructions, so a whole object's serializer inlines into straight-line code. The compiler can then interleave stores from neighbouring members with the value work between them.

**What the benchmark corpus actually copies.** We counted every `jsonifierMemcpy` call by call site and length while serializing each write test's data once (all five single-value tests, all ten documents minified and prettified at both sizes) and running the Minify and Prettify tests once, on Windows / MSVC 19.44 at Jsonifier e0f3ae4. The counters were a temporary patch and are not in the library.

| Path | Call site | Calls | Lengths seen | 64 bytes or more |
| --- | --- | --- | --- | --- |
| Serialize | key and literal blit (`serialize_impl.hpp`) | 102,447 | 16 (83,762), 32 (18,207), 64 (478) | 478 |
| Serialize | escape sequence (`string_utils.hpp`) | 1,934 | 2 only | 0 |
| Minify | carried-over run between structurals | 20,208 | 1 to 127+, 106 distinct | 878 |
| Minify | stage-1 tail block (`simd.hpp`) | 1 | 127+ | 1 |
| Prettify | carried-over run between structurals | 20,208 | 1 to 127+, 106 distinct | 878 |
| Prettify | stage-1 tail block (`simd.hpp`) | 1 | 127+ | 1 |

On the serialize side, no runtime length reaches `std::memcpy`. Of the 104,381 calls, 98.1% are the member-header blit, and its length is `lengthToCopy`, a template constant (the key literal rounded up to 16, 32 or 64 bytes), so every one of those calls folds to fixed-size vector stores. The only serialize-side copy with a runtime length is the escape-table copy, and in this corpus every escape it wrote was 2 bytes. String bodies never pass through `jsonifierMemcpy`: the String test made no calls at all. The 478 64-byte blits are compile-time constants too, so they inline rather than call `memcpy`, although we have not checked that in the disassembly. Runtime lengths that are actually long come only from reformatting. Minify and Prettify each copy 20,208 runs of 106 distinct lengths, and 879 of them (4.3%) are 64 bytes or more and go to `std::memcpy`: long string values between two structural characters, plus one stage-1 tail block per call. The other 95.7% take the two-chunk path; the most common bucket, 8 to 15 bytes, holds 39% of all runs.

That is why the writers round up and overstore instead of copying exact lengths. A 5-byte `false` is one 8-byte store. A 3-digit group is a 4-byte load followed by a 2-byte and a 1-byte store. A 7-byte key literal is one 8-byte store. The pointer then advances by the true length, and the size pass's slack absorbs the extra bytes.

Key literals longer than 8 bytes copy 16 bytes, or a multiple of 32, through the plain wrapper. That size is still a compile-time constant. The only copies whose length is known only at runtime are off the common path: escape sequences inside strings (only when an escapable byte is found), raw JSON passthrough, and the memset for indents deeper than 8 levels.

### Booleans: one subtraction, one store

A `bool` becomes JSON with no branch and no table lookup. Two 64-bit constants are built at compile time, each in the native byte order:

- `falseVInt` holds the bytes `false` packed into one word (435728179558, `0x65736C6166` on little-endian).
- `trueVInt` holds the difference between the `false` word and the `true` word (434025983730), not the `true` word itself.

The writer computes `state = falseVInt - value × trueVInt`. When `value` is 0 the result is the `false` word. When it is 1 the difference cancels and the `true` word is left. One 8-byte store writes it, and the pointer advances by `5 - value`, so `true` moves 4 bytes and `false` moves 5. The extra bytes land in the slack the size pass reserved (5 bytes per bool plus 64 at the end) and are overwritten by whatever comes next.

On the Bool test, which serializes individual bools one at a time in a loop, this writer runs at 1,145–2,364 MB/s freshly allocated on all five builds, 8.81–46.86× Glaze's 24–208 MB/s. simdjson's reflection writer runs 255 MB/s on Linux / GCC and 214 MB/s on macOS / GCC, so the lead over it is 8.82× and 9.03×. With the string reused, the reflection writer closes most of that gap (3,275 against Jsonifier's 3,389 MB/s, 1.04×, on Linux / GCC and 2,335 against 3,206 MB/s, 1.37×, on macOS / GCC), which puts most of its freshly allocated cost in allocation rather than in the write.

### Strings: picked by length

`string_serializer::impl` sorts every string into one of three length classes before it touches a byte. In all three the escapable bytes are the same: `"`, `\` and anything below 0x20.

1. **Under 16 bytes: one page-safe vector.** If the source does not sit within 16 bytes of the end of a 4 KiB page, `pageSafeShortImpl` loads a full 16-byte vector, masks its escape bits to the string's true length and, when nothing is set, stores all 16 bytes and advances by the length. A short clean key or value is one load, one test and one store, with no branch on its length. Reading past the string's end cannot fault, because the page check guarantees the 16 bytes are mapped; under ASan the load moves into a `JSONIFIER_NO_SANITIZE_ADDRESS` helper so the sanitizer does not flag it. Near a page end the string falls back to two overlapping 4- or 2-byte loads (`smallImpl`) below 8 bytes, or SWAR on 64-bit words (`swarFinish`) from 8 to 15.
2. **16 to 64 bytes: head and tail.** `mediumImpl` loads one vector from the start of the string and one ending at its last byte, tests both and, when both are clean, stores both. The two overlap in the middle, so any length in the range is covered with no loop and no remainder: 16-byte vectors up to 32 bytes, then 32-byte vectors on AVX2, or two 16-byte head-and-tail pairs over 32-byte halves where the widest register is 16 bytes. A string with any escapable byte returns `false` and takes the general path.
3. **Everything else: copy first, check second.** The general path walks the widest available SIMD width down to the narrowest, storing each block unconditionally and testing it afterwards. A clean block advances the full width; a dirty one advances to the first escapable byte, writes its escape from a 256-entry table and resumes. The last partial block is one 16-byte load ending at the string's last byte (`overlappedFinish`), shifted down to the bytes not yet written.

### Integers: exact length first, then 4 digits per store

Integers go through `to_chars` in `i_to_str.hpp` in three steps.

1. **Width.** In `serialize_impl`, every integer narrower than 64 bits is widened to `uint64_t` or `int64_t`, so one writer per signedness covers all eight integer types.
2. **Sign.** A negative value writes `'-'` and then takes its absolute value without a branch: `(v ^ (v >> 63)) - (v >> 63)` on the unsigned type. Because the result is unsigned, `INT64_MIN` works too.
3. **Length.** A balanced tree of comparisons against powers of ten picks the exact digit count, 1 to 20, in at most 5 compares. Each count has its own fixed writer (`to_chars_internal<v_type, N>`), so the writer knows at compile time where every digit goes and never reverses or shifts its output.

The writers use three lookup tables built at compile time: 100 two-digit entries, 1,000 three-digit entries and 10,000 four-digit entries, each digit group pre-packed as characters. Writing four digits is one 4-byte copy from the 10,000-entry table.

Divisions are replaced by multiplies:

- **Divide by 10^4** inside an 8-digit block: multiply by 3518437209 and shift right 45.
- **Divide by 10^8** for longer values: take the high bits of a 128-bit product with 12379400392853802749 and shift right 90 in total. GCC and Clang use `__uint128_t`, MSVC uses `_umul128`, and other compilers fall back to a portable `mulhi`.

So a 20-digit `uint64_t` is two 10^8 splits, four 10^4 splits and five 4-byte stores, with no loop and no per-digit work. A 3-digit group, as at the front of a 19-digit value, is one 4-byte table load followed by a 2-byte and a 1-byte store, so nothing is written past the 3 digits.

On the Int64 and Uint64 tests, which serialize individual values one at a time in a loop, Jsonifier is ahead of Glaze freshly allocated on every build: Int64 by 1.14× on Linux / GCC, 1.20× on Linux / Clang, 1.38× on MSVC and macOS / GCC and 5.66× on macOS / Clang, and Uint64 by 1.03× on Linux / Clang and Linux / GCC, 1.39× on macOS / GCC, 1.48× on MSVC and 5.18× on macOS / Clang. simdjson's reflection writer is ahead of it freshly allocated on Int64 on both GCC builds (992 against 941 MB/s on Linux / GCC, 746 against 691 on macOS / GCC), while Jsonifier takes Uint64 on both (917 against 899 on Linux / GCC and 685 against 669 on macOS / GCC, 1.02× each). With the string reused, Jsonifier is ahead of the fastest other library on both tests on every build, by 1.06× (Uint64, macOS / GCC) to 4.73× (Uint64, MSVC).

### Floats

Doubles go through `zmij::detail::write`, a shortest-round-trip formatter vendored in `zmij.hpp`.

## Minified and prettified output

`serialize_options` carries `prettify`, `indentSize` (default 3), `indentChar` (default space) and `prescanStrings` (default false) as template parameters, so the two modes compile to separate code with no runtime mode check.

| Piece | Minified | Prettified |
| --- | --- | --- |
| Member header | `"key":` packed store | `"key": ` packed store |
| Separator | one `,` byte | `indent_table<",\n">` blit |
| Open / close | one `{` / `}` byte | `"{\n"` + indent, `"\n"` + indent, then `}` |
| Size estimate per member | `key + 3` | `key + 4`, plus `2 + indent` per separator |

`indent_table` prebuilds, at compile time, the prefix (`,\n`, `{\n` or `\n`) followed by 8 levels of indent characters as a packed `uint64_t` array. A newline plus indent is a short run of 8-byte stores from that table, rounded up into the slack. Nesting deeper than 8 levels falls back to `memset` for the remainder, marked `[[unlikely]]`.

This prettify path is the direct one, used when serializing an object. Prettifying existing JSON text is a separate path (`prettifier.hpp`) built on the stage-1 structural index described in *Two Stages, On Demand*.

## Performance results

Jsonifier was fastest in 230 of the 235 write tests on five builds, tied 1 and lost 4. With the output string reused across iterations it was fastest in 232 of 235, tied 1 and lost 2. Each build runs 47 write tests: five single-value tests, ten corpus documents in minified and prettified form, the same ten documents as "Small" cut-down copies (at most 5 KiB minified) in both forms, and the Minify and Prettify reformatting tests. Jsonifier wins every full-size document test on every build in both runs apart from one reused tie, minified Canada on macOS / Clang. All four freshly allocated losses are single numeric values against simdjson's reflection writer, which exists only on GCC: Double and Int64 on Linux / GCC and on macOS / GCC. The one freshly allocated tie is minified Mesh Small on macOS / GCC. Reused, the two losses are String on macOS / GCC, against the reflection writer, and the Minify reformatting test on macOS / Clang, against Glaze. Windows / MSVC, Linux / Clang and macOS / Clang win all 47 tests freshly allocated, and Windows / MSVC, Linux / Clang and Linux / GCC win all 47 reused. Prettified documents are where the lead is widest: 1.42–10.56× over Glaze freshly allocated across the five builds, and 26–63× over the reflection writer on the two GCC builds.

These results come from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 16, 2026, with simdjson [9c00353](https://github.com/simdjson/simdjson/commit/9c00353) and BenchmarkSuite [ae63a25](https://github.com/nihilai-collective/benchmarksuite/commit/ae63a25) on every build. Jsonifier is [3b56371](https://github.com/nihilai-collective/jsonifier/commit/3b56371) on four builds and [06bd73a](https://github.com/nihilai-collective/jsonifier/commit/06bd73a) on macOS / Clang. Glaze is [5520fb0](https://github.com/stephenberry/glaze/commit/5520fb0) on the Linux and macOS builds and [e1ab988](https://github.com/stephenberry/glaze/commit/e1ab988) on Windows / MSVC. Every test runs twice: once with a freshly allocated output string per iteration, and once with a reused one (see Method). Sampling and tie rules are listed under Method below and match those in *Two Stages, On Demand*. `simdjson (reflection)` is simdjson 5's C++26 `to_json`. It only runs on GCC, which has P2996 reflection. Its writer has no single-pass pretty mode, and its prettified results appear in the tables at a small fraction of the minified throughput. The Minify and Prettify tests reformat existing JSON text instead of serializing objects; they compare Jsonifier against Glaze and simdjson On Demand and are counted with the rest.

### Method

**Hardware and builds.**

| Build | CPU | OS | Jsonifier | Glaze (string escape / float write) | simdjson |
| --- | --- | --- | --- | --- | --- |
| Linux / Clang 24.0 | Intel Core i9-14900KF | Linux 7.0.0-38 | AVX2 | AVX2 / SSE4.1 | haswell |
| Linux / GCC 16.2 | Intel Core i9-14900KF | Linux 7.0.0-38 | AVX2 | AVX2 / SSE4.1 | haswell |
| macOS / Clang 23.1 | Apple M1 (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| macOS / GCC 16.2 | Apple M1 (virtual) | macOS 25.6.0 | NEON | NEON / NEON | arm64 |
| Windows / MSVC 19.44 | Intel Core i9-14900KF | Windows 10.0.26200 | AVX2 | AVX2 / SSE4.1 | haswell |

Each library picks its own instruction set at build or run time; the table lists what produced these results. Glaze reports a backend per subsystem, so its string escaper and float writer can differ within one build.

**What is timed.** Every test runs twice. In the freshly allocated run, each iteration constructs a new output string, serializes into it, and destroys it, all inside the timed region, so allocation and deallocation are part of every measurement. In the reused run, labelled "(Reused)" in the sweep, the string is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the serialize work is measured. Both runs are tabulated below, freshly allocated first. The "Small" documents are cut-down copies of each corpus document, at most 5 KiB minified, and measure per-call overhead more than steady-state throughput. CPU caches are cleared before iterations. All libraries serialize the same test data.

**Throughput.** MB/s counts output bytes: each iteration is credited with the size of the JSON string that library produced, and throughput is total bytes over total time in the kept epoch window. MB here means 2²⁰ bytes. Output sizes can differ slightly between libraries for the same data, because each formats floats in its own way; on the Double test, for example, Glaze writes 1,798 bytes, Jsonifier 1,811 and simdjson's reflection writer 1,997. Each library is credited with its own output size.

**Sampling.**

1. Iterations start at 100 and double each epoch (100, 200, 400, …) up to 100,000.
2. Each epoch runs all its iterations and evaluates a trailing window of max(iterations / 10, 30) samples, capped at 100,000.
3. Sampling never stops early: epochs continue until 5 seconds have elapsed or the iteration cap is reached.
4. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result.

**Convergence.** A kept epoch counts as converged only if its RSE is under 5% and its mean shift under 2.5% on the three x86 builds, or under 10% and 5% on the virtualized M1. Non-converged results are included in the rankings and in the win, tie and loss counts. Each one is marked n/c in the tables below, and a test with a non-converged result should be read with reduced statistical confidence.

**Ranking.** Variance is Bessel-corrected. Two libraries tie when Welch's t-test (two-sided, p < 0.05) cannot separate their kept epochs. Jsonifier is counted as a win when it ranks first and no other library is statistically tied with it, as a tie when it is statistically tied with another library at the top, and as a loss when another library ranks above it. Win, tie and loss counts include non-converged results, which are marked n/c.

**Caveats.** Keeping the quietest epoch favours each library's least-disturbed stretch, which raises absolute throughput somewhat, but the rule applies identically to every library. The M1 numbers were taken in a virtual machine, so absolute MB/s may differ from bare metal; within a build, every library ran in the same environment.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 47 | 45 | 0 | 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 47 | 44 | 1 | 2 |
| macOS / Clang 23.1 (Apple M1, NEON) | 47 | 47 | 0 | 0 |
| **Total** | **235** | **230** | **1** | **4** |

Every write test per build follows, in MB/s, freshly allocated run first and reused run after it. Results marked n/c did not converge. Bool, Double, Int64, String and Uint64 serialize individual values one at a time in a loop. The rest serialize whole documents, and each build's second table holds the two reformatting tests. *Lead* is Jsonifier's throughput divided by the fastest other library's, so a value under 1 is a loss.

### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,145 | 24 | 46.86× |
| Double | 203 | 138 | 1.47× |
| Int64 | 518 | 376 | 1.38× |
| String | 1,155 | 890 | 1.30× |
| Uint64 | 552 | 372 | 1.48× |
| Canada (minified) | 1,339 | 837 | 1.60× |
| Canada (prettified) | 3,312 | 1,498 | 2.21× |
| CitmCatalog (minified) | 7,208 | 4,436 | 1.62× |
| CitmCatalog (prettified) | 4,656 | 2,047 | 2.27× |
| Discord (minified) | 7,157 | 4,163 | 1.72× |
| Discord (prettified) | 10,509 | 4,477 | 2.35× |
| Google Maps Response (minified) | 6,716 | 3,452 | 1.95× |
| Google Maps Response (prettified) | 13,941 | 5,411 | 2.58× |
| Instruments (minified) | 7,615 | 4,058 | 1.88× |
| Instruments (prettified) | 13,625 | 4,569 | 2.98× |
| Marine IK Reverse (minified) | 885 | 681 | 1.30× |
| Marine IK Reverse (prettified) | 3,182 | 1,885 | 1.69× |
| Marine IK (minified) | 897 | 602 | 1.49× |
| Marine IK (prettified) | 3,119 | 1,292 | 2.41× |
| Mesh (minified) | 1,441 | 920 | 1.57× |
| Mesh (prettified) | 2,381 | 1,522 | 1.56× |
| Random (minified) | 3,227 | 2,865 | 1.13× |
| Random (prettified) | 4,472 | 2,289 | 1.95× |
| Twitter (minified) | 8,248 | 5,006 | 1.65× |
| Twitter (prettified) | 11,146 | 3,721 | 3.00× |
| Canada Small (minified) | 1,868 | 1,098 | 1.70× |
| Canada Small (prettified) | 5,739 | 2,978 | 1.93× |
| CitmCatalog Small (minified) | 6,092 | 3,624 | 1.68× |
| CitmCatalog Small (prettified) | 11,086 | 4,560 | 2.43× |
| Discord Small (minified) | 5,934 | 3,544 | 1.67× |
| Discord Small (prettified) | 7,160 | 3,697 | 1.94× |
| Google Maps Response Small (minified) | 5,981 | 3,490 | 1.71× |
| Google Maps Response Small (prettified) | 14,250 | 5,055 | 2.82× |
| Instruments Small (minified) | 5,566 | 2,853 (n/c) | 1.95× |
| Instruments Small (prettified) | 11,250 | 3,672 (n/c) | 3.06× |
| Marine IK Reverse Small (minified) | 1,519 | 926 | 1.64× |
| Marine IK Reverse Small (prettified) | 6,141 | 2,917 | 2.11× |
| Marine IK Small (minified) | 1,506 | 934 | 1.61× |
| Marine IK Small (prettified) | 6,457 | 2,889 | 2.24× |
| Mesh Small (minified) | 1,855 | 1,203 | 1.54× |
| Mesh Small (prettified) | 3,617 | 1,943 | 1.86× |
| Random Small (minified) | 5,840 | 3,226 | 1.81× |
| Random Small (prettified) | 9,917 | 4,212 | 2.35× |
| Twitter Small (minified) | 5,381 | 4,557 | 1.18× |
| Twitter Small (prettified) | 7,945 | 3,260 | 2.44× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,172 | 1,932 | 1,137 | 1.64× |
| Prettify | 2,418 | 2,368 | 927 | 1.02× |

### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,364 | 190 | 12.45× |
| Double | 341 | 306 | 1.11× |
| Int64 | 933 | 775 | 1.20× |
| String | 4,389 | 2,014 | 2.18× |
| Uint64 | 904 | 877 | 1.03× |
| Canada (minified) | 1,618 | 964 | 1.68× |
| Canada (prettified) | 4,680 | 3,300 | 1.42× |
| CitmCatalog (minified) | 10,389 | 5,663 | 1.83× |
| CitmCatalog (prettified) | 17,905 | 5,819 | 3.08× |
| Discord (minified) | 13,903 | 5,919 | 2.35× |
| Discord (prettified) | 17,624 | 5,031 | 3.50× |
| Google Maps Response (minified) | 11,118 | 4,019 | 2.77× |
| Google Maps Response (prettified) | 18,597 | 5,014 | 3.71× |
| Instruments (minified) | 18,144 | 5,052 | 3.59× |
| Instruments (prettified) | 22,644 | 5,724 | 3.96× |
| Marine IK Reverse (minified) | 843 | 616 | 1.37× |
| Marine IK Reverse (prettified) | 6,158 | 3,699 | 1.66× |
| Marine IK (minified) | 891 | 602 | 1.48× |
| Marine IK (prettified) | 6,216 | 3,076 | 2.02× |
| Mesh (minified) | 1,288 | 1,038 | 1.24× |
| Mesh (prettified) | 4,103 | 1,776 | 2.31× |
| Random (minified) | 14,463 | 3,507 | 4.12× |
| Random (prettified) | 18,016 | 3,999 | 4.50× |
| Twitter (minified) | 18,862 | 5,945 | 3.17× |
| Twitter (prettified) | 22,760 | 5,172 | 4.40× |
| Canada Small (minified) | 1,612 | 974 | 1.66× |
| Canada Small (prettified) | 5,028 | 3,456 | 1.46× |
| CitmCatalog Small (minified) | 8,884 | 3,136 | 2.83× |
| CitmCatalog Small (prettified) | 18,166 | 3,733 | 4.87× |
| Discord Small (minified) | 9,926 | 3,820 | 2.60× |
| Discord Small (prettified) | 14,245 | 3,122 | 4.56× |
| Google Maps Response Small (minified) | 11,370 | 3,798 | 2.99× |
| Google Maps Response Small (prettified) | 22,179 | 4,776 | 4.64× |
| Instruments Small (minified) | 12,750 | 3,545 | 3.60× |
| Instruments Small (prettified) | 22,591 | 4,272 | 5.29× |
| Marine IK Reverse Small (minified) | 1,336 | 797 | 1.68× |
| Marine IK Reverse Small (prettified) | 7,197 | 3,951 | 1.82× |
| Marine IK Small (minified) | 1,422 | 829 | 1.71× |
| Marine IK Small (prettified) | 7,915 | 3,894 | 2.03× |
| Mesh Small (minified) | 1,277 | 964 | 1.32× |
| Mesh Small (prettified) | 4,277 | 1,627 | 2.63× |
| Random Small (minified) | 12,607 | 3,566 | 3.54× |
| Random Small (prettified) | 19,998 | 4,045 | 4.94× |
| Twitter Small (minified) | 8,484 | 4,415 | 1.92× |
| Twitter Small (prettified) | 11,175 | 4,112 | 2.72× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,590 | 2,911 | 1,329 | 1.23× |
| Prettify | 4,450 | 4,005 | 1,478 | 1.11× |

### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 2,249 | 208 | 255 | 8.81× |
| Double | 351 | 307 | 353 | 1.00× |
| Int64 | 941 | 828 | 992 | 0.95× |
| String | 3,947 | 2,057 | 3,760 | 1.05× |
| Uint64 | 917 | 892 | 899 | 1.02× |
| Canada (minified) | 2,135 | 1,467 | 1,054 | 1.46× |
| Canada (prettified) | 6,194 | 3,307 | 183 | 1.87× |
| CitmCatalog (minified) | 12,843 | 6,177 | 11,361 | 1.13× |
| CitmCatalog (prettified) | 18,863 | 6,086 | 540 | 3.10× |
| Discord (minified) | 13,827 | 6,306 | 11,308 | 1.22× |
| Discord (prettified) | 18,726 | 6,218 | 547 | 3.01× |
| Google Maps Response (minified) | 11,597 | 4,231 | 10,149 | 1.14× |
| Google Maps Response (prettified) | 18,601 | 6,910 | 546 | 2.69× |
| Instruments (minified) | 19,493 | 5,765 | 16,086 | 1.21× |
| Instruments (prettified) | 24,370 | 6,191 | 506 | 3.94× |
| Marine IK Reverse (minified) | 1,313 | 1,003 | 895 | 1.31× |
| Marine IK Reverse (prettified) | 6,680 | 3,754 | 196 | 1.78× |
| Marine IK (minified) | 1,333 | 970 | 897 | 1.37× |
| Marine IK (prettified) | 6,682 | 3,159 | 196 | 2.12× |
| Mesh (minified) | 2,423 | 1,614 | 1,282 | 1.50× |
| Mesh (prettified) | 4,533 | 2,656 | 172 | 1.71× |
| Random (minified) | 13,810 | 3,669 | 11,633 | 1.19× |
| Random (prettified) | 18,493 | 4,846 | 528 | 3.82× |
| Twitter (minified) | 17,847 | 6,721 | 13,010 | 1.37× |
| Twitter (prettified) | 21,877 | 4,904 | 528 | 4.46× |
| Canada Small (minified) | 2,020 | 1,551 | 1,053 | 1.30× |
| Canada Small (prettified) | 5,591 | 3,464 | 198 | 1.61× |
| CitmCatalog Small (minified) | 8,983 | 3,665 | 6,908 | 1.30× |
| CitmCatalog Small (prettified) | 17,179 | 4,248 | 549 | 4.04× |
| Discord Small (minified) | 9,591 | 4,091 | 6,770 | 1.42× |
| Discord Small (prettified) | 13,686 | 3,953 | 534 | 3.46× |
| Google Maps Response Small (minified) | 10,122 | 3,835 | 8,033 | 1.26× |
| Google Maps Response Small (prettified) | 19,768 | 5,603 | 537 | 3.53× |
| Instruments Small (minified) | 14,073 | 3,712 | 7,745 | 1.82× |
| Instruments Small (prettified) | 25,961 | 3,993 | 527 | 6.50× |
| Marine IK Reverse Small (minified) | 1,796 | 1,191 | 1,474 | 1.22× |
| Marine IK Reverse Small (prettified) | 8,057 | 3,624 | 334 | 2.22× |
| Marine IK Small (minified) | 1,839 | 1,210 | 1,474 | 1.25× |
| Marine IK Small (prettified) | 8,255 | 3,898 | 323 | 2.12× |
| Mesh Small (minified) | 1,734 | 1,300 | 1,339 | 1.30× |
| Mesh Small (prettified) | 4,402 | 2,096 | 195 | 2.10× |
| Random Small (minified) | 10,757 | 3,961 | 7,623 | 1.41× |
| Random Small (prettified) | 20,195 | 4,736 | 585 | 4.26× |
| Twitter Small (minified) | 7,833 | 5,376 | 6,037 | 1.30× |
| Twitter Small (prettified) | 11,129 | 3,127 | 519 | 3.56× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,217 | 2,412 | 1,342 | 1.33× |
| Prettify | 4,542 | 3,668 | 1,489 | 1.24× |

### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 1,933 | 118 | 214 | 9.02× |
| Double | 262 | 201 | 283 | 0.93× |
| Int64 | 691 | 502 | 746 | 0.93× |
| String | 1,209 | 1,015 | 1,168 | 1.04× |
| Uint64 | 685 | 492 | 669 | 1.02× |
| Canada (minified) | 2,057 | 1,395 | 658 | 1.47× |
| Canada (prettified) | 6,251 | 2,801 | 99 (n/c) | 2.23× |
| CitmCatalog (minified) | 8,072 | 1,863 | 6,937 | 1.16× |
| CitmCatalog (prettified) | 18,806 | 3,597 | 311 | 5.23× |
| Discord (minified) | 11,500 | 2,551 | 8,805 | 1.31× |
| Discord (prettified) | 12,671 | 2,502 | 340 | 5.06× |
| Google Maps Response (minified) | 8,418 | 1,529 | 7,112 | 1.18× |
| Google Maps Response (prettified) | 15,781 | 2,531 (n/c) | 297 | 6.23× |
| Instruments (minified) | 12,661 | 1,682 | 7,186 | 1.76× |
| Instruments (prettified) | 14,562 | 2,030 | 288 | 7.17× |
| Marine IK Reverse (minified) | 1,017 | 704 (n/c) | 656 (n/c) | 1.44× |
| Marine IK Reverse (prettified) | 5,169 | 2,407 (n/c) | 108 (n/c) | 2.15× |
| Marine IK (minified) | 1,244 | 691 (n/c) | 660 | 1.80× |
| Marine IK (prettified) | 4,987 (n/c) | 1,735 | 123 (n/c) | 2.87× |
| Mesh (minified) | 2,256 | 995 | 837 | 2.27× |
| Mesh (prettified) | 3,507 | 1,252 | 68 (n/c) | 2.80× |
| Random (minified) | 7,560 (n/c) | 1,324 (n/c) | 6,963 | 1.09× |
| Random (prettified) | 11,634 | 2,269 | 269 | 5.13× |
| Twitter (minified) | 12,391 | 3,186 | 8,743 | 1.42× |
| Twitter (prettified) | 12,309 | 1,443 | 375 | 8.53× |
| Canada Small (minified) | 2,078 | 1,414 | 697 | 1.47× |
| Canada Small (prettified) | 5,033 | 2,400 | 95 | 2.10× |
| CitmCatalog Small (minified) | 7,529 | 1,571 | 6,591 | 1.14× |
| CitmCatalog Small (prettified) | 16,273 | 2,709 | 324 | 6.01× |
| Discord Small (minified) | 8,889 | 2,213 | 6,934 | 1.28× |
| Discord Small (prettified) | 10,420 (n/c) | 2,112 | 327 | 4.93× |
| Google Maps Response Small (minified) | 8,159 | 1,503 | 7,250 | 1.13× |
| Google Maps Response Small (prettified) | 16,044 | 2,501 | 264 (n/c) | 6.42× |
| Instruments Small (minified) | 11,466 | 1,474 | 6,047 | 1.90× |
| Instruments Small (prettified) | 13,487 | 1,786 | 281 | 7.55× |
| Marine IK Reverse Small (minified) | 1,579 | 761 | 1,477 | 1.07× |
| Marine IK Reverse Small (prettified) | 6,954 | 2,303 | 129 (n/c) | 3.02× |
| Marine IK Small (minified) | 1,484 | 829 | 1,300 | 1.14× |
| Marine IK Small (prettified) | 6,329 | 3,007 | 158 | 2.10× |
| Mesh Small (minified) | 638 | 699 | 995 | 0.64× |
| Mesh Small (prettified) | 3,712 | 1,277 | 87 | 2.91× |
| Random Small (minified) | 9,072 | 1,543 | 7,215 | 1.26× |
| Random Small (prettified) | 12,486 | 2,202 | 279 | 5.67× |
| Twitter Small (minified) | 8,220 (n/c) | 2,691 | 6,398 (n/c) | 1.28× |
| Twitter Small (prettified) | 11,874 | 1,374 | 304 | 8.64× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,863 | 1,425 | 854 | 1.31× |
| Prettify | 2,820 | 1,624 | 592 | 1.74× |

### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,829 | 115 | 15.84× |
| Double | 261 | 186 | 1.40× |
| Int64 | 2,563 | 452 | 5.66× |
| String | 983 | 950 | 1.04× |
| Uint64 | 2,385 | 460 | 5.18× |
| Canada (minified) | 1,973 | 1,401 | 1.41× |
| Canada (prettified) | 5,778 | 2,605 | 2.22× |
| CitmCatalog (minified) | 6,088 | 1,570 | 3.88× |
| CitmCatalog (prettified) | 10,585 | 2,035 (n/c) | 5.20× |
| Discord (minified) | 9,049 | 2,217 | 4.08× |
| Discord (prettified) | 16,248 | 2,515 | 6.46× |
| Google Maps Response (minified) | 7,925 | 1,418 | 5.59× |
| Google Maps Response (prettified) | 16,956 | 2,735 | 6.20× |
| Instruments (minified) | 10,012 | 1,339 | 7.48× |
| Instruments (prettified) | 20,145 | 1,907 | 10.56× |
| Marine IK Reverse (minified) | 1,262 | 756 | 1.67× |
| Marine IK Reverse (prettified) | 5,898 | 2,649 (n/c) | 2.23× |
| Marine IK (minified) | 1,278 | 615 (n/c) | 2.08× |
| Marine IK (prettified) | 5,549 (n/c) | 2,408 (n/c) | 2.30× |
| Mesh (minified) | 2,450 | 953 | 2.57× |
| Mesh (prettified) | 4,333 | 1,582 | 2.74× |
| Random (minified) | 8,338 | 1,672 | 4.99× |
| Random (prettified) | 14,229 | 1,886 (n/c) | 7.54× |
| Twitter (minified) | 12,866 | 3,381 | 3.81× |
| Twitter (prettified) | 15,833 | 3,577 | 4.43× |
| Canada Small (minified) | 1,943 (n/c) | 1,310 | 1.48× |
| Canada Small (prettified) | 6,350 | 2,336 | 2.72× |
| CitmCatalog Small (minified) | 7,483 | 1,614 | 4.64× |
| CitmCatalog Small (prettified) | 14,891 | 2,038 | 7.31× |
| Discord Small (minified) | 9,502 | 2,250 | 4.22× |
| Discord Small (prettified) | 13,177 | 2,321 | 5.68× |
| Google Maps Response Small (minified) | 6,891 | 1,285 | 5.36× |
| Google Maps Response Small (prettified) | 16,327 | 2,348 | 6.95× |
| Instruments Small (minified) | 11,278 | 1,584 | 7.12× |
| Instruments Small (prettified) | 21,169 | 2,078 | 10.18× |
| Marine IK Reverse Small (minified) | 1,636 | 877 | 1.87× |
| Marine IK Reverse Small (prettified) | 6,734 | 2,404 | 2.80× |
| Marine IK Small (minified) | 1,719 | 874 | 1.97× |
| Marine IK Small (prettified) | 6,930 | 2,780 | 2.49× |
| Mesh Small (minified) | 2,143 | 898 | 2.39× |
| Mesh Small (prettified) | 4,386 | 1,369 | 3.20× |
| Random Small (minified) | 8,437 | 1,407 | 6.00× |
| Random Small (prettified) | 15,381 | 1,956 | 7.86× |
| Twitter Small (minified) | 9,259 | 2,647 | 3.50× |
| Twitter Small (prettified) | 12,531 | 2,604 | 4.81× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 2,087 | 1,429 | 596 | 1.46× |
| Prettify | 3,295 | 2,228 | 448 | 1.48× |

### Reused output strings

With the string's capacity kept across iterations, Jsonifier is fastest in 232 of the 235 write tests, ties 1 and loses 2: String on macOS / GCC, 8,603 vs the reflection writer's 9,684 MB/s (0.89×), and Minify on macOS / Clang, 982 vs Glaze's 1,415 MB/s (0.69×), where neither result converged. The tie is minified Canada on macOS / Clang, where Jsonifier's result did not converge. Thirty-three tests across the five builds have at least one non-converged result (twenty freshly allocated, thirteen reused): twenty-nine on the two macOS builds and four on Windows / MSVC, where Glaze did not converge on Instruments Small in both forms freshly allocated and on minified CitmCatalog Small reused, and Jsonifier did not converge on prettified Instruments reused. Each is marked n/c in the tables. Twelve of them are Jsonifier's own, all wins except the reused minified Canada tie and Minify loss on macOS / Clang.

| Platform / compiler | Write tests | Jsonifier fastest | Tied | Jsonifier lost |
| --- | --- | --- | --- | --- |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 47 | 47 | 0 | 0 |
| macOS / GCC 16.2 (Apple M1, NEON) | 47 | 46 | 0 | 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 47 | 45 | 1 | 1 |
| **Total** | **235** | **232** | **1** | **2** |

#### Windows / MSVC 19.44 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 1,761 | 267 | 6.59× |
| Double | 791 | 315 | 2.51× |
| Int64 | 3,832 | 1,020 | 3.76× |
| String | 7,803 | 2,382 | 3.28× |
| Uint64 | 4,902 | 1,036 | 4.73× |
| Canada (minified) | 1,706 | 1,052 | 1.62× |
| Canada (prettified) | 5,228 | 3,076 | 1.70× |
| CitmCatalog (minified) | 7,335 | 4,998 | 1.47× |
| CitmCatalog (prettified) | 14,776 | 7,509 | 1.97× |
| Discord (minified) | 7,223 | 4,923 | 1.47× |
| Discord (prettified) | 10,900 | 5,220 | 2.09× |
| Google Maps Response (minified) | 7,345 | 4,086 | 1.80× |
| Google Maps Response (prettified) | 14,829 | 6,014 | 2.47× |
| Instruments (minified) | 7,970 | 4,642 | 1.72× |
| Instruments (prettified) | 13,384 (n/c) | 5,476 | 2.44× |
| Marine IK Reverse (minified) | 1,005 | 734 | 1.37× |
| Marine IK Reverse (prettified) | 4,798 | 3,262 | 1.47× |
| Marine IK (minified) | 1,017 | 749 | 1.36× |
| Marine IK (prettified) | 4,801 | 3,273 | 1.47× |
| Mesh (minified) | 1,758 | 1,274 | 1.38× |
| Mesh (prettified) | 3,318 | 2,158 | 1.54× |
| Random (minified) | 6,732 | 3,108 | 2.17× |
| Random (prettified) | 11,169 | 4,630 | 2.41× |
| Twitter (minified) | 8,543 | 5,709 | 1.50× |
| Twitter (prettified) | 11,183 | 4,278 | 2.61× |
| Canada Small (minified) | 2,000 | 1,202 | 1.66× |
| Canada Small (prettified) | 6,433 | 3,441 | 1.87× |
| CitmCatalog Small (minified) | 6,463 | 4,455 (n/c) | 1.45× |
| CitmCatalog Small (prettified) | 13,193 | 6,217 | 2.12× |
| Discord Small (minified) | 6,219 | 4,784 | 1.30× |
| Discord Small (prettified) | 8,554 | 4,851 | 1.76× |
| Google Maps Response Small (minified) | 6,808 | 4,099 | 1.66× |
| Google Maps Response Small (prettified) | 16,969 | 6,511 | 2.61× |
| Instruments Small (minified) | 7,121 | 4,745 | 1.50× |
| Instruments Small (prettified) | 11,905 | 5,636 | 2.11× |
| Marine IK Reverse Small (minified) | 1,572 | 995 | 1.58× |
| Marine IK Reverse Small (prettified) | 6,459 | 3,407 | 1.90× |
| Marine IK Small (minified) | 1,594 | 1,003 | 1.59× |
| Marine IK Small (prettified) | 6,759 | 3,532 | 1.91× |
| Mesh Small (minified) | 1,663 | 1,281 | 1.30× |
| Mesh Small (prettified) | 3,181 | 2,223 | 1.43× |
| Random Small (minified) | 6,687 | 3,868 | 1.73× |
| Random Small (prettified) | 11,980 | 5,280 | 2.27× |
| Twitter Small (minified) | 7,247 | 5,066 | 1.43× |
| Twitter Small (prettified) | 9,248 | 5,558 | 1.66× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,168 | 1,931 | 1,133 | 1.64× |
| Prettify | 3,760 | 2,387 | 920 | 1.58× |

#### Linux / Clang 24.0 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 5,111 | 429 | 11.91× |
| Double | 866 | 706 | 1.23× |
| Int64 | 5,091 | 2,611 | 1.95× |
| String | 12,062 | 5,567 | 2.17× |
| Uint64 | 5,231 | 2,534 | 2.06× |
| Canada (minified) | 1,612 | 1,007 | 1.60× |
| Canada (prettified) | 4,680 | 3,916 | 1.19× |
| CitmCatalog (minified) | 10,452 | 6,668 | 1.57× |
| CitmCatalog (prettified) | 17,931 | 7,849 | 2.28× |
| Discord (minified) | 14,268 | 6,088 | 2.34× |
| Discord (prettified) | 17,960 | 5,721 | 3.14× |
| Google Maps Response (minified) | 11,895 | 4,470 | 2.66× |
| Google Maps Response (prettified) | 19,339 | 5,543 | 3.49× |
| Instruments (minified) | 18,297 | 6,391 | 2.86× |
| Instruments (prettified) | 22,634 | 6,864 | 3.30× |
| Marine IK Reverse (minified) | 847 | 619 | 1.37× |
| Marine IK Reverse (prettified) | 6,173 | 4,067 | 1.52× |
| Marine IK (minified) | 893 | 631 | 1.42× |
| Marine IK (prettified) | 6,273 | 4,112 | 1.53× |
| Mesh (minified) | 1,295 | 1,076 | 1.20× |
| Mesh (prettified) | 4,097 | 1,843 | 2.22× |
| Random (minified) | 14,567 | 3,855 | 3.78× |
| Random (prettified) | 17,995 | 4,604 | 3.91× |
| Twitter (minified) | 19,306 | 7,011 | 2.75× |
| Twitter (prettified) | 22,702 | 6,448 | 3.52× |
| Canada Small (minified) | 1,638 | 1,027 | 1.60× |
| Canada Small (prettified) | 5,173 | 3,946 | 1.31× |
| CitmCatalog Small (minified) | 9,573 | 3,927 | 2.44× |
| CitmCatalog Small (prettified) | 18,978 | 4,486 | 4.23× |
| Discord Small (minified) | 11,226 | 4,096 | 2.74× |
| Discord Small (prettified) | 15,759 | 3,637 | 4.33× |
| Google Maps Response Small (minified) | 12,307 | 3,727 | 3.30× |
| Google Maps Response Small (prettified) | 23,974 | 5,323 | 4.50× |
| Instruments Small (minified) | 14,105 | 3,756 | 3.76× |
| Instruments Small (prettified) | 24,554 | 4,720 | 5.20× |
| Marine IK Reverse Small (minified) | 1,364 | 863 | 1.58× |
| Marine IK Reverse Small (prettified) | 7,429 | 4,285 | 1.73× |
| Marine IK Small (minified) | 1,469 | 839 | 1.75× |
| Marine IK Small (prettified) | 8,057 | 4,122 | 1.95× |
| Mesh Small (minified) | 1,332 | 985 | 1.35× |
| Mesh Small (prettified) | 4,481 | 1,741 | 2.57× |
| Random Small (minified) | 14,030 | 3,961 | 3.54× |
| Random Small (prettified) | 23,933 | 4,745 | 5.04× |
| Twitter Small (minified) | 9,666 | 5,239 | 1.85× |
| Twitter Small (prettified) | 12,462 | 4,744 | 2.63× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,583 | 2,872 | 1,331 | 1.25× |
| Prettify | 4,445 | 4,017 | 1,469 | 1.11× |

#### Linux / GCC 16.2 (i9-14900KF, AVX2)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,389 | 686 | 3,275 | 1.04× |
| Double | 887 | 641 | 653 | 1.36× |
| Int64 | 5,393 | 2,590 | 4,142 | 1.30× |
| String | 11,712 | 6,356 | 9,468 | 1.24× |
| Uint64 | 4,971 | 2,641 | 4,460 | 1.11× |
| Canada (minified) | 2,136 | 1,548 | 1,053 | 1.38× |
| Canada (prettified) | 6,197 | 3,915 | 181 | 1.58× |
| CitmCatalog (minified) | 12,924 | 7,175 | 11,308 | 1.14× |
| CitmCatalog (prettified) | 18,910 | 8,221 | 537 | 2.30× |
| Discord (minified) | 14,036 | 6,917 | 11,273 | 1.25× |
| Discord (prettified) | 19,178 | 7,252 | 552 | 2.64× |
| Google Maps Response (minified) | 12,064 | 5,488 | 10,539 | 1.14× |
| Google Maps Response (prettified) | 19,373 | 7,639 | 552 | 2.54× |
| Instruments (minified) | 19,709 | 6,577 | 15,917 | 1.24× |
| Instruments (prettified) | 24,724 | 7,507 | 505 | 3.29× |
| Marine IK Reverse (minified) | 1,318 | 1,018 | 896 | 1.30× |
| Marine IK Reverse (prettified) | 6,687 | 4,156 | 197 | 1.61× |
| Marine IK (minified) | 1,334 | 1,022 | 891 | 1.30× |
| Marine IK (prettified) | 6,674 | 4,180 | 203 | 1.60× |
| Mesh (minified) | 2,402 | 1,755 | 1,279 | 1.37× |
| Mesh (prettified) | 4,602 | 2,851 | 170 | 1.61× |
| Random (minified) | 13,939 | 4,096 | 11,638 | 1.20× |
| Random (prettified) | 18,531 | 5,801 | 519 | 3.19× |
| Twitter (minified) | 17,841 | 7,758 | 12,859 | 1.39× |
| Twitter (prettified) | 21,659 | 5,601 | 528 | 3.87× |
| Canada Small (minified) | 2,051 | 1,580 | 1,062 | 1.30× |
| Canada Small (prettified) | 5,900 | 3,896 | 200 | 1.51× |
| CitmCatalog Small (minified) | 9,598 | 4,217 | 7,261 | 1.32× |
| CitmCatalog Small (prettified) | 20,529 | 4,289 | 552 | 4.79× |
| Discord Small (minified) | 10,448 | 4,736 | 7,211 | 1.45× |
| Discord Small (prettified) | 15,343 | 4,333 | 536 | 3.54× |
| Google Maps Response Small (minified) | 10,792 | 4,164 | 8,737 | 1.24× |
| Google Maps Response Small (prettified) | 23,768 | 6,798 | 541 | 3.50× |
| Instruments Small (minified) | 16,564 | 4,043 | 8,518 | 1.94× |
| Instruments Small (prettified) | 29,399 | 4,768 | 521 | 6.17× |
| Marine IK Reverse Small (minified) | 1,823 | 1,220 | 1,503 | 1.21× |
| Marine IK Reverse Small (prettified) | 8,332 | 4,104 | 333 | 2.03× |
| Marine IK Small (minified) | 1,845 | 1,219 | 1,507 | 1.22× |
| Marine IK Small (prettified) | 8,455 | 4,119 | 317 | 2.05× |
| Mesh Small (minified) | 1,999 | 1,337 | 1,403 | 1.42× |
| Mesh Small (prettified) | 4,649 | 2,298 | 197 | 2.02× |
| Random Small (minified) | 12,741 | 4,551 | 8,131 | 1.57× |
| Random Small (prettified) | 21,983 | 5,772 | 584 | 3.81× |
| Twitter Small (minified) | 9,070 | 6,054 | 6,677 | 1.36× |
| Twitter Small (prettified) | 13,015 | 3,501 | 516 | 3.72× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 3,221 | 2,390 | 1,324 | 1.35× |
| Prettify | 4,578 | 3,666 | 1,516 | 1.25× |

#### macOS / GCC 16.2 (Apple M1, NEON)

| Test | Jsonifier | Glaze | simdjson (reflection) | Lead |
| --- | --- | --- | --- | --- |
| Bool | 3,206 | 812 | 2,335 | 1.37× |
| Double | 815 | 465 | 669 | 1.22× |
| Int64 | 3,686 | 2,096 | 3,018 | 1.22× |
| String | 8,603 | 4,738 | 9,684 | 0.89× |
| Uint64 | 2,958 | 2,299 | 2,792 | 1.06× |
| Canada (minified) | 1,989 | 1,463 | 684 | 1.36× |
| Canada (prettified) | 5,727 | 3,437 | 106 (n/c) | 1.67× |
| CitmCatalog (minified) | 8,089 | 1,987 | 7,133 | 1.13× |
| CitmCatalog (prettified) | 18,915 | 4,254 | 322 | 4.45× |
| Discord (minified) | 12,064 | 2,790 | 9,012 | 1.34× |
| Discord (prettified) | 13,718 | 2,785 | 341 | 4.93× |
| Google Maps Response (minified) | 8,762 | 1,633 | 6,728 | 1.30× |
| Google Maps Response (prettified) | 16,461 | 3,103 | 292 | 5.31× |
| Instruments (minified) | 13,041 | 1,851 | 7,092 | 1.84× |
| Instruments (prettified) | 15,716 | 2,272 | 273 | 6.92× |
| Marine IK Reverse (minified) | 1,133 | 688 (n/c) | 636 | 1.65× |
| Marine IK Reverse (prettified) | 5,413 | 2,659 | 132 (n/c) | 2.04× |
| Marine IK (minified) | 1,145 | 747 | 543 | 1.53× |
| Marine IK (prettified) | 5,221 | 2,400 | 120 (n/c) | 2.18× |
| Mesh (minified) | 2,181 | 997 | 800 | 2.19× |
| Mesh (prettified) | 3,037 | 1,170 | 80 | 2.59× |
| Random (minified) | 8,729 | 1,582 | 6,947 | 1.26× |
| Random (prettified) | 12,461 | 2,397 | 272 | 5.20× |
| Twitter (minified) | 12,991 | 3,726 | 8,938 | 1.45× |
| Twitter (prettified) | 14,481 | 2,813 | 282 (n/c) | 5.15× |
| Canada Small (minified) | 2,115 | 1,232 | 710 | 1.72× |
| Canada Small (prettified) | 6,016 | 2,933 | 101 | 2.05× |
| CitmCatalog Small (minified) | 8,025 | 1,822 | 6,918 | 1.16× |
| CitmCatalog Small (prettified) | 17,668 | 3,120 | 264 | 5.66× |
| Discord Small (minified) | 10,945 (n/c) | 2,747 | 8,434 | 1.30× |
| Discord Small (prettified) | 11,669 | 2,652 | 285 | 4.40× |
| Google Maps Response Small (minified) | 10,110 | 1,684 | 8,070 | 1.25× |
| Google Maps Response Small (prettified) | 20,302 | 2,639 | 293 | 7.69× |
| Instruments Small (minified) | 13,384 | 1,760 | 6,972 | 1.92× |
| Instruments Small (prettified) | 17,094 | 2,238 | 297 | 7.64× |
| Marine IK Reverse Small (minified) | 1,475 | 807 | 1,296 | 1.14× |
| Marine IK Reverse Small (prettified) | 6,281 | 3,004 | 167 | 2.09× |
| Marine IK Small (minified) | 1,525 | 954 | 1,501 | 1.02× |
| Marine IK Small (prettified) | 7,927 | 3,438 | 158 | 2.31× |
| Mesh Small (minified) | 2,185 | 908 | 1,023 | 2.13× |
| Mesh Small (prettified) | 3,435 | 1,414 | 85 | 2.43× |
| Random Small (minified) | 9,852 | 1,796 | 6,945 | 1.42× |
| Random Small (prettified) | 13,287 | 2,414 | 269 | 5.50× |
| Twitter Small (minified) | 11,687 | 3,288 | 7,587 | 1.54× |
| Twitter Small (prettified) | 13,470 | 2,384 | 301 | 5.65× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 1,911 | 1,447 | 530 | 1.32× |
| Prettify | 2,900 | 1,740 | 579 | 1.67× |

#### macOS / Clang 23.1 (Apple M1, NEON)

| Test | Jsonifier | Glaze | Lead |
| --- | --- | --- | --- |
| Bool | 2,503 | 674 | 3.71× |
| Double | 702 | 600 | 1.17× |
| Int64 | 2,853 | 1,968 | 1.45× |
| String | 6,630 | 4,149 | 1.60× |
| Uint64 | 3,219 | 2,058 | 1.56× |
| Canada (minified) | 1,156 (n/c) | 1,461 | 0.79× |
| Canada (prettified) | 6,016 | 3,004 (n/c) | 2.00× |
| CitmCatalog (minified) | 5,006 | 1,334 | 3.75× |
| CitmCatalog (prettified) | 15,738 | 2,154 | 7.31× |
| Discord (minified) | 10,252 | 2,706 | 3.79× |
| Discord (prettified) | 13,907 | 2,731 | 5.09× |
| Google Maps Response (minified) | 8,526 | 1,665 | 5.12× |
| Google Maps Response (prettified) | 18,401 | 2,674 | 6.88× |
| Instruments (minified) | 11,929 | 1,755 | 6.80× |
| Instruments (prettified) | 17,293 | 2,198 | 7.87× |
| Marine IK Reverse (minified) | 1,289 | 721 | 1.79× |
| Marine IK Reverse (prettified) | 5,823 | 2,822 | 2.06× |
| Marine IK (minified) | 1,304 | 757 | 1.72× |
| Marine IK (prettified) | 4,503 | 2,768 | 1.63× |
| Mesh (minified) | 2,428 | 1,056 | 2.30× |
| Mesh (prettified) | 4,482 | 1,664 | 2.69× |
| Random (minified) | 8,205 | 1,695 | 4.84× |
| Random (prettified) | 14,888 | 2,334 | 6.38× |
| Twitter (minified) | 13,212 | 3,967 | 3.33× |
| Twitter (prettified) | 17,138 (n/c) | 3,777 | 4.54× |
| Canada Small (minified) | 2,305 | 1,543 | 1.49× |
| Canada Small (prettified) | 6,472 | 2,404 | 2.69× |
| CitmCatalog Small (minified) | 8,343 | 1,825 | 4.57× |
| CitmCatalog Small (prettified) | 17,577 | 2,672 | 6.58× |
| Discord Small (minified) | 9,296 (n/c) | 2,617 | 3.55× |
| Discord Small (prettified) | 14,696 | 2,815 | 5.22× |
| Google Maps Response Small (minified) | 7,347 | 1,629 | 4.51× |
| Google Maps Response Small (prettified) | 17,871 | 2,813 | 6.35× |
| Instruments Small (minified) | 13,297 | 1,841 | 7.22× |
| Instruments Small (prettified) | 24,191 | 2,118 | 11.42× |
| Marine IK Reverse Small (minified) | 1,750 | 907 | 1.93× |
| Marine IK Reverse Small (prettified) | 7,353 | 3,196 | 2.30× |
| Marine IK Small (minified) | 1,614 | 944 | 1.71× |
| Marine IK Small (prettified) | 7,308 | 3,278 | 2.23× |
| Mesh Small (minified) | 2,359 | 1,007 | 2.34× |
| Mesh Small (prettified) | 4,836 | 1,689 | 2.86× |
| Random Small (minified) | 9,020 | 1,832 | 4.92× |
| Random Small (prettified) | 17,349 | 2,595 | 6.69× |
| Twitter Small (minified) | 11,429 | 3,060 | 3.74× |
| Twitter Small (prettified) | 14,988 | 3,877 | 3.87× |

| Reformatting test | Jsonifier | Glaze | simdjson (ondemand) | Lead |
| --- | --- | --- | --- | --- |
| Minify | 982 (n/c) | 1,415 (n/c) | 685 | 0.69× |
| Prettify | 3,223 | 1,783 | 534 | 1.81× |

### Losses and ties

The prettified lead comes from the indent tables: a newline plus indent is a few 8-byte stores, not a loop. Windows / MSVC, Linux / Clang and macOS / Clang win every freshly allocated test, and Windows / MSVC, Linux / Clang and Linux / GCC win every reused test. The four freshly allocated losses:

- **Double, Linux / GCC.** Jsonifier runs 351 vs the reflection writer's 353 MB/s (1.00×), a loss by Welch's test. It is ahead of Glaze (307 MB/s, 1.14×), and ahead of the reflection writer reused (887 vs 653 MB/s, 1.36×).
- **Int64, Linux / GCC.** Jsonifier runs 941 vs 992 MB/s (0.95×). It is ahead of Glaze (828 MB/s, 1.14×), and ahead of the reflection writer reused (5,393 vs 4,142 MB/s, 1.30×).
- **Double, macOS / GCC.** Jsonifier runs 262 vs 283 MB/s (0.93×). It is ahead of Glaze (201 MB/s, 1.30×), and ahead of the reflection writer reused (815 vs 669 MB/s, 1.22×).
- **Int64, macOS / GCC.** Jsonifier runs 691 vs 746 MB/s (0.93×). Reused it is ahead, 3,686 vs 3,018 MB/s (1.22×).

The freshly allocated tie is minified Mesh Small on macOS / GCC, where the reflection writer posts the higher mean (995 vs 638 MB/s) but the two kept epochs are too noisy for Welch's test to separate; it is a win reused. The two reused losses:

- **String, macOS / GCC.** The reflection writer runs 9,684 vs Jsonifier's 8,603 MB/s (0.89×). Freshly allocated, Jsonifier is ahead, 1,209 vs 1,168 MB/s (1.04×).
- **Minify, macOS / Clang.** Glaze reformats at 1,415 vs Jsonifier's 982 MB/s (0.69×), and neither result converged. Freshly allocated, Jsonifier is ahead, 2,087 vs 1,429 MB/s (1.46×). Minify reformats existing text through the stage-1 path, not the object serializer. It is the only test in either run in which Jsonifier is behind Glaze.

The reused tie is minified Canada on macOS / Clang, where Jsonifier's own result did not converge (1,156 vs Glaze's 1,461 MB/s); freshly allocated it is a win.

Every full-size document is ahead of the reflection writer on both GCC builds, freshly allocated and reused. Minified documents lead it by 1.09–3.13× freshly allocated and prettified documents by 26–63×. The Small documents follow the full-size ones: Jsonifier wins 99 of the 100 freshly allocated (the Mesh Small tie above) and all 100 reused. Not profiled yet: why the reflection writer edges ahead on single numeric values when the output string is freshly allocated, and on single String values when it is reused, is not known, and this paper does not guess at its algorithm.

**Reformatting.** Jsonifier wins Minify on all five builds freshly allocated, by 1.23–1.64× over the next-fastest library (Glaze in every case), and on four of five reused (1.25–1.64×), losing on macOS / Clang. It wins Prettify on all five builds in both runs, by 1.02–1.74× freshly allocated and 1.11–1.81× reused.

## Questions from external review

An outside review of a draft raised five concerns. Each is answered here against the code as it stands; where the answer is "not measured yet", it says so.

### Can an overstore run past the buffer?

No, because every overstore lands inside the string's own size, not past it. `serializeJson` resizes the buffer to the full bound plus 64 bytes before writing, so the write pass works inside memory the string already owns. No write ever reaches capacity or the allocator's edge, and ASan sees only in-bounds accesses.

The bound covers the largest overstore of each writer:

| Writer | Largest write beyond its true length | Covered by |
| --- | --- | --- |
| Bool | 3 bytes (8 written, 5 reserved) | 64-byte tail slack |
| Integer or float | none past the 32 bytes reserved per number | `staticSize` of 32 |
| Key literal over 16 bytes | up to 31 bytes (rounded to a multiple of 32) | 64-byte tail slack |
| Indent run | up to 7 bytes (rounded to 8) | 64-byte tail slack |
| String SIMD block | one vector width, stored before the escape check | the `6n + 2` reservation, which the copied bytes never exceed |
| Short string (under 16 bytes) | up to 15 bytes (one 16-byte store) | 64-byte tail slack |

The largest single overstore is 31 bytes, under the 64-byte slack. That slack is a fixed constant, not derived from the widest writer, so a future writer with a wider overstore must raise it.

### Does `6n + 2` blow up memory on huge strings?

Yes, the reservation is real. A 100 MB string field asks for about 600 MB before the final shrink, and the string keeps that capacity afterwards unless the caller calls `shrink_to_fit`. How much of it is touched depends on the buffer:

- With `resize_and_overwrite`, nothing is zero-filled. On Linux and macOS, pages that are never written are usually never faulted in, so the cost is mostly address space. On Windows, the allocation still counts against the commit limit.
- Without `resize_and_overwrite`, `resize` zero-fills the whole bound, so every page is touched.

Setting `prescanStrings` in `serialize_options` removes the 6× factor, at the price of one extra pass over every string's bytes during sizing. See *Pre-scanning strings* below. Its throughput cost has not been measured.

### Pre-scanning strings

`prescanStrings` replaces the `6n + 2` string bound with a scan that returns the exact number of bytes the writer will emit for the string, plus 2 for the quotes. The option is off by default and is a compile-time `if constexpr` in the string `get_size_impl`, so the default path is unchanged.

The scan (`string_serializer::escapedSize`) reads 8 bytes at a time and applies the same SWAR flag test the writer's tail path uses (`flagMask`): a word with no `"`, `\` or control byte adds 8 and moves on. A flagged word is summed byte by byte from `escapeTableSizes`, the same table the writer uses, where `\b \t \n \f \r " \\` take 2 bytes, other control bytes take 6, and everything else takes 1. The tail under 8 bytes is summed the same way. A NUL byte is counted as 1, although the writer's table emits nothing for it, so the bound can exceed the output by a byte but never fall short.

The 64-byte slack in `serializeJson` is unchanged, so the overstore argument above still holds. The option only affects strings: numbers, booleans and keys keep their existing bounds.

### Is it portable to big-endian targets?

The code has big-endian paths, but none are tested. `packed_blitter`, the bool constants, the digit tables and the SWAR string tail each branch on `std::endian::native` at compile time and use `std::byteswap` where needed. The CI matrix covers only little-endian x86 and ARM, so those branches have never run.

### Why is the String test slower on NEON than on AVX2?

The gap is a freshly allocated one. Freshly allocated, Jsonifier runs String at 1,209 MB/s on macOS / GCC (1.19× Glaze's 1,015, 1.04× the reflection writer's 1,168) and 983 MB/s on macOS / Clang (1.03× Glaze's 950). The AVX2 builds run the same test at 1,155–4,389 MB/s and lead Glaze by 1.30× (Windows / MSVC), 2.18× (Linux / Clang) and 1.92× (Linux / GCC). With the string reused the M1 lead over Glaze is 1.60× on macOS / Clang (6,630 vs 4,149 MB/s) and 1.82× on macOS / GCC (8,603 vs 4,738), against the 1.84–3.28× the AVX2 builds show reused. On macOS / GCC the reflection writer is ahead reused, at 9,684 MB/s.

So most of the M1 shortfall sits in the part of the freshly allocated run that the reused run removes, the allocation and release of the output string, rather than in the copy loop. That is an inference from the two runs, not a profile. Two features of the general loop remain candidates for whatever is left:

- Each block stops at its first escapable byte, writes that one escape, and reloads from the next byte. A string dense in escapes therefore pays one vector load per escape.
- Finding the first escapable byte needs a byte-mask extraction (`opBitMaskRaw`). x86 has a single instruction for this; NEON has to emulate it.

Neither has been isolated.

### Does simdjson's writer win doubles on GCC?

On both GCC builds it wins the freshly allocated run: 353 vs Jsonifier's 351 MB/s on Linux / GCC (1.00×, separated by Welch's test) and 283 vs 262 on macOS / GCC (0.93×). Reused, Jsonifier is ahead on both, 887 vs 653 MB/s (1.36×) on Linux / GCC and 815 vs 669 (1.22×) on macOS / GCC. Jsonifier beats Glaze on the Double test on all five builds freshly allocated, by 1.11–1.47×. Why the reflection writer is faster on the freshly allocated runs has not been measured, and this paper does not guess at its algorithm.

### What to check on MSVC

The Windows build has landed (table above), but its assembly has not been inspected yet. Three things are worth confirming there:

1. The 10^8 reciprocal uses `_umul128` and stays in registers, with no stack temporary.
2. `resize_and_overwrite` compiles without iterator-debug checks. Release builds default to `_ITERATOR_DEBUG_LEVEL=0`, so this should hold unless the project overrides it.
3. The fixed-size scalar stores stay scalar and are not merged into wider unaligned vector stores that split cache lines.

## Limitations

- **Over-allocation.** The size bound is worst case: every number reserves 32 bytes and, unless `prescanStrings` is set, every string `6n + 2`. A string-heavy document asks for up to about 6× its output size before shrinking. That is cheap with `resize_and_overwrite` but costs peak memory on large payloads. `prescanStrings` tightens the string part to the exact escaped length at the cost of an extra pass over each string; numbers still reserve 32 bytes each.
- **Excluded keys are a runtime lookup.** Types with `jsonifierExcludedKeys` do a set lookup per member in both passes.
- **Registration required.** Like the parser, the fast path needs `jsonifier::core<T>`; there is no reflection-free serializer for arbitrary types.
