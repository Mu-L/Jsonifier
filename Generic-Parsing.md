# Schema-Free, On Demand: Generic JSON Parsing in Jsonifier

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Jsonifier's two earlier papers were about parsing JSON into known types. If the parser knows the shape of the document at compile time, it can read in a single pass and skip building a structural tape. This paper covers the opposite case: **schema-free parsing**, where the caller walks a document of unknown shape and reads only the values it wants. That is the workload simdjson's On Demand API was built for, and the one where a structural index is actually worth building.

`jsonifier::generic` is a lazy, tape-driven parser. Its API is shaped like simdjson On Demand, but it relaxes one restriction: a value can be read more than once. This paper describes the design choices that let it match or beat On Demand on its own ground:

- a stage-1 reader tuned for small documents;
- a forward cursor that assumes reads happen in document order;
- zero-copy string extraction;
- a SIMD field index that is only built for objects that are actually being read out of order.

The benchmark suite reads every document four ways: every field in order, every field with each object's keys in reverse, a sparse subset of fields in order, and that subset in reverse. Each document is read both at full size and as a small (≤5 KiB) copy, and every single-document test runs twice: once filling freshly allocated output objects on every iteration, and once reusing the same cleared output objects, whose buffers keep their capacity. Across five platform/compiler builds, Jsonifier won 1,232 of 1,490 single-document results against simdjson On Demand, tied 57 and lost 201. On reverse-order reads it won 333 of 360. On Windows/MSVC it won 287 of 298 single-document results, tied 6 and lost 5.

The same parser also reads streams of many documents, such as NDJSON or comma-separated records, through `iterateMany`, the counterpart of simdjson's `iterate_many` (§8). Across five platform/compiler builds, on 26 streaming tests, Jsonifier won 113 of 130 results, tied 3 and lost 14, and won every streaming test on Windows/MSVC.

---

## 1. Background: why On Demand is hard to beat

simdjson's On Demand API does not build a DOM. Stage 1 produces a tape of structural positions, and stage 2 is a cursor over that tape driven by the caller. A value that is never requested is skipped over by structural depth, without being parsed. Reading in document order costs roughly one pass over the tape.

The price is a strictly forward-only API. Each value can be consumed once, and reading an object's fields out of order means rescanning the object for every key.

## 2. Design goals

1. **Keep On Demand's shape.** Existing On Demand code should port to `jsonifier::generic` mostly mechanically.
2. **Allow values to be read again.** A value handle remembers where it sits on the tape and can re-seek to it, instead of being invalidated.
3. **Make in-order reads free.** The common case must cost no more than a cursor increment and a key compare.
4. **Keep out-of-order reads close to in-order speed.** Their cost should stay close to linear in the size of the object, no matter what order the caller reads keys in.

## 3. Architecture

### 3.1 The tape

Stage 1 is Jsonifier's existing structural indexer (see *Two Stages, On Demand*, §4). It writes one `uint32_t` offset per structural character, and appends a sentinel entry `tape[count] = length`. Because of the sentinel, stage 2 never has to bounds-check the end of the tape on its hot paths.

Small documents are routed to a dedicated POD reader. When the input is shorter than one SIMD step, the fixed cost of setting up the full-width reader outweighs the actual work, and that reader's step sizes are chosen to fit the input.

### 3.2 A shared forward cursor

`document_state` holds one cursor into the tape and one depth counter. Every object, array and value handle refers to that shared state; a handle does not own its own iterator.

When a handle is used, it *settles* first:

- If the shared state is deeper than the handle, the state finishes the open containers up to the handle's depth.
- If the depth still doesn't match, the handle rewinds the cursor to its own opening position.

In-order reads never trigger either branch. Out-of-order reads pay only for the distance they actually move. On the hot paths, the cursor and depth are copied into locals, advanced there, and written back to the shared state once, so the compiler can keep them in registers.

### 3.3 Lazy errors

Accessors return `[[nodiscard]] error_code` and write their result to an out-parameter; they don't wrap results in a `result<T>`. A structural error is stored in the handle and passed along to every handle derived from it. `document::atEnd()` checks that the whole document was consumed. Errors therefore cost one well-predicted branch per access.

### 3.4 Finding fields

`findFieldUnordered(key)` runs in three tiers:

1. **The expected field.** In-order reads almost always find the requested key right at the cursor, possibly after skipping one comma. This tier needs one tape read and one key compare.
2. **Scan and wrap.** If the key isn't at the cursor, scan forward to the end of the object, then wrap around to the start of the object and scan up to where the cursor began.
3. **Escape-aware retry.** Only if both scans miss, rescan the object comparing keys with escape sequences decoded. Escaped keys are rare, so the common path never pays for decoding.

Once an object has shown that it is being read out of order, tier 2 is replaced by a probe of the field index described in §5.

### 3.5 Runtime CPU selection

`jsonifier::generic` takes part in the same runtime tier selection as the rest of the library. On x86, one Jsonifier binary carries every instruction-set tier it was configured for and picks one at run time. The headers are compiled once per tier inside a target-attribute region (`backend_passes.hpp`: `#pragma GCC target` on GCC, `#pragma clang attribute` on Clang), which yields one complete `jsonifier_core_internal<backend>` per tier: AVX-512 (`avx512f`, `avx512bw`, `avx512vbmi2`), AVX2 and AVX. Each of those inherits its tier's parser, serializer, validator, minifier, prettifier and generic iterator through CRTP, so every call inside a tier is a direct, inlinable call into code built for that tier. On first use, `selectSupportedBackend` reads CPUID (leaves 0, 1 and 7, plus the extended leaves for `lzcnt`) and XCR0, so a tier counts only if both the CPU and the operating system support it, and caches the widest supported tier in a function-local `static`. Every public entry point then dispatches with a fold expression over the compiled tiers:

```cpp
static const jsonifier_backend backend{ selectedBackend() };
static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
```

That is a load of a cached enum and at most three compares, once per call. There is no function pointer and no vtable. simdjson's runtime dispatch, by contrast, goes through virtual functions: `implementation` exposes `create_dom_parser_implementation`, `minify` and `validate_utf8` as virtuals, and `dom_parser_implementation` declares `parse`, `stage1`, `stage2` and `parse_string` pure virtual. Its own On Demand documentation notes that the On Demand API "has limited runtime dispatch support" and recommends compiling for a specific x64 target. In Jsonifier the same dispatch covers every API, including typed parsing, serialization and the generic On Demand-style iterator, and inside a tier nothing is virtual, so the per-tier code keeps all of its compile-time specialization.

The scope is x86. ARM builds compile one backend (NEON, or SVE2 when configured), as simdjson's ARM builds do. The set of x86 tiers compiled in is bounded by the configured tier (`JSONIFIER_CONFIGURED_AVX_TIER`), so a build configured for AVX2 carries AVX2 and AVX but not AVX-512. We have not separately measured the cost of the per-call check. This is the case simdjson's On Demand documentation leaves to the caller: a generic Jsonifier reader compiled once for x86 runs its AVX-512, AVX2 or AVX stage 1 depending on the machine, with no change to the calling code.

## 4. Zero-copy strings

`getString(std::string_view&)` returns a view straight into the input buffer whenever the string contains no escapes. It finds where the string ends with SIMD stop masks (a mask marking where a quote, backslash or control character appears):

- The first register is handled outside the loop ("peeled"), so short strings never enter the loop at all.
- The leftover bytes at the end are handled by a lookup table instead of a byte-by-byte loop.

Non-ASCII bytes go through UTF-8 validation on the view path. The `std::string` getter is split differently: pure-ASCII strings take the fast path, and everything else goes through the one-pass unescape-and-copy scanner. This keeps validation work off the path that already has to copy the bytes.

Strings that do contain escapes are decoded into an arena. Each string's tape offset maps to a fixed slot in the arena, so decoding the same value twice yields the same view. Earlier designs let the arena wrap around, which could overwrite views the caller still held; the fixed slots remove that failure.

**A GCC pitfall.** With `-funroll-loops`, GCC unrolls an un-peeled SIMD scan loop into a long prologue that runs even for short strings, which are most of the strings in the suite. The fix is to peel the first iteration by hand and use the table-driven tail described above.

## 5. The field index: making out-of-order reads cheap

Reading in reverse order is the worst case for tier 2 in §3.4. Each lookup scans past every key that comes after the target, so reading all *n* keys of an object in reverse costs O(n²).

### 5.1 The map

`field_index_map` is an insert-only, SwissTable-style hash map built specifically for this job:

- **Control bytes** come in 16-byte groups. An empty slot holds `-128`. An occupied slot holds `H2 = hash & 0x7F`, so every occupied control byte is non-negative.
- **Each entry** stores `{hash, object tape index, key tape index}`. It doesn't copy the key text; the key is compared against the tape directly.
- **The hash** mixes the key bytes with the object's tape index. The same key name in two different objects therefore hashes to different entries, and one map can serve the whole document. Short keys go through a single `mix64` mixing step; keys longer than 16 bytes are hashed a block at a time, murmur-style.
- **Probing** compares a whole control group against `H2` in one step, through the library's own SIMD wrappers (`gatherValuesU`, `opCmpEqRaw`, `opBitMaskRaw`), which lower to:
  - x86: `_mm_cmpeq_epi8` + `_mm_movemask_epi8`, one bit per slot;
  - NEON: `vceqq_u8` + a `vshrn`-narrowed nibble mask, four bits per slot, since NEON has no movemask instruction;
  - SVE2 (128-bit vectors): `svcmpeq_u8`, packed into the same four-bits-per-slot nibble mask;
  - any other target: the library's scalar SIMD emulation.
- **Reset** between documents clears the control bytes with `memset`, and only if the map was actually used for the previous document.

### 5.2 The lesson: whole-document indexing lost

The first version recorded every key the scanner skipped past, in every object. On Marine IK Reverse that made the parser **2.3x slower** than having no index at all, because Marine IK consists mostly of small objects with 3–6 fields each. For objects that small, re-scanning a few keys is cheaper than hashing, inserting and probing them.

### 5.3 Waiting until an object earns an index

The first fix was a counter on each object handle:

- The handle counts how many keys it has scanned past.
- Until that count reaches a threshold *T*, lookups use the plain scan-and-wrap path.
- After that, later scans record every key they pass into the map, and later lookups probe the map first.

Small objects never touch the map; large objects get O(1) lookups after one pass over their keys. The first A/B of that counter, run with *T* = 16:

| Test | Compiler | No index | Whole-doc index | Index after 16 skipped keys | vs simdjson |
|---|---|---|---|---|---|
| Marine IK Reverse (min) | GCC 16 | 7.49 ms | 17.1 ms | 6.24 ms | +79% |
| Marine IK Reverse (pretty) | GCC 16 | 8.77 ms | 18.0 ms | 7.42 ms | +61% |
| Marine IK Reverse (min) | Clang 24 | 8.03 ms | 19.2 ms | 6.48 ms | +42% |
| Marine IK Reverse (pretty) | Clang 24 | 8.86 ms | 20.0 ms | 7.25 ms | +40% |

*The "no index" and "index after 16 skipped keys" columns come from one A/B run and the "whole-doc index" column from another; each value is the median of 3 rounds, built with the CI flags. The "vs simdjson" column compares the "index after 16 skipped keys" build against simdjson in that same run.*

### 5.4 Choosing the threshold: a parameter sweep

*T* trades two costs. Set too high, objects with many fields rescan for longer before the map activates. Set too low, medium-sized objects pay for recording scans (hashing and inserting every key they pass) that a short rescan would have beaten. We swept *T* ∈ {2, 4, 8, 16} on four builds, running the full suite at each value, with the counter-only trigger above. The table shows Jsonifier's reverse-order margin over simdjson on the two tests that are sensitive to the threshold:

| Build | Test | *T* = 16 | *T* = 8 | *T* = 4 | *T* = 2 |
|---|---|---|---|---|---|
| Linux / GCC | Marine IK Reverse (min) | +74% | +82% | +11% | +39% |
| Linux / GCC | Random Reverse (min) | +29% | +37% | +42% | **−26%** |
| Linux / Clang | Marine IK Reverse (min) | +39% | +51% | **−14%** | +13% |
| Linux / Clang | Random Reverse (min) | +34% | +39% | +55% | **−25%** |
| macOS / Clang | Marine IK Reverse (min) | — | +55% | **−6%** | — |
| macOS / Clang | Random Reverse (min) | tie | — | +43% | **−27%** |
| macOS / GCC | Random Reverse (min) | +32% | +35% | — | **−38%** |

*"—" means the result did not converge in that run.*

Two failure modes repeat across builds:

- **At *T* = 2, Random Reverse loses on all four builds** where it converged. Random's objects are mid-sized, so almost every one crosses the threshold and pays for recording scans it didn't need.
- **At *T* = 4, Marine IK Reverse loses on two of the three builds** where it converged, for the same reason. On the third, Linux/GCC, it still wins, but its margin falls from +82% at *T* = 8 to +11%.

*T* = 8 is the lowest value with no reverse-order losses on any build. It also matched or beat *T* = 16 on most reverse tests: on Linux/Clang it lifted CitmCatalog Reverse from +85% to +106% and Discord Reverse from +307% to +365%, and in the sweep runs it turned Linux/Clang's in-order record from 21-0-2 into 22-1-0. (The sweep runs are separate from the runs reported in §7.)

The threshold is therefore chosen per OS/compiler, the same way Jsonifier chooses its stage-1 step geometry:

| Build | *T* | Basis |
|---|---|---|
| Linux / GCC | 8 | swept |
| Linux / Clang | 8 | swept |
| macOS / GCC | 8 | swept |
| macOS / Clang | 16 | swept; 8 and 16 had no reverse losses and could not be separated on this host |
| Windows / MSVC | 8 | not swept; uses the value that won on the other builds |

On macOS/Clang, 16 kept larger margins on the documents whose objects have many fields (Twitter, Discord, Mesh), while 8 won on Random, Google Maps and Instruments. Neither produced a reverse loss.

### 5.5 Requiring evidence of out-of-order access

A skipped-key count cannot tell a large object read out of order from a large object read sparsely in order. A sparse reader that asks for three fields of a 30-field object skips most of its keys on the way, so the count crosses *T* even though every lookup is found ahead of the cursor. Recording keys during those skips costs a hash and an insert per key for an index that no lookup needs.

The trigger in the measured code (§6) therefore asks for two kinds of evidence, both kept per object handle:

- **Size:** the skipped-key count has reached *T*, chosen per build as in §5.4. This is the gate that keeps small objects such as Marine IK's off the map.
- **Out-of-order access:** at least two lookups on this object have missed the expected field, scanned to the end of the object, and wrapped around to its start. A wrap is the event that only out-of-order reads produce. One wrap is not enough: a sparse reverse read of three fields often wraps once on its last lookup, whose key is first in the object, and an index built then would serve no further lookup.

Until both hold, lookups use scan-and-wrap and record nothing, so skips never pay for hashing. Once both hold, the next lookup that misses the expected field builds the object's index in one dedicated pass over the whole object, recording every key, and then probes it. Every later out-of-order lookup on that object is a single probe; a key the map does not hold falls through to the escape-aware retry of §3.4.

*T* was not re-swept after the wrap gate was added, and the wrap count of 2 is fixed on every build; it was not swept.

## 6. Methodology

- **Harness:** Json-Performance, branch `generic-parsing-main`, using the adaptive-sampling benchmark stage.
- **Tests:** 5 POD tests, plus 9 documents each run in minified and prettified form under four access patterns, at two sizes. Every test runs twice (see *Allocation* below), which gives 154 single-document results per build at full size and another 144 for the small documents.
  - **In order:** every field, keys requested in document order.
  - **Reverse:** every field, with every object's keys requested in reverse document order.
  - **Sparse:** a few fields from each record of the document's main arrays (for example a status's text, retweet count and user screen name in Twitter, or each performance's id, start and venue code in CitmCatalog); every other value is skipped by each library.
  - **Sparse reverse:** the same subset, requested in reverse document order.
- **Document sizes:** the full-size tests read the corpus documents as they are (from 11.8 KB for Google Maps up to 2.1 MB for Canada, minified). The *Small* tests read a truncated copy of each document: every array, and every object keyed by numbers, is cut to the same number of elements, and that number is the largest one that keeps the minified document at or under 5 KiB. The prettified small documents are the same values printed with indentation. Small documents put more weight on per-document setup, stage 1's fixed costs and the first lookups into each object, and less on steady-state throughput.
- **Allocation: fresh and reused.** Every single-document test runs twice for both libraries. In the **fresh** run, each iteration fills a newly constructed output struct, so every string, vector and nested object it holds is allocated again on every parse. In the **reused** run (`(Reused)` in the harness), one output struct lives across iterations and is cleared before each one, so its strings and vectors keep their capacity and the parse writes into buffers that are already allocated. In both runs each library keeps one parser for the whole test, so its tape, string buffer and other internal buffers are reused, as a long-running application would reuse them. The streaming tests (§8) run fresh only.
- **Readers:** the reverse and sparse readers are hand-written mirrors of the in-order readers. Both libraries fill the same structs through the same field-by-field traversal and use their unordered field lookup.
- **Correctness:** every test's output was serialized from both libraries and compared byte-for-byte, and all 515 pairs (103 tests, single-document and streaming, on each of the 5 builds) are identical. That comparison ran on Jsonifier `3e7bf6d`, before the small-document and reused tests were added, and it has not been re-run on the measured commit.
- **Convergence:** a result counts only if its retained epoch meets the RSE and mean-shift limits; non-converged results stay in the tallies and are marked (n/c) in the tables. Forty-three of the 1,490 single-document results and 19 of the 130 streaming results did not converge (see §9). The x86 builds used RSE < 5% and shift < 2.5%. The NEON builds ran on virtualized Apple hosts (an M1 for Clang, an M2 Pro for GCC) and used RSE < 10% and shift < 5%.
- **Integer parsing:** Jsonifier reads integers with a SWAR parser ported from void-numerics, which consumes digits in 8-, 4-, 2- and 1-byte chunks instead of one at a time.
- **Builds:**

| Build | Stage-1 kernel | Converged, full size | Converged, small | Converged, total |
|---|---|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 151 / 154 | 143 / 144 | 294 / 298 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 152 / 154 | 144 / 144 | 296 / 298 |
| Linux 7.0.0 / GCC 16.2 | AVX2 | 154 / 154 | 144 / 144 | 298 / 298 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2 | NEON | 148 / 154 | 144 / 144 | 292 / 298 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 124 / 154 | 143 / 144 | 267 / 298 |

All builds ran BenchmarkSuite `e7faa83`, Jsonifier `6bea81e` and simdjson `15e7eb8`. Jsonifier selects its generic stage-1 kernel at run time from the detected CPU; the table lists the kernel each build ran. The x86 builds ran on an Intel Core i9-14900KF. The results were published to `generic-parsing-main` on October 13, 2026.

## 7. Results

The percentages below are Jsonifier's throughput relative to simdjson On Demand. "tie" means Welch's t-test could not separate the two; "(n/c)" marks a value that did not converge, and it stays in the tallies. In the per-test tables, each cell gives the **fresh** result first and the **reused** result second.

### 7.1 Summary

**Fresh output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 13-2-3 | 17-0-1 | 18-0-0 | 18-0-0 | 17-1-0 | 18-0-0 | 18-0-0 | 18-0-0 | 142-3-4 |
| Linux / Clang | 5-0-0 | 17-0-1 | 17-1-0 | 18-0-0 | 14-0-4 | 10-0-8 | 16-0-2 | 11-1-6 | 13-1-4 | 121-3-25 |
| Linux / GCC | 5-0-0 | 9-0-9 | 16-0-2 | 16-1-1 | 18-0-0 | 4-2-12 | 16-0-2 | 12-0-6 | 18-0-0 | 114-3-32 |
| macOS / GCC | 5-0-0 | 12-1-5 | 17-1-0 | 17-1-0 | 18-0-0 | 15-1-2 | 18-0-0 | 16-0-2 | 17-1-0 | 135-5-9 |
| macOS / Clang | 3-1-1 | 13-4-1 | 16-1-1 | 12-2-4 | 13-0-5 | 14-2-2 | 17-1-0 | 9-0-9 | 12-3-3 | 109-14-26 |
| **All** | **23-1-1** | **64-7-19** | **83-3-4** | **81-4-5** | **81-0-9** | **60-6-24** | **85-1-4** | **66-1-23** | **78-5-7** | **621-28-96** |

**Reused output objects:**

| Build | POD | In order | Reverse | Sparse | Sparse reverse | Small, in order | Small, reverse | Small, sparse | Small, sparse reverse | Total |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows / MSVC | 5-0-0 | 15-2-1 | 18-0-0 | 18-0-0 | 18-0-0 | 17-1-0 | 18-0-0 | 18-0-0 | 18-0-0 | 145-3-1 |
| Linux / Clang | 5-0-0 | 17-1-0 | 16-2-0 | 18-0-0 | 14-0-4 | 9-2-7 | 16-0-2 | 10-0-8 | 14-0-4 | 119-5-25 |
| Linux / GCC | 5-0-0 | 9-0-9 | 16-0-2 | 16-0-2 | 18-0-0 | 6-1-11 | 16-0-2 | 12-1-5 | 18-0-0 | 116-2-31 |
| macOS / GCC | 5-0-0 | 14-1-3 | 18-0-0 | 17-1-0 | 17-1-0 | 14-3-1 | 16-1-1 | 15-0-3 | 18-0-0 | 134-7-8 |
| macOS / Clang | 3-0-2 | 10-3-5 | 16-0-2 | 11-2-5 | 11-2-5 | 13-2-3 | 15-1-2 | 7-2-9 | 11-0-7 | 97-12-40 |
| **All** | **23-0-2** | **65-7-18** | **84-2-4** | **80-3-7** | **78-3-9** | **59-9-22** | **81-2-7** | **62-3-25** | **79-0-11** | **611-29-105** |


Each cell is wins-ties-losses.

Across all 1,490 single-document results (43 of them did not converge; see §9), Jsonifier won 1,232, tied 57 and lost 201: 621-28-96 with fresh output objects and 611-29-105 with reused ones.

- **Reverse order is almost all wins at both sizes.** Across fresh and reused runs, Jsonifier won 167 of 180 full-size reverse results (5 ties, 8 losses) and 166 of 180 small reverse results (3 ties, 11 losses). All 19 of those losses are on Canada or Google Maps: eight on Linux/GCC (Canada at both sizes, −2.6% to −6.9%), four on Linux/Clang (Google Maps Small, −1.8% to −3.9%), one on Windows/MSVC (prettified Google Maps, fresh, −11.4%), one on macOS/GCC (minified Canada Small, reused, −2.7%) and five on macOS/Clang (Canada, −5.5% to −23.5%).
- **Windows/MSVC lost 5 of 298 single-document results:** minified Canada (−4.1%), Marine IK (−4.2%) and Mesh (−3.5%) read in order with fresh objects, minified Mesh read in order with reused objects (−4.9%), and prettified Google Maps Reverse with fresh objects (−11.4%). Its 6 ties are prettified Canada and Mesh read in order (full size, fresh and reused), minified Mesh Small (fresh) and minified Canada Small (reused) read in order. It won all 10 POD results and every sparse and sparse-reverse test, at both sizes and in both allocation modes.
- **Reusing the output objects helps both libraries and changes few verdicts.** Comparing each test's reused run with its fresh run, the median speedup from reuse is 6.1–11.1% for Jsonifier and 4.6–8.1% for simdjson, depending on the build. Jsonifier's median margin over simdjson moves by +0.7 to +1.8 percentage points on the non-MSVC builds and by +5.3 points on MSVC, where allocation is most expensive: Discord Reverse (min) goes from +358.0% to +443.6%, and Mesh Small Sparse (min) from +20.7% to +32.9%.

### 7.2 POD tests

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Bool | +66.0% / +65.7% | +26.0% / +26.3% | +48.6% / +45.0% | +25.7% / +23.5% | tie / −4.3% |
| Double | +62.2% / +72.9% | +49.1% / +49.2% | +52.5% / +53.7% | +76.0% / +87.6% | +40.6% / +34.0% (n/c) |
| Int64 | +52.8% / +56.5% | +62.2% / +59.9% | +41.1% / +43.4% | +47.8% / +33.8% | +24.1% / +27.5% |
| String | +17.6% / +34.5% | +12.5% / +15.6% | +7.2% / +6.0% | +7.9% / +12.6% | −11.4% / −12.9% |
| Uint64 | +58.7% / +64.0% | +54.5% / +53.5% | +53.5% / +55.0% | +55.5% / +54.1% | +39.5% / +25.3% |

### 7.3 Documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | −4.1% / +5.7% (n/c) | +5.8% / +7.6% | −7.4% / −8.0% | +9.5% / −5.2% (n/c) | +13.9% / +34.4% (n/c) |
| Canada (pretty) | tie / tie | +6.5% / +7.2% | −6.5% / −9.8% | −8.7% / +0.8% | +17.3% (n/c) / −17.1% (n/c) |
| CitmCatalog (min) | +73.2% / +76.8% | +7.1% / +7.6% | +8.3% / +6.9% | −3.0% / +8.9% | +18.5% / tie |
| CitmCatalog (pretty) | +69.5% / +75.5% | +5.1% / +6.2% | +4.8% / +5.5% | +17.8% / +25.2% | +37.1% (n/c) / −7.4% |
| Discord (min) | +70.3% / +96.4% | +6.9% / +12.0% | +4.6% / +9.1% | +28.5% / +60.8% | +9.2% / +1.5% |
| Discord (pretty) | +60.8% / +92.8% | +7.4% / +11.4% | +3.4% / +9.7% | +43.9% / +18.3% | tie / +4.6% |
| Google Maps (min) | +54.5% / +62.7% | +15.5% / +16.6% | +13.3% / +14.9% | +28.7% / +30.5% | tie / −5.2% |
| Google Maps (pretty) | +62.1% / +65.8% | +12.0% / +10.8% | +14.6% / +16.7% | +24.8% / +25.7% | +11.2% / tie |
| Instruments (min) | +103.7% / +120.7% | +6.4% / +5.4% | −3.2% / −3.4% | +4.5% / +19.0% | +4.3% / −21.5% |
| Instruments (pretty) | +108.0% / +104.4% | +3.5% / +3.4% | −3.5% / −4.8% | +8.9% / +10.4% | +16.3% / +6.4% |
| Marine IK (min) | −4.2% / +2.1% | +4.5% / +3.6% | −6.7% / −6.2% | −7.2% / tie | +15.3% / +20.6% (n/c) |
| Marine IK (pretty) | +9.2% / +10.0% | +10.1% / +7.7% | −1.8% / −4.7% | tie / +11.9% | +10.5% / +9.8% (n/c) |
| Mesh (min) | −3.5% / −4.9% | +1.3% / +1.8% | −2.6% / −3.0% | −10.2% / −18.9% | tie (n/c) / +59.9% (n/c) |
| Mesh (pretty) | tie / tie | −0.6% / tie | −4.9% / −5.9% | −5.0% / −5.5% | tie (n/c) / +23.5% |
| Random (min) | +65.8% / +88.4% | +21.4% / +24.6% | +16.5% / +20.6% | +46.6% / +58.6% | +11.7% / +9.6% |
| Random (pretty) | +66.9% / +90.4% | +19.8% / +22.7% | +16.3% / +19.4% | +43.3% / +53.3% | +32.0% (n/c) / +44.3% (n/c) |
| Twitter (min) | +56.3% / +78.1% | +12.6% / +14.0% | +5.9% / +6.0% | +2.7% / +19.3% | −2.9% / −1.6% (n/c) |
| Twitter (pretty) | +56.1% / +71.4% | +12.5% / +13.8% | −3.2% / −3.0% | +12.0% / +20.7% | +2.0% / tie |

### 7.4 Documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +4.6% / +3.7% | +5.0% / +6.7% | −2.6% / −3.3% | tie (n/c) / +7.8% | tie (n/c) / −23.5% (n/c) |
| Canada (pretty) | +6.2% / +9.7% | +5.8% / +2.6% | −4.9% / −6.9% | +24.5% (n/c) / +11.6% | −5.5% / −11.4% (n/c) |
| CitmCatalog (min) | +111.8% / +121.6% | +63.2% / +66.4% | +81.6% / +85.0% | +31.4% / +50.6% | +71.1% (n/c) / +76.7% |
| CitmCatalog (pretty) | +113.8% / +119.0% | +58.8% / +62.8% | +76.3% / +80.2% | +30.8% / +50.0% | +119.7% / +60.5% (n/c) |
| Discord (min) | +358.0% / +443.6% | +243.4% / +272.4% | +322.9% / +347.8% | +412.2% / +418.1% | +221.6% / +288.6% |
| Discord (pretty) | +366.7% / +394.4% | +243.7% / +264.2% | +311.9% / +339.4% | +304.9% / +365.0% | +193.7% / +324.5% |
| Google Maps (min) | +18.1% (n/c) / +119.1% | +0.5% / tie | +21.9% / +22.6% | +19.9% / +24.7% | +24.4% / +44.8% |
| Google Maps (pretty) | −11.4% / +35.6% | tie / tie | +20.3% / +21.8% | +14.0% / +20.4% | +7.0% / +8.3% |
| Instruments (min) | +251.8% / +273.4% | +123.6% / +125.4% | +134.9% / +148.4% | +144.0% / +138.2% | +135.6% / +146.4% |
| Instruments (pretty) | +235.3% / +260.9% | +115.0% / +272.0% (n/c) | +125.9% / +128.3% | +117.9% / +136.5% | +99.8% / +99.8% |
| Marine IK (min) | +44.5% / +44.5% | +30.8% / +33.2% | +66.3% / +70.4% | +53.2% / +53.9% (n/c) | +40.7% (n/c) / +16.2% |
| Marine IK (pretty) | +55.3% / +49.9% (n/c) | +31.3% (n/c) / +14.9% | +39.2% / +39.9% | +51.7% / +47.7% | +51.5% (n/c) / +27.1% |
| Mesh (min) | +54.0% / +59.8% | +32.3% / +35.0% | +49.7% / +53.2% | +75.6% / +58.0% | +47.0% (n/c) / +34.4% |
| Mesh (pretty) | +47.6% / +58.6% | +29.8% / +33.5% | +43.5% / +45.3% | +46.3% / +67.6% | +25.7% / +66.1% |
| Random (min) | +70.5% / +81.1% | +12.6% / +14.3% | +20.5% / +21.8% | +40.7% / +47.0% | +80.1% (n/c) / +38.0% |
| Random (pretty) | +72.3% / +82.6% | +12.7% / +14.3% | +18.0% / +20.2% | +38.7% / +43.8% | +21.8% / +28.0% |
| Twitter (min) | +323.5% / +365.8% | +198.2% / +210.9% | +127.0% / +132.8% | +131.7% (n/c) / +169.5% | +159.9% / +110.6% |
| Twitter (pretty) | +319.2% / +351.8% | +182.4% / +193.7% | +116.0% / +124.4% | +140.4% / +179.6% | +129.0% / +174.0% |

### 7.5 Documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +53.4% / +59.0% | +7.0% / +7.4% | +20.0% / +20.0% | +19.4% / +48.4% | +40.8% / +13.1% |
| Canada (pretty) | +97.5% / +102.7% | +16.6% / +15.2% | +10.8% / +9.4% | +5.4% / tie | +7.6% / +3.9% |
| CitmCatalog (min) | +69.5% / +77.5% | +23.4% / +22.6% | +18.9% / +18.8% | +37.1% / +29.0% | +21.4% / +34.6% |
| CitmCatalog (pretty) | +79.5% / +81.4% | +19.1% / +17.8% | +16.2% / +16.0% | +13.2% / +16.2% | +21.7% / +8.7% |
| Discord (min) | +25.4% / +36.0% | +1.7% / +0.5% | +5.6% / +5.7% | +43.5% (n/c) / +15.0% | +15.2% / +20.5% |
| Discord (pretty) | +29.7% / +37.2% | +1.9% / +1.2% | +3.8% / +4.7% | +15.3% / +24.2% | −2.0% / +9.5% |
| Google Maps (min) | +52.4% / +51.8% | +2.1% / +2.7% | tie / −2.3% | +19.9% / +22.9% | tie / −13.6% |
| Google Maps (pretty) | +58.5% / +63.2% | +1.9% / +2.3% | −0.6% / −0.9% | +22.0% / +21.7% | +54.8% (n/c) / −7.8% |
| Instruments (min) | +55.2% / +55.0% | +3.6% / +3.5% | +3.7% / +5.7% | +19.6% / +4.6% | +16.8% / −2.5% |
| Instruments (pretty) | +51.6% / +70.7% | +5.2% / +6.1% | +3.6% / +3.5% | +14.1% / +17.4% | −6.7% / −4.2% |
| Marine IK (min) | +75.4% / +76.5% | +29.3% / +27.9% | +31.0% / +30.7% | +8.8% / +18.8% | +45.2% / +35.4% |
| Marine IK (pretty) | +103.4% / +96.5% | +11.4% / +21.5% | +18.7% / +16.2% | +17.2% / +34.9% | +7.0% (n/c) / +21.5% |
| Mesh (min) | +37.4% / +38.0% | +8.3% / +8.0% | +19.6% / +21.1% | tie / +22.9% | +23.8% / +43.4% |
| Mesh (pretty) | +44.0% / +42.4% | +7.8% / +7.6% | +13.3% / +13.7% | +6.1% / +6.8% | +121.7% (n/c) / tie (n/c) |
| Random (min) | +51.3% / +59.1% | +14.4% / +16.6% | +6.6% / +7.4% | +62.6% / +52.7% | +19.3% / tie |
| Random (pretty) | +51.1% / +58.3% | +10.6% / +12.2% | +7.0% / +6.2% | +31.7% / +35.5% | tie / +57.4% |
| Twitter (min) | +55.0% / +59.1% | +4.8% / +5.7% | +5.3% / +5.1% | +14.0% / +12.5% | −7.1% / +7.7% |
| Twitter (pretty) | +55.8% / +61.1% | +3.9% / +4.0% | +2.7% / +3.0% | +16.6% / +19.7% | −10.5% / −2.4% |

### 7.6 Documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +67.1% / +65.9% | +2.3% / +2.4% | +24.6% / +24.3% | +19.5% / +18.8% | +43.6% (n/c) / +10.0% (n/c) |
| Canada (pretty) | +96.5% / +91.1% | +12.3% / +12.2% | +9.7% / +9.9% | +16.4% / +3.7% | −6.0% / −11.7% |
| CitmCatalog (min) | +56.6% / +58.9% | +20.9% / +22.2% | +34.5% / +36.9% | +24.3% / +6.4% | +45.2% / +14.3% |
| CitmCatalog (pretty) | +50.7% / +56.3% | +18.6% / +18.9% | +30.6% / +31.2% | +22.3% / +5.4% | +27.6% / +27.4% |
| Discord (min) | +39.0% / +47.0% | +6.6% / +8.1% | +8.9% / +12.2% | +24.6% / +32.5% | +24.8% / +18.5% |
| Discord (pretty) | +44.1% / +45.9% | +6.2% / +7.6% | +7.8% / +10.5% | +25.9% / +23.3% | −6.2% / −2.5% |
| Google Maps (min) | +44.7% / +43.9% | −7.2% / −8.1% | +9.9% / +10.1% | +5.8% / tie | +3.2% / −12.6% (n/c) |
| Google Maps (pretty) | +51.8% / +52.4% | −7.1% / −7.5% | +10.6% / +9.5% | +20.1% / +10.1% | −22.4% / −20.7% |
| Instruments (min) | +42.8% / +46.3% | −1.2% / −1.7% | +22.7% / +23.4% | +17.9% / +44.9% | −14.9% (n/c) / +5.4% |
| Instruments (pretty) | +51.0% / +51.0% | −0.8% / −2.0% | +18.9% / +18.9% | +16.5% / +10.3% | +4.5% / −4.2% |
| Marine IK (min) | +53.1% / +53.0% | +11.7% / +12.5% | +54.3% / +55.1% | +64.0% / +37.9% | +41.4% (n/c) / +63.1% (n/c) |
| Marine IK (pretty) | +72.7% / +71.9% | +15.0% / +15.4% | +30.5% / +31.2% | +36.6% / +20.9% (n/c) | +17.4% (n/c) / tie |
| Mesh (min) | +34.1% / +36.2% | +7.2% / +7.7% | +19.8% / +20.4% | +13.7% / +14.2% | +28.8% (n/c) / +9.1% |
| Mesh (pretty) | +46.8% / +42.9% | +6.8% / +7.2% | +13.0% / +13.8% | +6.5% / +7.1% | +37.0% / +9.2% |
| Random (min) | +38.0% / +41.9% | +9.4% / +10.5% | +7.0% / +9.0% | +32.7% / +34.3% | +4.1% / tie |
| Random (pretty) | +39.7% / +28.9% | +8.6% / +8.8% | +7.2% / +8.0% | +33.5% / +23.7% | +18.9% / +10.2% |
| Twitter (min) | +58.1% / +59.1% | +4.5% / +4.6% | +15.4% / +14.7% | +26.3% / +19.0% | −5.4% / +14.0% |
| Twitter (pretty) | +59.1% / +58.0% | +4.7% / +5.2% | +12.4% / +12.8% | +20.1% / +28.7% | +1.3% / +1.9% |

### 7.7 Small documents, in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +0.5% / tie | +5.6% / +2.3% | −15.2% / −15.5% | −10.7% / −9.6% | +6.9% / +11.8% |
| Canada (pretty) | +4.0% / +5.5% | +4.5% / +5.3% | −11.5% / −11.2% | +2.9% / tie | +3.8% / −2.7% |
| CitmCatalog (min) | +29.2% / +40.9% | −6.6% / −4.4% | −5.8% / −3.6% | +13.8% / +12.2% | −2.1% / +2.3% |
| CitmCatalog (pretty) | +34.6% / +45.0% | −7.4% / −5.6% | −5.0% / −4.9% | +39.4% / +5.8% | −16.4% / −2.2% |
| Discord (min) | +32.4% / +69.8% | +1.2% / +6.8% | tie / +3.4% | +17.4% / +34.0% | +8.9% / +7.2% |
| Discord (pretty) | +38.0% / +75.8% | +1.3% / +6.3% | −0.6% / +8.2% | +15.9% / +31.6% | +2.6% / +6.0% |
| Google Maps (min) | +62.3% / +75.6% | +12.6% / +13.8% | +11.2% / +13.7% | +20.9% / +25.7% | +4.7% / +7.3% |
| Google Maps (pretty) | +66.9% / +78.3% | +10.6% / +10.4% | +11.5% / +12.1% | +24.6% / +26.2% | +5.6% / +1.3% |
| Instruments (min) | +84.9% / +100.3% | −2.0% / −2.2% | −5.9% / −7.0% | +5.8% / +12.7% | +3.6% / tie |
| Instruments (pretty) | +97.6% / +97.5% | −2.7% / −3.3% | −8.0% / −7.4% | +23.7% / +17.0% | tie / +2.4% |
| Marine IK (min) | +12.6% / +14.5% | +0.9% / −0.3% | −4.0% / −4.0% | +5.0% / +3.6% | +2.7% / +4.4% |
| Marine IK (pretty) | +20.3% / +21.9% | +2.0% / +2.3% | −4.4% / −4.1% | +4.5% / +4.8% | +2.4% / +2.1% |
| Mesh (min) | tie / +3.6% | −2.6% / tie | −7.3% / −11.2% | −16.3% / tie | tie / tie |
| Mesh (pretty) | +1.5% / +2.2% | −5.5% / −5.6% | −13.1% / −13.5% | tie / tie | +8.0% / −66.8% |
| Random (min) | +62.0% / +85.7% | +4.6% / +15.0% | +12.8% / +17.8% | +43.2% / +33.8% | +17.5% / +14.2% |
| Random (pretty) | +58.7% / +81.8% | +7.3% / +18.4% | +13.5% / +16.5% | +13.1% / +46.3% | +13.0% / +22.4% |
| Twitter (min) | +60.6% / +82.1% | −6.1% / tie | −2.4% / tie | +39.4% / +25.2% | +5.7% / +12.7% |
| Twitter (pretty) | +56.7% / +73.9% | −4.6% / −1.3% | tie / −1.6% | +6.1% / +29.7% | +3.4% / +5.8% |

### 7.8 Small documents, keys in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +2.7% / +3.8% | +7.2% / +9.7% | −6.0% / −5.6% | +6.0% / −2.7% | +7.9% / −6.8% |
| Canada (pretty) | +7.6% / +8.1% | +7.0% / +5.4% | −4.3% / −4.9% | +30.8% / tie | +2.4% / −10.0% |
| CitmCatalog (min) | +77.8% / +89.6% | +43.8% / +51.1% | +74.0% / +79.1% | +67.6% / +108.4% | +50.7% / +59.0% |
| CitmCatalog (pretty) | +77.2% / +86.1% | +43.2% / +46.5% | +66.8% / +72.0% | +64.3% / +55.9% | +47.8% / +53.1% |
| Discord (min) | +371.9% / +472.5% | +245.1% / +270.8% | +271.4% / +305.8% | +316.6% / +312.1% | +262.9% / +292.2% |
| Discord (pretty) | +370.6% / +681.0% | +230.7% / +255.3% | +268.3% / +291.0% | +298.8% / +340.5% | +232.4% / +270.4% |
| Google Maps (min) | +32.0% / +36.3% | −2.3% / −3.9% | +20.8% / +20.1% | +13.7% / +24.1% | +4.2% / +6.0% |
| Google Maps (pretty) | +34.4% / +38.8% | −3.1% / −1.8% | +17.9% / +18.7% | +18.4% / +18.8% | tie / tie (n/c) |
| Instruments (min) | +241.6% / +262.0% | +122.8% / +125.8% | +130.6% / +134.5% | +148.6% / +157.2% | +133.0% / +135.7% |
| Instruments (pretty) | +243.7% / +250.3% | +116.8% / +122.8% | +125.3% / +126.8% | +142.5% / +140.5% | +120.8% / +206.8% |
| Marine IK (min) | +38.2% / +39.6% | +28.4% / +28.9% | +54.7% / +57.6% | +46.3% / +47.8% | +29.9% / +32.7% |
| Marine IK (pretty) | +42.0% / +41.9% | +25.2% / +28.1% | +51.3% / +54.3% | +53.8% / +48.8% | +30.4% / +31.6% |
| Mesh (min) | +33.6% / +55.3% | +23.3% / +27.3% | +25.6% / +32.2% | +19.9% / +30.4% | +26.7% / +27.9% |
| Mesh (pretty) | +34.3% / +53.5% | +20.1% / +25.6% | +26.5% / +27.0% | +20.0% / +37.0% | +21.7% / +32.0% |
| Random (min) | +81.7% / +93.8% | +27.2% / +34.3% | +40.0% / +44.9% | +58.1% / +71.9% | +43.5% / +47.3% |
| Random (pretty) | +81.1% / +93.1% | +26.7% / +32.3% | +38.1% / +40.7% | +56.3% / +59.4% | +42.8% / +45.4% |
| Twitter (min) | +294.5% / +311.1% | +159.6% / +167.7% | +144.1% / +149.5% | +163.8% / +147.9% | +148.6% / +161.4% |
| Twitter (pretty) | +291.4% / +308.0% | +156.4% / +159.9% | +138.9% / +140.8% | +163.7% / +177.3% | +147.8% / +159.5% |

### 7.9 Small documents, sparse fields in order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +36.7% / +38.5% | +20.2% / +22.4% | +14.5% / +15.5% | +41.1% / +15.9% | −5.9% / −4.7% |
| Canada (pretty) | +73.8% / +81.4% | +24.2% / +24.0% | +15.5% / +14.7% | +24.9% / +44.1% | −1.0% / tie |
| CitmCatalog (min) | +34.3% / +41.2% | +13.6% / +15.2% | +15.6% / +19.3% | +10.9% / +14.6% | +14.5% / +15.5% |
| CitmCatalog (pretty) | +59.1% / +61.9% | +9.3% / +10.4% | +12.6% / +13.9% | +15.2% / +21.2% | +10.7% / +10.7% |
| Discord (min) | +21.1% / +29.9% | +2.3% / −2.2% | −1.1% / −0.6% | +18.6% / +10.3% | +192.9% / −1.4% |
| Discord (pretty) | +35.3% / +45.5% | tie / −2.7% | −6.1% / −3.6% | +21.6% / +21.3% | −6.9% / −7.7% |
| Google Maps (min) | +52.1% / +50.7% | −1.2% / −1.6% | −1.9% / −0.9% | +6.8% / +13.8% | +4.3% / −6.3% |
| Google Maps (pretty) | +55.4% / +53.7% | −1.9% / −1.4% | −1.7% / −0.7% | +14.6% / +26.9% | −3.7% / −2.0% |
| Instruments (min) | +28.3% / +33.0% | −3.5% / −2.6% | +1.7% / +2.7% | +19.7% / +16.4% | −3.9% (n/c) / −8.1% |
| Instruments (pretty) | +37.5% / +41.5% | −2.2% / −2.4% | +1.6% / +3.2% | −1.6% / −12.0% | −8.2% / −5.6% |
| Marine IK (min) | +53.7% / +52.6% | +16.7% / +18.6% | +27.1% / +27.3% | +4.9% / +15.4% | +16.2% / +8.4% |
| Marine IK (pretty) | +73.3% / +75.3% | +17.1% / +17.8% | +22.2% / +21.8% | +14.1% / +19.3% | +8.2% / +5.7% |
| Mesh (min) | +20.7% / +32.9% | +6.6% / +4.6% | +14.9% / +19.0% | +12.3% / +27.6% | +8.1% / +15.2% |
| Mesh (pretty) | +28.1% / +38.7% | +4.7% / +2.9% | +12.3% / +12.3% | −8.3% / −7.2% | +3.4% / tie |
| Random (min) | +32.8% / +44.7% | +5.2% / +4.9% | +2.3% / +1.7% | +20.6% / +37.7% | +4.4% / +8.1% |
| Random (pretty) | +37.0% / +48.3% | +8.8% / +6.2% | +4.4% / +5.5% | +29.4% / +41.3% | −1.0% / +3.3% |
| Twitter (min) | +26.7% / +27.5% | −11.2% / −11.7% | −3.0% / tie | +3.7% / −8.2% | −1.5% / −2.0% |
| Twitter (pretty) | +27.6% / +33.5% | −9.9% / −10.4% | −0.8% / −2.9% | +45.1% / +6.0% | −5.3% / −6.0% |

### 7.10 Small documents, sparse fields in reverse order

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Canada (min) | +33.4% (n/c) / +32.4% | +21.2% / +22.5% | +19.3% / +20.1% | +37.2% / +31.1% | −9.9% / −9.9% |
| Canada (pretty) | +63.0% / +66.4% | +24.3% / +22.6% | +21.6% / +21.5% | +28.2% / +68.6% | −5.1% / −3.1% |
| CitmCatalog (min) | +18.5% / +35.3% | +13.6% / +11.7% | +30.3% / +27.8% | +21.6% / +21.2% | +7.9% / +10.4% |
| CitmCatalog (pretty) | +35.2% / +40.6% | +16.1% / +14.0% | +26.8% / +25.4% | +4.4% / +20.3% | +17.7% / +6.5% |
| Discord (min) | +31.3% / +46.0% | +3.5% / +1.6% | +7.8% / +9.5% | +26.4% / +48.1% | +5.8% / +5.3% |
| Discord (pretty) | +35.9% / +37.7% | +1.3% / +0.5% | +4.1% / +6.2% | +22.8% / +27.7% | tie / +2.5% |
| Google Maps (min) | +33.8% / +39.0% | −10.3% / −8.1% | +7.1% / +7.2% | +2.0% / +8.7% | tie / −6.9% |
| Google Maps (pretty) | +39.3% / +45.6% | −8.5% / −8.1% | +6.9% / +7.0% | +11.2% / +6.4% | −11.0% / −5.6% |
| Instruments (min) | +23.4% / +31.8% | +2.6% / +0.4% | +15.7% / +17.4% | +33.6% / +40.9% | +7.6% / +8.3% |
| Instruments (pretty) | +36.2% / +38.1% | tie / +0.9% | +15.9% / +15.8% | +28.4% / +25.5% | +3.6% / +4.4% |
| Marine IK (min) | +34.9% / +32.4% | +1.3% / +3.1% | +30.3% / +30.7% | +29.4% / +30.9% | +17.9% / +22.5% |
| Marine IK (pretty) | +57.5% / +55.7% | +5.2% / +5.6% | +24.9% / +25.8% | +27.7% / +23.7% | +14.2% / +6.4% |
| Mesh (min) | +8.4% / +37.7% | +5.3% / +5.1% | +13.0% / +10.9% | +7.7% / +13.1% | +4.9% / +6.2% |
| Mesh (pretty) | +23.7% / +31.2% | +2.4% / +3.2% | +6.6% / +9.3% | +7.9% / +11.9% | +3.0% / −0.8% |
| Random (min) | +37.6% / +37.1% | +2.3% / +5.5% | +7.5% / +5.6% | +20.3% / +39.5% | +4.4% / +9.6% |
| Random (pretty) | +40.6% / +38.1% | +4.6% / +5.5% | +7.8% / +7.6% | +20.7% / +22.2% | +3.1% / −0.9% |
| Twitter (min) | +33.5% / +36.4% | −2.9% / −2.5% | +7.9% / +9.5% | +40.7% / +27.2% | +4.2% / +3.0% |
| Twitter (pretty) | +34.8% / +39.4% | −2.3% / −1.2% | +6.6% / +5.6% | tie / +2.1% | tie / −1.3% |

### 7.11 What the results show

- **Reverse order is where the design pays off.** On documents whose objects have many fields (Discord, Twitter, Instruments), simdjson has to rescan for every out-of-order key, and Jsonifier is 2.0x to 5.4x faster at full size: +99.8% up to +443.6% (Discord min, MSVC, reused). The small documents show the same pattern, from +116.8% to +681.0%, because a 5 KiB Discord or Instruments document still holds objects with dozens of keys. On documents made of small objects (Canada, Google Maps) there is little to rescan, and outside MSVC the full-size margin runs from −23.5% to +44.8%. That matches the cost model in §5: the map only pays off when there are many skipped keys to index.
- **Sparse reverse reads are a strength.** Jsonifier won 159 of 180 full-size sparse-reverse results across both allocation modes (3 ties, 18 losses), and 157 of 180 small ones (5 ties, 18 losses). MSVC and Linux/GCC won every one of their 72. Of the 18 full-size losses, 10 are on macOS/Clang and 8 on Linux/Clang (Google Maps, −7.1% to −8.1%, and Instruments, −0.8% to −2.0%), and 7 of the 18 are Google Maps.
- **MSVC shows the largest gaps.** On MSVC, simdjson's rescans are far slower than on the other builds: Discord Reverse (pretty, fresh) runs at 109.8 MB/s on MSVC against 234.6 MB/s on Linux/Clang, on the same CPU. Jsonifier reads the same test at 512.4 and 806.3 MB/s. Every MSVC full-size sparse result is a win, by +25.4% to +103.4%, and every full-size sparse-reverse result by +28.9% to +96.5%.
- **Sparse reads in order are mostly wins.** Across both allocation modes, Jsonifier won 161 of 180 full-size sparse results (7 ties, 12 losses) and 128 of 180 small ones (4 ties, 48 losses). MSVC and Linux/Clang won all 36 full-size results, and MSVC all 36 small ones. On full-size documents, 9 of the 12 losses are on macOS/Clang (Twitter, Google Maps, Instruments and Discord) and 3 are Google Maps on Linux/GCC (−0.6% to −2.3%).
- **In-order full reads are mostly wins at full size.** Across 180 full-size in-order results, Jsonifier won 129, tied 14 and lost 37. Linux/Clang went 34-1-1 and MSVC 28-4-4. Linux/GCC split its in-order tests 18-0-18 across the two modes, macOS/GCC 26-2-8 and macOS/Clang 23-7-6.
- **Small documents read in order are the weakest in-order category.** Across 180 results, Jsonifier won 119, tied 15 and lost 46. MSVC (34-2-0), macOS/GCC (29-4-3) and macOS/Clang (27-4-5) won most of theirs, but Linux/Clang went 19-2-15 and Linux/GCC 10-3-23. These documents are at most 5 KiB, so a parse is short, and the fixed per-parse costs of Jsonifier's stage 1 and document setup count for more. The Linux/Clang losses there are modest (−0.3% to −7.4%) and Linux/GCC's reach −15.5% on Canada, while the in-order small-document wins on Google Maps and Random on those two builds run from +4.6% to +18.4%.
- **In-order losses at full size cluster on Linux/GCC.** It lost Canada, Instruments, Marine IK, Mesh and Twitter in order, by −1.8% to −9.8%. macOS/GCC lost eight: Canada (pretty, fresh, −8.7%; min, reused, −5.2%), CitmCatalog (min, fresh, −3.0%), Marine IK (min, fresh, −7.2%) and Mesh (min, −10.2% fresh and −18.9% reused; pretty, −5.0% and −5.5%). Canada, Marine IK and Mesh are dominated by float arrays, where the time goes into number parsing and stage 1, not into the generic layer's field lookup. Linux/Clang lost one full-size in-order test, prettified Mesh (fresh, −0.6%).
- **The POD tests win on most builds.** Jsonifier went 46-1-3 on them, and every non-macOS/Clang build won all 10. Double won all 10 results (+34.0% to +87.6%), Int64 all 10 (+24.1% to +62.2%) and Uint64 all 10 (+25.3% to +64.0%). Bool won 8 of 10 (+23.5% to +66.0%), tying on macOS/Clang fresh and losing there reused (−4.3%), and String won 8 of 10 (+6.0% to +34.5%), losing on macOS/Clang fresh (−11.4%) and reused (−12.9%).

## 8. Streams: many documents in one buffer

Many real inputs are not one document but a sequence of them: NDJSON logs, JSON Lines exports, or documents joined by commas. simdjson handles these with `iterate_many`, which runs stage 1 over a window of the input and hands out one On Demand document at a time. `jsonifier::generic` does the same through `parser::iterateMany`, and every document it hands out is an ordinary `generic::document`, with re-readable values and the per-object field index from §5.

### 8.1 Design

```cpp
for (jsonifier::generic::document doc : parser.iterateMany(buffer)) { ... }
auto stream = parser.iterateMany<parse_options{ .newLineDelimited = false }>(buffer, batchSize);
```

- **The separator is part of the type.** `parse_options::newLineDelimited` selects it: `true` (NDJSON and other whitespace-separated streams, the `iterateMany` default) or `false` for comma-delimited streams. The two modes find document boundaries in different ways (§8.2), and the choice is made at compile time.
- **Windowed stage 1.** Stage 1 runs over one window of `batchSize` bytes (1 MiB by default), using the same indexer, and the same small-input routing, as `iterate()`. Tape offsets are relative to the window, so no single window can exceed 4 GiB, but the stream as a whole can.
- **One document per tape slice.** Each document is bound to the parser's shared cursor (§3.2), starting at its first structural. The accessors, the lazy errors and the field index work unchanged; the field index is reset per document.
- **Documents cut by the window edge.** No document is ever parsed from two windows. The next window starts at the first byte of the first document that the current window could not complete, and a document larger than a whole window returns `error_code::capacity`, as in simdjson.
- **Comma-separated streams.** In comma mode, a comma between top-level documents is accepted as a separator. A leading or doubled comma is a `tape_error`, and a trailing comma is `trailing_content`. A comma that falls at the start of a new window is still consumed as a separator.
- **Reporting.** The stream exposes `truncatedBytes()` for an unfinished final document, and each iterator exposes `currentIndex()` and `source()` for the current document's offset and raw text. After an error, the stream yields that error once and then ends.
- **Lifetime.** As with simdjson, advancing the stream invalidates the previous document; the only thing that survives is a zero-copy view of an unescaped string, which points into the caller's buffer.

### 8.2 Finding where documents end

The simplest way to find each document's end is to walk its structurals and count depth before handing the document out. That bounds every later tape scan to the document, but a reader that consumes the document in order walks the same structurals again, so every in-order document pays for two passes over its part of the tape.

The whitespace-delimited mode finds boundaries without that pass:

- **A local rule marks document starts.** Inside any container, every value is preceded on the tape by `[`, `{`, `,` or `:`. So in a whitespace-delimited stream, a structural that can start a value and directly follows a closer or a scalar begins a new top-level document. No depth needs to be tracked to recognize it.
- **One short backward scan per window.** Scanning backward from the end of the window's tape, the first position that satisfies the rule is the start of the window's last document. Everything before it is complete. That last document is deferred to the next window unless this is the final window; only when a window holds a single document, or for the final document of the final window, is a depth walk used to decide completeness. The scan usually stops within a few structurals of the window's end.
- **The reader tells the stream where the document ended.** When the caller advances, the parser's state already records how far the reads went. If the cursor is still inside the document, the stream finishes the open containers from the cursor. For an in-order reader, whose cursor rests just before the closing brace, that is about one step. If the cursor is past a closed root value, it is the end. Only a document the caller never touched is skipped from its start.

Comma mode cannot use the local rule, because a top-level comma looks exactly like a comma inside a container. It keeps the up-front depth walk, and that difference is visible in the results: on the in-order corpus streams on x86, Jsonifier's comma-separated throughput is 6.1% to 27.5% below its own NDJSON throughput on the same records.

### 8.3 Method

The streaming benchmarks run in the same Json-Performance suite and the same run as §6 (branch `generic-parsing-main`), using the same adaptive-sampling stage. Both libraries use a 1 MiB batch, simdjson runs unthreaded (`parser.threaded = false`), and both fill the same structs through the same field-by-field readers. Each stream is read from a saved file and repeated to its target size, so every build streams the same bytes. There are 26 tests per build, in two groups:

- **Amazon Cellphones and Stream Formats.** These follow simdjson's streaming benchmarks. *Amazon Cellphones* is the `amazon_cellphones.ndjson` file from the simdjson repository (793 lines, each a 9-element array), with the same per-brand rating aggregation and the same skipped header line; *Large Amazon Cellphones* repeats it to 10 MiB, as simdjson's `large_amazon_cellphones` benchmark does. *Stream Formats* uses the `{"id", "name", "payload", "flag"}` documents with 16-byte (Small) and 4096-byte (Large) payloads, in NDJSON and comma-separated form, and sums `"id"` over the stream. simdjson generates 128 MB of these; the port repeats the first 64 documents to 16 MB so that a run fits the sampling window, so the ids cycle through 0–63. Every record here is an array or a four-key object, so this group has no reverse variant.
- **Record streams from the paper's corpus.** The main record array of five of the documents from §6 is split into one document per record, each record is re-serialized minified, and the stream is repeated to 4 MiB: CitmCatalog `performances`, Google Maps `rows`, Instruments `patterns`, Random `result` and Twitter `statuses`. Each stream runs in NDJSON and comma-separated form, both in order and with every object's keys read in reverse, using the reverse readers from §6. The other four documents have no record array to split and are not streamed.
- **Correctness:** for every streaming test on every build, Jsonifier's serialized output was compared with simdjson's, and all 130 pairs are byte-identical. They are part of the 515 pairs in §6, with the same scope: the comparison ran on `3e7bf6d`.

| Build | Stage-1 kernel | Converged |
|---|---|---|
| Windows 10.0.26200 / MSVC 19.51 | AVX2 | 23 / 26 |
| Linux 7.0.0 / Clang 24.0 | AVX2 | 26 / 26 |
| Linux 7.0.0 / GCC 16.2 | AVX2 | 26 / 26 |
| macOS 25.6 (M2 Pro, virtual) / GCC 16.2 | NEON | 16 / 26 |
| macOS 25.6 (M1, virtual) / Clang 23.1 | NEON | 20 / 26 |

The commits, hosts and convergence limits are the same as in §6. Unlike the single-document tests, the streaming tests have no reused variant: each document is read into a freshly constructed record.

### 8.4 Results

As in §7, the percentages are Jsonifier's throughput relative to simdjson On Demand.

| Build | simdjson's benchmarks W-T-L | Corpus, in order W-T-L | Corpus, reverse W-T-L | Total W-T-L |
|---|---|---|---|---|
| Windows / MSVC | 6-0-0 | 10-0-0 | 10-0-0 | 26-0-0 |
| Linux / Clang | 5-0-1 | 10-0-0 | 9-0-1 | 24-0-2 |
| Linux / GCC | 5-0-1 | 8-0-2 | 8-0-2 | 21-0-5 |
| macOS / GCC | 6-0-0 | 10-0-0 | 7-1-2 | 23-1-2 |
| macOS / Clang | 3-0-3 | 8-0-2 | 8-2-0 | 19-2-5 |
| **All** | **25-0-5** | **46-0-4** | **42-3-5** | **113-3-14** |

**Amazon Cellphones and Stream Formats:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| Amazon Cellphones (NDJSON) | +27.3% | +20.7% | +5.6% | +28.7% | +14.4% |
| Large Amazon Cellphones (NDJSON) | +13.6% | +15.5% | +1.4% | +12.3% | +12.5% |
| Stream Formats Small (NDJSON) | +51.4% | −5.2% | +5.9% | +9.9% (n/c) | −15.8% |
| Stream Formats Small (comma) | +80.7% | +73.2% | +99.1% | +82.4% (n/c) | +66.2% (n/c) |
| Stream Formats Large (NDJSON) | +93.1% | +4.6% | −5.4% | +10.4% | −7.8% |
| Stream Formats Large (comma) | +95.9% | +10.3% | +3.1% | +6.1% | −6.0% |

**Corpus record streams, in order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | +55.7% | +6.5% | +3.7% | +5.9% | +16.7% |
| CitmCatalog (comma) | +59.4% | +17.8% | +23.8% | +21.5% | +25.2% (n/c) |
| Google Maps (NDJSON) | +76.5% | +28.0% | +13.1% | +25.6% (n/c) | +9.5% |
| Google Maps (comma) | +59.3% | +27.1% | +32.3% | +17.7% | −28.4% |
| Instruments (NDJSON) | +129.0% | +15.7% | −5.3% | +10.9% (n/c) | −7.6% |
| Instruments (comma) | +88.2% | +29.3% | +30.8% | +32.7% | +22.7% |
| Random (NDJSON) | +84.7% | +20.3% | +12.6% | +47.5% | +39.1% |
| Random (comma) | +77.4% | +27.7% | +29.8% | +90.5% (n/c) | +53.2% |
| Twitter (NDJSON) | +64.5% | +7.5% | −5.3% | +15.3% (n/c) | +41.2% |
| Twitter (comma) | +54.8% | +15.2% | +10.3% | +26.5% | +21.7% (n/c) |

**Corpus record streams, keys in reverse order:**

| Test | MSVC | Linux Clang | Linux GCC | macOS GCC | macOS Clang |
|---|---|---|---|---|---|
| CitmCatalog (NDJSON) | +78.4% | +37.7% | +44.6% | +33.7% | +35.2% (n/c) |
| CitmCatalog (comma) | +89.5% | +52.1% | +71.0% | +48.7% | +51.2% |
| Google Maps (NDJSON) | +46.3% (n/c) | +9.6% | +12.2% | −8.5% (n/c) | +6.5% |
| Google Maps (comma) | +40.7% | +14.5% | +30.6% | +18.8% | tie (n/c) |
| Instruments (NDJSON) | +38.7% | −11.0% | −24.8% | −14.1% (n/c) | tie |
| Instruments (comma) | +62.5% | +5.6% | −4.8% | tie | +12.1% (n/c) |
| Random (NDJSON) | +126.4% | +43.9% | +32.4% | +77.2% (n/c) | +59.9% |
| Random (comma) | +131.6% | +61.7% | +53.9% | +85.8% | +115.4% |
| Twitter (NDJSON) | +384.0% (n/c) | +215.7% | +138.6% | +200.0% | +173.1% |
| Twitter (comma) | +389.1% (n/c) | +231.1% | +165.6% | +197.5% (n/c) | +196.0% |

### 8.5 What the results show

- **Amazon Cellphones and Stream Formats.** Jsonifier took 25 of the 30 results in that group and lost 5. It won Amazon Cellphones (+5.6% to +28.7%), Large Amazon Cellphones (+1.4% to +15.5%) and Stream Formats Small (comma) (+66.2% to +99.1%) on all five builds. The five losses are all Stream Formats: Small (NDJSON) on Linux/Clang (−5.2%) and macOS/Clang (−15.8%), Large (NDJSON) on Linux/GCC (−5.4%) and macOS/Clang (−7.8%), and Large (comma) on macOS/Clang (−6.0%). Stream Formats Small has the smallest documents in the suite, so it puts the most weight on per-document overhead.
- **Windows/MSVC won every streaming test,** 26-0-0; Linux/Clang lost two of 26, macOS/GCC two, Linux/GCC five and macOS/Clang five.
- **In-order corpus streams are mostly wins.** Of the 50 in-order corpus results, Jsonifier won 46 and lost 4: Instruments in NDJSON form on Linux/GCC (−5.3%) and macOS/Clang (−7.6%), Twitter in NDJSON form on Linux/GCC (−5.3%), and Google Maps in comma form on macOS/Clang (−28.4%).
- **Reverse order carries over from single documents to streams.** Jsonifier won 42 of 50 reverse stream results and tied 3. The margins follow object size, as in §7.11: Twitter statuses, which have many fields, give +138.6% to +389.1%, while Google Maps rows, which have few, give +6.5% to +46.3%. Four of the five losses are Instruments: NDJSON on Linux/Clang (−11.0%), Linux/GCC (−24.8%) and macOS/GCC (−14.1%), and comma-separated on Linux/GCC (−4.8%); the fifth is Google Maps NDJSON on macOS/GCC (−8.5%). The ties are Instruments on macOS/GCC (comma) and macOS/Clang (NDJSON), and Google Maps in comma form on macOS/Clang.
- **MSVC again shows the largest gaps.** On the same CPU, simdjson reads Random Reverse (comma) at 199.5 MB/s under MSVC and 466.6 MB/s under Linux/Clang, while Jsonifier reads it at 462.1 and 754.5 MB/s.

## 9. Limitations

- **Linux/GCC and macOS/Clang hold most of the single-document losses:** 63 and 66 of the 201, against 50 on Linux/Clang, 17 on macOS/GCC and 5 on MSVC. macOS/Clang's are spread across access patterns, led by 18 small sparse, 10 full-size sparse reverse, 10 small sparse reverse and 9 full-size sparse. The largest individual losses are on macOS/Clang: Mesh Small (pretty, reused) read in order at −66.8% and Canada Reverse (min, reused) at −23.5%. Both macOS builds ran on virtualized hosts, so part of these results likely reflects host noise.
- **Linux/GCC loses mostly on reads in document order.** 41 of its 63 losses are in-order reads: 23 of its 36 small in-order results and 18 of its 36 full-size ones. At full size they are on Canada, Instruments, Marine IK, Mesh and Twitter, by −1.8% to −9.8%; at small size they reach −15.5% (Canada Small, min, reused). It also loses reverse reads of Canada at both sizes (−2.6% to −6.9%) and 11 small sparse results, while it won every sparse-reverse result at both sizes.
- **Small documents lose on Linux.** Linux/Clang lost 15 of its 36 small in-order results and Linux/GCC 23 of 36, while Linux/Clang lost one of its 36 full-size in-order tests. Linux/Clang also lost 14 of its 36 small sparse results and 8 of its 36 small sparse-reverse ones, and Linux/GCC 11 small sparse. At 5 KiB per document, Jsonifier's fixed per-parse cost is the next target.
- **Number-heavy documents in order** remain a common loss. Canada, Marine IK and Mesh lose in order on both GCC builds and on MSVC (minified, by −3.5% to −4.9%). The cause is float-array throughput rather than out-of-order access, so it is out of scope for the field index.
- **Instruments streams** lose on four builds: read in order in NDJSON form on Linux/GCC (−5.3%) and macOS/Clang (−7.6%), and in reverse in NDJSON form on Linux/Clang (−11.0%), Linux/GCC (−24.8%) and macOS/GCC (−14.1%) and in comma form on Linux/GCC (−4.8%), although the single-document Instruments Reverse tests win on every build.
- **Some results did not converge.** 43 of the 1,490 single-document results did not converge: 31 on macOS/Clang, 6 on macOS/GCC, 4 on Windows/MSVC and 2 on Linux/Clang. 19 of the 130 streaming results did not converge: 10 on macOS/GCC, 6 on macOS/Clang and 3 on Windows/MSVC. Linux/GCC converged on every test. These results stay in the tallies and are marked (n/c) in §7 and §8. Most of them come from the two NEON builds, whose limits are looser (RSE < 10%, shift < 5%), so read those cells with that in mind.
- **The thresholds were tuned on this suite.** *T* was selected by sweeping {2, 4, 8, 16} on the full-size benchmarks, before the wrap gate and the small documents were added, and MSVC was not swept. The wrap count of 2 was not swept.
- **Comma-separated streams still walk each document up front.** The local boundary rule in §8.2 only holds for whitespace-delimited streams, so comma mode finds each document's end with a depth walk before handing it out, and the caller then walks the same structurals again. On the x86 in-order corpus streams this leaves comma mode 6.1% to 27.5% slower than NDJSON mode on the same records. The rule also assumes the input is valid: inside a malformed container, two adjacent values with no separator would be split into separate documents, and the reader would then report the errors. Reverse reads still pay a walk from the first key to the end of the object when the stream advances, the same walk an up-front boundary pass would pay when handing the document out.
- **Streams were compared unthreaded and fresh only.** simdjson can run stage 1 for the next window on a second thread; Jsonifier has no threaded mode, so §8 compares both single-threaded. The streaming tests also have no reused-output variant.
- **A known inefficiency remains in the miss path.** A lookup for a key that doesn't exist still ends with a full escape-aware rescan of the object, even after the object is indexed. It does not affect correctness and is a candidate for follow-up work.
- **The output comparison predates the measured commits.** The byte-for-byte comparison in §6 ran on `3e7bf6d`, not on the commits measured here, and it does not cover the small-document or reused tests.

## 10. Conclusion

A schema-free, tape-driven parser can match simdjson On Demand on its home ground and beat it outright once the access pattern stops being strictly forward. `jsonifier::generic` keeps On Demand's shape and relaxes its single-use restriction. Four pieces carry that result:

- a stage-1 reader tuned for small documents;
- a shared forward cursor, so reads in document order stay cheap;
- zero-copy strings with a fixed-slot arena;
- a SIMD field index that is only built once an object has shown it needs one.

Across 1,490 single-document results on five builds, covering full-size and small documents, each read into both freshly allocated and reused output objects, Jsonifier won 1,232, tied 57 and lost 201. On reverse reads it won 333 of 360 at both sizes, and on Windows/MSVC it lost 5 single-document results of 298, by 3.5% to 11.4%. Reusing the output objects speeds up both libraries by similar amounts and leaves the verdicts largely unchanged (621-28-96 fresh, 611-29-105 reused). The main remaining targets are macOS/Clang, with 66 of the 201 losses on a virtualized host, Linux/GCC, with 63, and small documents on Linux.

The design also carries over to streams. Because each streamed document is an ordinary `generic::document` bound to the parser's shared cursor, `iterateMany` needed no new reading machinery, only windowing and document boundaries. The one lesson it added repeats §5's: finding every document's end up front makes an in-order reader pay for two passes over the tape, and letting the reader's own cursor report where each document ended removes that pass for whitespace-delimited streams. Across 130 streaming results, Jsonifier won 113, tied 3 and lost 14, and it won every streaming test on Windows/MSVC. Instruments streams and the Stream Formats tests on macOS/Clang are the main remaining streaming targets.

The main engineering lesson is in §5. An index over the whole document made the target workload 2.3x *slower*, because most objects are small. The version that works builds the index lazily, per object, and only once that object has shown both that it is large and that it is being read out of order: enough skipped keys to clear *T*, and two lookups that had to wrap around. In short: make the in-order read cost nothing, and only pay for an index on objects whose access pattern shows it is needed.

## 11. Dedication

This paper is dedicated to the author's Little Buddy.