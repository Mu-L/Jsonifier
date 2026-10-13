# Two Stages, On Demand: The Stage-1 + Stage-2 Architecture in Jsonifier

**Nihilai Collective Corp — Engineering Papers**  
*Nihilai Collective Corp*  
*October 2026 — Jsonifier*  

---

## Abstract

Since Langdale and Lemire's 2019 paper *Parsing Gigabytes of JSON per Second*, the two-stage SIMD parsing model — a vectorized structural-indexing pass (stage 1) followed by a tape-driven materialization pass (stage 2) — has been treated as the canonical architecture for high-performance JSON processing. simdjson, the reference implementation, routes every document through stage 1 unconditionally.

Jsonifier takes a different position: **two-stage parsing is a specialized tool, not a mandatory front door.** When parsing a full document into concrete, reflection-registered C++ types, every structural fact the tape would record is rediscovered anyway during value materialization — so building the tape means touching every byte twice for information used once. Jsonifier therefore parses full documents in a single fused pass, and reserves its stage-1 + stage-2 machinery for the workloads where a structural index genuinely pays for itself: partial/unordered reading, prettifying, and minifying. Measured with both paths compiled into one binary across 225 tests on five builds, skipping the tape wins on POD-type data on every build and on 97 of the 100 minified documents, the tape is faster on 57 of the 60 prettified documents on x86, and on the Apple M1 the fused path wins 30 of the 40 prettified documents.

This paper describes both halves of that architecture: the single-pass primary path, and Jsonifier's stage-1 implementation — including its per-compiler-tuned step geometry, its fold-expression drain architecture for bitmask-to-index extraction, and its distributed UTF-8 validation strategy — with direct comparisons to simdjson's current design throughout.

---

## 1. Background: the canonical two-stage model

simdjson's pipeline is well documented. Stage 1 sweeps the input in 64-byte blocks, computing per-block bitmasks: backslashes, odd-length escape terminations, quotes, an in-string range mask derived via carry-less multiplication (prefix XOR), structural characters (`{ } [ ] : ,`), whitespace, and pseudo-structural scalar starts. The surviving bits are converted to byte offsets — the *structural tape* — via a tzcnt/blsr extraction loop. Stage 2 then walks the tape to build a DOM or, in the On Demand API, lazily materializes only the values the caller touches.

The model's strength is that stage 1 is branch-free and data-parallel; its cost is that it is unconditional. Every byte of every document is classified and indexed before a single value is parsed, regardless of whether the caller's access pattern needs the index at all.

## 2. The Jsonifier position: pay for the tape only when the tape pays for you

Jsonifier's primary API parses JSON directly into user-defined structs registered through compile-time reflection (`jsonifier::core<T>`). In that setting the parser already knows, at compile time, the expected shape of the document: which keys exist, their serialized order, their types. The parse loop is a schema-directed walk, not a blind tree construction.

Consider what the structural tape provides: the positions of every brace, bracket, colon, comma, quote, and scalar start. Now consider what a schema-directed single-pass parser does at each of those same positions: it is *already there*, with its iterator parked on that exact byte, consuming it as part of matching a known key literal or delimiting a value. The tape's information is a strict subset of what the fused pass discovers for free in program order. Building it first means that, for a full-document parse in which every value is materialized:

- every byte of the input is loaded twice (once in stage 1, once in stage 2),
- the tape itself is written and re-read (cache traffic proportional to structural density), and
- stage 2's control flow is driven by indirect loads through the index array rather than by a pointer walking linearly through memory the prefetcher already understands.

(We scope the double-load claim deliberately: a two-stage consumer that *skips* content — simdjson's On Demand API skipping unrequested fields — does not reload the skipped bytes in its second pass. That is exactly the workload where the tape earns its keep, and exactly why Jsonifier retains the two-stage machinery for partial reading. The accounting above describes the full-materialization case, where nothing is skipped and the tape's information is fully redundant with the walk.)

For full-document parsing into known types, the tape is overhead in principle, and in practice whether skipping it is faster comes down to two questions: is the input indented, and is the machine x86? §2.1 through §2.4 measure both paths in one binary on five platform/compiler builds. In the freshly allocated run, the fused path wins all 25 POD-type tests and 97 of the 100 minified documents (full-size and Small) across all five builds; the tape takes Canada and Mesh on Linux/GCC, and minified Instruments on macOS/GCC is a tie. The two-stage path wins 57 of the 60 prettified documents on x86, and on the M1 the fused path wins 30 of the 40 prettified documents, the tape wins 7 and three are ties.

**The routing rule is simple: the two-stage machinery is engaged for partial reading, prettifying, and minifying — workloads where the caller does *not* want every value, or wants pure structural transformation. Full-document parsing takes the single-pass path.** §2.4 shows where the measurements say that rule should be refined.

One anticipated objection deserves preemption here: that Jsonifier's requirement of ahead-of-time registration (`jsonifier::core<T>`) concedes generality that simdjson retains, since simdjson parses arbitrary documents with no such declaration. For truly dynamic workloads — schemas unknown until runtime, exploratory traversal, structural transformation of unknown documents — this is correct, and simdjson's DOM and On Demand models are the appropriate tools; Jsonifier's registration model simply does not address that problem. But for the workload this paper concerns — parsing documents into concrete types the caller has defined — the objection dissolves on inspection, because the schema knowledge exists at compile time in both programs. A simdjson caller materializing a struct writes the schema into their source as a sequence of field accesses in a fixed order chosen at authoring time; that traversal code is a schema declaration in imperative clothing. The difference is not the presence of compile-time knowledge but its legibility to the library: expressed as hand-written traversal, the knowledge is opaque — simdjson cannot fuse key literals from it, cannot learn permuted orders through it, and cannot skip building the index it implies is unnecessary. Expressed as a reflection registration, the identical knowledge becomes architecture: fused member headers, adaptive order recovery, and the routing rule above. The comparison between the two libraries on known-type workloads is therefore not "declared schema versus no schema" — it is the same schema, declared once where the compiler can consume it versus restated per call site where it cannot.

### 2.1 Method: both paths in one binary

Every comparison in this section comes from the [Json-Performance](https://github.com/nihilai-collective/Json-Performance) sweep published October 16, 2026, with simdjson [9c00353](https://github.com/simdjson/simdjson/commit/9c00353) and BenchmarkSuite [ae63a25](https://github.com/nihilai-collective/benchmarksuite/commit/ae63a25) on every build. Jsonifier is [3b56371](https://github.com/nihilai-collective/jsonifier/commit/3b56371) on four builds and [06bd73a](https://github.com/nihilai-collective/jsonifier/commit/06bd73a) on macOS/Clang. The harness registers both Jsonifier paths in the same binary. "jsonifier" is the default fused single-pass path, which the harness calls scalar structural iteration. "jsonifier (two-stage)" makes exactly the same `parseJson` call with `partialRead` set, which routes it through stage 1 and then stage 2. "simdjson (ondemand)" is the reference for §2.2 to §2.4. Glaze runs in the same sweep and appears here only in the validation test of §2.7, and `simdjson (reflection)`, simdjson 5's C++26 static-reflection reader, gets its own comparison in §2.5. Because the two Jsonifier paths are compiled together, run back to back on each test, and ranked against each other by the same statistics, the only thing that differs between them is the path.

All libraries parse fully into the target data structures and perform UTF-8 validation. Every test runs twice. In the freshly allocated run, every iteration constructs a new object to parse into and destroys it again inside the timed region, so allocation and deallocation are part of each measurement. In the reused run, labelled "(Reused)" in the sweep, the object is created once and held across iterations; it is cleared, keeping its capacity, outside the timed region before each iteration, so only the parse itself is measured. Parser instances are reused in both. §2.2, §2.3, §2.5 and §2.6 give both runs; the discussion in §2.4 quotes the freshly allocated run unless it says otherwise. Each build runs 45 full-parse tests: five POD-type tests (arrays of a single value type: Bool, Double, Int64, String, Uint64), ten corpus documents in minified and prettified form, and "Small" cut-down copies of the same ten documents (at most 5 KiB minified) in both forms, which weigh per-call overhead more heavily than the full-size documents. Two of the ten documents, "Marine IK Reverse" and its Small copy, request every key in the reverse of its document order; in every other test, all libraries receive keys in document order. The sweep also runs a partial-reading test on the Twitter document and its Small copy, which §2.6 covers, and a validation-only test, which §2.7 covers.

Sampling is adaptive. Iterations start at 100 and double each epoch, and sampling does not stop early: epochs continue until 5 seconds have elapsed or the iteration cap of 100,000 is reached. Every epoch after the first is scored by its relative standard error plus its epoch-over-epoch mean shift, and the lowest-scoring epoch is kept as the result. A result counts as converged only if that epoch has RSE below 5% and mean shift below 2.5% on the three x86 builds (10% and 5% on the virtualized M1), and non-converged results stay in the rankings and tallies, marked n/c in the tables. Twenty-one full-parse test rows in this sweep have at least one of the three compared libraries not converged: eleven freshly allocated and ten reused, all on the two macOS builds. Each one is marked n/c where it appears. Ties are declared by Welch's t-test on the kept epoch (two-sided, p < 0.05), and every verdict in this paper is a pairwise test between the two libraries or paths named in its column. Two properties of this rule matter for reading the results. Keeping the quietest epoch favors the least-disturbed stretch of each run, which raises absolute throughput somewhat, but it is applied identically to every library. And because the kept epoch has the smallest variance available, small differences are more often resolved as wins or losses than they would be under a first-to-converge rule: this sweep produced five ties between the two Jsonifier paths across 225 freshly allocated tests and seven across 225 reused ones.

### 2.2 Fused against two-stage

Ranked head to head, the fused path wins 154 tests, 5 are statistical ties, and the two-stage path wins 66. The split is a matter of platform and indentation:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 0 | 20 | 45 of 45 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 26 | 1 | 18 | 45 of 45 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 24 | 0 | 21 | 45 of 45 |
| macOS / GCC 16.2 (Apple M1, NEON) | 38 | 2 | 5 | 38 of 45 |
| macOS / Clang 23.1 (Apple M1, NEON) | 41 | 2 | 2 | 41 of 45 |
| **Aggregate** | **154** | **5** | **66** | **214 of 225** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 0 | 18 / 0 / 2 | 19 / 1 / 0 |
| Prettified corpus documents | 0 / 0 / 10 | 2 / 1 / 17 | 16 / 1 / 3 |
| Small minified documents | 10 / 0 / 0 | 20 / 0 / 0 | 20 / 0 / 0 |
| Small prettified documents | 0 / 0 / 10 | 0 / 0 / 20 | 14 / 2 / 4 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 45 / 0 / 0 | 45 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 44 / 0 / 1 | 37 / 0 / 8 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 43 / 0 / 2 | 43 / 0 / 2 |
| macOS / GCC 16.2 (Apple M1, NEON) | 44 / 0 / 1 | 43 / 1 / 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 45 / 0 / 0 | 45 / 0 / 0 |
| **Aggregate** | **221 / 0 / 4** | **213 / 1 / 11** |

**Reused objects.** With the target object held across iterations:

| Platform / Compiler | Fused faster | Tie | Two-stage faster | Tests converged |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 25 | 0 | 20 | 45 of 45 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 27 | 1 | 17 | 45 of 45 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 23 | 1 | 21 | 45 of 45 |
| macOS / GCC 16.2 (Apple M1, NEON) | 39 | 3 | 3 | 39 of 45 |
| macOS / Clang 23.1 (Apple M1, NEON) | 41 | 2 | 2 | 41 of 45 |
| **Aggregate** | **155** | **7** | **63** | **215 of 225** |

Split by input shape (fused / tie / two-stage):

| Input shape | Windows / MSVC | Linux (Clang + GCC) | macOS (GCC + Clang) |
|---|---|---|---|
| POD-type tests | 5 / 0 / 0 | 10 / 0 / 0 | 10 / 0 / 0 |
| Minified corpus documents | 10 / 0 / 0 | 18 / 0 / 2 | 19 / 0 / 1 |
| Prettified corpus documents | 0 / 0 / 10 | 2 / 2 / 16 | 16 / 3 / 1 |
| Small minified documents | 10 / 0 / 0 | 20 / 0 / 0 | 20 / 0 / 0 |
| Small prettified documents | 0 / 0 / 10 | 0 / 0 / 20 | 15 / 2 / 3 |

Against simdjson, counting a test for Jsonifier by the verdict of whichever of its two paths has the higher throughput against simdjson On Demand:

| Platform / Compiler | Best Jsonifier path (W / T / L) | Fused path alone (W / T / L) |
|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 45 / 0 / 0 | 45 / 0 / 0 |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 45 / 0 / 0 | 37 / 0 / 8 |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 42 / 1 / 2 | 42 / 0 / 3 |
| macOS / GCC 16.2 (Apple M1, NEON) | 44 / 0 / 1 | 44 / 0 / 1 |
| macOS / Clang 23.1 (Apple M1, NEON) | 45 / 0 / 0 | 45 / 0 / 0 |
| **Aggregate** | **221 / 1 / 3** | **213 / 0 / 12** |

Across 225 tests the fused path wins 155, ties 7 and loses 63 to the two-stage path, and the x86 split holds: the tape wins 26 of the 30 full-size prettified documents and all 30 Small prettified ones, and the fused path wins all 30 Small minified documents and 28 of the 30 full-size minified ones (the tape takes Canada and Mesh on Linux/GCC). On the M1 the fused path wins all 20 Small minified documents and 19 of the 20 full-size minified ones (the tape takes Marine IK on macOS/GCC), and 31 of the 40 prettified documents, with 5 ties and 4 tape wins. Against simdjson, the faster Jsonifier path wins 221, ties 1 and loses 3, and the fused path alone wins 213 and loses 12.

### 2.3 Per-test results

Throughput is in MB/s. "Faster Jsonifier path" is the Welch's t-test verdict between the two Jsonifier paths, "Best Jsonifier path vs simdjson" is the verdict for whichever of the two has the higher throughput.

#### Freshly allocated objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 546 | 129 | 122 | 4.22× | Fused | Win |
| Double (POD) | 547 | 77 | 169 | 7.14× | Fused | Win |
| Int64 (POD) | 1,514 | 541 | 375 | 2.80× | Fused | Win |
| String (POD) | 899 | 660 | 607 | 1.36× | Fused | Win |
| Uint64 (POD) | 1,495 | 563 | 379 | 2.66× | Fused | Win |
| Canada (minified) | 588 | 548 | 363 | 1.07× | Fused | Win |
| Canada (prettified) | 1,560 | 1,654 | 1,121 | 0.94× | Two-stage | Win |
| CitmCatalog (minified) | 1,513 | 1,232 | 518 | 1.23× | Fused | Win |
| CitmCatalog (prettified) | 2,592 | 3,094 | 1,397 | 0.84× | Two-stage | Win |
| Discord (minified) | 1,152 | 1,014 | 611 | 1.14× | Fused | Win |
| Discord (prettified) | 1,376 | 1,603 | 990 | 0.86× | Two-stage | Win |
| Google Maps Response (minified) | 1,173 | 1,135 | 506 | 1.03× | Fused | Win |
| Google Maps Response (prettified) | 2,236 | 2,651 | 1,269 | 0.84× | Two-stage | Win |
| Instruments (minified) | 1,982 | 1,385 | 662 | 1.43× | Fused | Win |
| Instruments (prettified) | 2,077 | 2,545 | 1,254 | 0.82× | Two-stage | Win |
| Marine IK Reverse (minified) | 490 | 437 | 36 | 1.12× | Fused | Win |
| Marine IK Reverse (prettified) | 1,980 | 2,123 | 194 | 0.93× | Two-stage | Win |
| Marine IK (minified) | 504 | 440 | 254 | 1.15× | Fused | Win |
| Marine IK (prettified) | 2,023 | 2,171 | 1,263 | 0.93× | Two-stage | Win |
| Mesh (minified) | 928 | 815 | 424 | 1.14× | Fused | Win |
| Mesh (prettified) | 1,198 | 1,476 | 789 | 0.81× | Two-stage | Win |
| Random (minified) | 863 | 854 | 518 | 1.01× | Fused | Win |
| Random (prettified) | 1,360 | 1,564 | 943 | 0.87× | Two-stage | Win |
| Twitter (minified) | 1,241 | 1,122 | 799 | 1.11× | Fused | Win |
| Twitter (prettified) | 1,337 | 1,621 | 1,174 | 0.83× | Two-stage | Win |
| Canada Small (minified) | 574 | 532 | 326 | 1.08× | Fused | Win |
| Canada Small (prettified) | 1,477 | 1,582 | 977 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 954 | 805 | 467 | 1.18× | Fused | Win |
| CitmCatalog Small (prettified) | 1,534 | 1,789 | 1,034 | 0.86× | Two-stage | Win |
| Discord Small (minified) | 1,067 | 939 | 639 | 1.14× | Fused | Win |
| Discord Small (prettified) | 1,236 | 1,384 | 911 | 0.89× | Two-stage | Win |
| Google Maps Response Small (minified) | 1,082 | 999 | 522 | 1.08× | Fused | Win |
| Google Maps Response Small (prettified) | 2,089 | 2,431 | 1,297 | 0.86× | Two-stage | Win |
| Instruments Small (minified) | 1,899 | 1,331 | 674 | 1.43× | Fused | Win |
| Instruments Small (prettified) | 1,928 | 2,441 | 1,297 | 0.79× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 426 | 379 | 39 | 1.12× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,717 | 1,867 | 205 | 0.92× | Two-stage | Win |
| Marine IK Small (minified) | 451 | 397 | 251 | 1.13× | Fused | Win |
| Marine IK Small (prettified) | 1,794 | 1,876 | 1,244 | 0.96× | Two-stage | Win |
| Mesh Small (minified) | 681 | 619 | 230 | 1.10× | Fused | Win |
| Mesh Small (prettified) | 1,023 | 1,251 | 503 | 0.82× | Two-stage | Win |
| Random Small (minified) | 1,002 | 975 | 510 | 1.03× | Fused | Win |
| Random Small (prettified) | 1,491 | 1,753 | 972 | 0.85× | Two-stage | Win |
| Twitter Small (minified) | 1,499 | 1,267 | 971 | 1.18× | Fused | Win |
| Twitter Small (prettified) | 1,482 | 1,833 | 1,429 | 0.81× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,898 | 337 | 227 | 5.62× | Fused | Win |
| Double (POD) | 1,325 | 414 | 259 | 3.20× | Fused | Win |
| Int64 (POD) | 3,053 | 959 | 583 | 3.18× | Fused | Win |
| String (POD) | 1,759 | 1,163 | 1,149 | 1.51× | Fused | Win |
| Uint64 (POD) | 3,382 | 964 | 566 | 3.51× | Fused | Win |
| Canada (minified) | 1,027 | 901 | 668 | 1.14× | Fused | Win |
| Canada (prettified) | 2,687 | 2,709 | 1,999 | 0.99× | Two-stage | Win |
| CitmCatalog (minified) | 2,480 | 1,857 | 1,247 | 1.34× | Fused | Win |
| CitmCatalog (prettified) | 3,880 | 4,436 | 3,090 | 0.87× | Two-stage | Win |
| Discord (minified) | 2,291 | 1,787 | 1,453 | 1.28× | Fused | Win |
| Discord (prettified) | 2,115 | 2,844 | 2,390 | 0.74× | Two-stage | Win |
| Google Maps Response (minified) | 2,664 | 1,970 | 1,378 | 1.35× | Fused | Win |
| Google Maps Response (prettified) | 3,674 | 4,308 | 3,244 | 0.85× | Two-stage | Win |
| Instruments (minified) | 2,712 | 1,982 | 1,731 | 1.37× | Fused | Win |
| Instruments (prettified) | 2,857 | 3,483 | 3,131 | 0.82× | Two-stage | Win |
| Marine IK Reverse (minified) | 859 | 750 | 615 | 1.15× | Fused | Win |
| Marine IK Reverse (prettified) | 3,528 | 3,486 | 2,872 | 1.01× | Fused | Win |
| Marine IK (minified) | 874 | 755 | 615 | 1.16× | Fused | Win |
| Marine IK (prettified) | 3,537 | 3,543 | 2,883 | 1.00× | Tie | Win |
| Mesh (minified) | 1,456 | 1,207 | 1,169 | 1.21× | Fused | Win |
| Mesh (prettified) | 2,025 | 2,278 | 2,151 | 0.89× | Two-stage | Win |
| Random (minified) | 1,686 | 1,433 | 1,129 | 1.18× | Fused | Win |
| Random (prettified) | 2,252 | 2,532 | 2,008 | 0.89× | Two-stage | Win |
| Twitter (minified) | 2,038 | 1,833 | 1,774 | 1.11× | Fused | Win |
| Twitter (prettified) | 2,040 | 2,615 | 2,501 | 0.78× | Two-stage | Win |
| Canada Small (minified) | 1,035 | 942 | 732 | 1.10× | Fused | Win |
| Canada Small (prettified) | 2,547 | 2,740 | 2,164 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,920 | 1,550 | 1,262 | 1.24× | Fused | Win |
| CitmCatalog Small (prettified) | 2,697 | 3,162 | 2,669 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 2,087 | 1,805 | 1,473 | 1.16× | Fused | Win |
| Discord Small (prettified) | 2,034 | 2,633 | 2,135 | 0.77× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,453 | 1,832 | 1,387 | 1.34× | Fused | Win |
| Google Maps Response Small (prettified) | 3,448 | 4,033 | 3,264 | 0.85× | Two-stage | Win |
| Instruments Small (minified) | 2,741 | 1,957 | 1,600 | 1.40× | Fused | Win |
| Instruments Small (prettified) | 2,827 | 3,489 | 2,997 | 0.81× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 836 | 727 | 594 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 3,037 | 3,433 | 2,798 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 923 | 791 | 595 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 3,225 | 3,672 | 2,815 | 0.88× | Two-stage | Win |
| Mesh Small (minified) | 1,018 | 866 | 740 | 1.18× | Fused | Win |
| Mesh Small (prettified) | 1,533 | 1,723 | 1,540 | 0.89× | Two-stage | Win |
| Random Small (minified) | 2,293 | 1,806 | 1,202 | 1.27× | Fused | Win |
| Random Small (prettified) | 2,619 | 3,215 | 2,170 | 0.81× | Two-stage | Win |
| Twitter Small (minified) | 2,655 | 2,372 | 2,324 | 1.12× | Fused | Win |
| Twitter Small (prettified) | 2,367 | 3,148 | 3,161 | 0.75× | Two-stage | Loss |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,827 | 275 | 184 | 6.64× | Fused | Win |
| Double (POD) | 1,160 | 344 | 233 | 3.38× | Fused | Win |
| Int64 (POD) | 3,350 | 779 | 531 | 4.30× | Fused | Win |
| String (POD) | 1,798 | 1,069 | 1,044 | 1.68× | Fused | Win |
| Uint64 (POD) | 3,382 | 860 | 522 | 3.93× | Fused | Win |
| Canada (minified) | 920 | 932 | 793 | 0.99× | Two-stage | Win |
| Canada (prettified) | 2,489 | 2,703 | 2,323 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 2,492 | 1,904 | 1,391 | 1.31× | Fused | Win |
| CitmCatalog (prettified) | 4,073 | 4,349 | 3,422 | 0.94× | Two-stage | Win |
| Discord (minified) | 2,264 | 1,856 | 1,437 | 1.22× | Fused | Win |
| Discord (prettified) | 2,529 | 2,861 | 2,322 | 0.88× | Two-stage | Win |
| Google Maps Response (minified) | 2,683 | 1,965 | 1,129 | 1.37× | Fused | Win |
| Google Maps Response (prettified) | 4,081 | 4,190 | 2,594 | 0.97× | Two-stage | Win |
| Instruments (minified) | 3,444 | 2,301 | 1,415 | 1.50× | Fused | Win |
| Instruments (prettified) | 3,485 | 3,883 | 2,585 | 0.90× | Two-stage | Win |
| Marine IK Reverse (minified) | 756 | 728 | 620 | 1.04× | Fused | Win |
| Marine IK Reverse (prettified) | 3,357 | 3,334 | 2,817 | 1.01× | Fused | Win |
| Marine IK (minified) | 775 | 734 | 614 | 1.06× | Fused | Win |
| Marine IK (prettified) | 3,305 | 3,332 | 2,904 | 0.99× | Two-stage | Win |
| Mesh (minified) | 1,087 | 1,169 | 1,190 | 0.93× | Two-stage | Loss |
| Mesh (prettified) | 1,841 | 2,102 | 2,190 | 0.88× | Two-stage | Loss |
| Random (minified) | 1,456 | 1,402 | 855 | 1.04× | Fused | Win |
| Random (prettified) | 2,090 | 2,393 | 1,547 | 0.87× | Two-stage | Win |
| Twitter (minified) | 2,047 | 1,740 | 1,347 | 1.18× | Fused | Win |
| Twitter (prettified) | 2,006 | 2,580 | 1,950 | 0.78× | Two-stage | Win |
| Canada Small (minified) | 1,071 | 963 | 778 | 1.11× | Fused | Win |
| Canada Small (prettified) | 2,612 | 2,760 | 2,251 | 0.95× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,938 | 1,598 | 1,228 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 2,789 | 3,276 | 2,590 | 0.85× | Two-stage | Win |
| Discord Small (minified) | 1,916 | 1,629 | 1,308 | 1.18× | Fused | Win |
| Discord Small (prettified) | 1,991 | 2,358 | 1,911 | 0.84× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,456 | 1,810 | 1,129 | 1.36× | Fused | Win |
| Google Maps Response Small (prettified) | 3,778 | 4,023 | 2,698 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,391 | 2,174 | 1,256 | 1.56× | Fused | Win |
| Instruments Small (prettified) | 3,323 | 3,759 | 2,386 | 0.88× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 811 | 701 | 571 | 1.16× | Fused | Win |
| Marine IK Reverse Small (prettified) | 3,037 | 3,337 | 2,716 | 0.91× | Two-stage | Win |
| Marine IK Small (minified) | 913 | 750 | 573 | 1.22× | Fused | Win |
| Marine IK Small (prettified) | 3,186 | 3,479 | 2,691 | 0.92× | Two-stage | Win |
| Mesh Small (minified) | 1,006 | 875 | 738 | 1.15× | Fused | Win |
| Mesh Small (prettified) | 1,529 | 1,695 | 1,512 | 0.90× | Two-stage | Win |
| Random Small (minified) | 2,180 | 1,776 | 895 | 1.23× | Fused | Win |
| Random Small (prettified) | 2,824 | 3,110 | 1,671 | 0.91× | Two-stage | Win |
| Twitter Small (minified) | 2,935 | 2,186 | 1,634 | 1.34× | Fused | Win |
| Twitter Small (prettified) | 2,402 | 3,092 | 2,298 | 0.78× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,503 | 227 | 202 | 6.61× | Fused | Win |
| Double (POD) | 948 | 314 | 155 | 3.02× | Fused | Win |
| Int64 (POD) | 2,230 | 649 | 471 | 3.44× | Fused | Win |
| String (POD) | 1,187 | 708 | 771 | 1.68× | Fused | Win |
| Uint64 (POD) | 1,923 | 530 | 413 | 3.63× | Fused | Win |
| Canada (minified) | 626 | 568 | 413 | 1.10× | Fused | Win |
| Canada (prettified) | 1,625 | 1,561 | 1,205 | 1.04× | Fused | Win |
| CitmCatalog (minified) | 1,806 | 1,148 | 990 | 1.57× | Fused | Win |
| CitmCatalog (prettified) | 2,952 | 2,801 | 2,405 | 1.05× | Fused | Win |
| Discord (minified) | 1,619 | 1,039 | 930 | 1.56× | Fused | Win |
| Discord (prettified) | 1,823 | 1,761 | 1,433 | 1.04× | Fused | Win |
| Google Maps Response (minified) | 1,728 | 1,043 | 592 | 1.66× | Fused | Win |
| Google Maps Response (prettified) | 2,694 | 1,833 | 1,376 (n/c) | 1.47× | Fused | Win |
| Instruments (minified) | 1,400 | 1,395 | 1,021 | 1.00× | Tie | Win |
| Instruments (prettified) | 2,715 | 2,296 | 1,808 | 1.18× | Fused | Win |
| Marine IK Reverse (minified) | 444 (n/c) | 403 | 343 (n/c) | 1.10× | Fused | Win |
| Marine IK Reverse (prettified) | 2,212 | 1,794 | 1,626 | 1.23× | Fused | Win |
| Marine IK (minified) | 458 (n/c) | 420 | 380 (n/c) | 1.09× | Fused | Win |
| Marine IK (prettified) | 1,893 | 2,146 | 1,528 (n/c) | 0.88× | Two-stage | Win |
| Mesh (minified) | 917 | 751 | 730 | 1.22× | Fused | Win |
| Mesh (prettified) | 969 (n/c) | 1,230 | 1,295 | 0.79× | Two-stage | Loss |
| Random (minified) | 1,031 | 738 | 554 (n/c) | 1.40× | Fused | Win |
| Random (prettified) | 1,590 | 879 | 932 | 1.81× | Fused | Win |
| Twitter (minified) | 1,667 | 1,400 | 987 | 1.19× | Fused | Win |
| Twitter (prettified) | 1,946 | 1,807 | 1,353 | 1.08× | Fused | Win |
| Canada Small (minified) | 724 | 564 | 348 | 1.28× | Fused | Win |
| Canada Small (prettified) | 1,211 | 1,573 | 1,196 | 0.77× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,099 | 956 | 652 | 1.15× | Fused | Win |
| CitmCatalog Small (prettified) | 1,970 | 1,833 | 1,585 | 1.07× | Fused | Win |
| Discord Small (minified) | 1,192 | 1,129 | 867 | 1.06× | Fused | Win |
| Discord Small (prettified) | 1,521 | 994 | 767 (n/c) | 1.53× | Fused | Win |
| Google Maps Response Small (minified) | 1,283 | 904 | 629 | 1.42× | Fused | Win |
| Google Maps Response Small (prettified) | 2,531 | 1,971 | 1,530 | 1.28× | Fused | Win |
| Instruments Small (minified) | 2,296 | 1,213 | 988 | 1.89× | Fused | Win |
| Instruments Small (prettified) | 2,656 | 2,376 | 1,416 | 1.12× | Fused | Win |
| Marine IK Reverse Small (minified) | 482 | 470 | 285 | 1.03× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,068 | 2,017 | 1,371 | 1.03× | Fused | Win |
| Marine IK Small (minified) | 594 | 441 | 316 | 1.35× | Fused | Win |
| Marine IK Small (prettified) | 2,012 | 2,181 | 1,575 | 0.92× | Two-stage | Win |
| Mesh Small (minified) | 739 | 603 | 417 | 1.23× | Fused | Win |
| Mesh Small (prettified) | 1,180 | 1,252 | 764 | 0.94× | Two-stage | Win |
| Random Small (minified) | 1,371 | 908 | 607 | 1.51× | Fused | Win |
| Random Small (prettified) | 1,880 | 1,337 | 999 | 1.41× | Fused | Win |
| Twitter Small (minified) | 1,959 | 1,474 | 1,082 | 1.33× | Fused | Win |
| Twitter Small (prettified) | 2,205 | 2,124 | 1,447 | 1.04× | Tie | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 980 | 223 | 182 | 4.40× | Fused | Win |
| Double (POD) | 772 | 267 | 180 | 2.89× | Fused | Win |
| Int64 (POD) | 1,993 | 544 | 385 | 3.66× | Fused | Win |
| String (POD) | 1,061 | 610 | 747 | 1.74× | Fused | Win |
| Uint64 (POD) | 1,923 | 596 | 412 | 3.23× | Fused | Win |
| Canada (minified) | 632 | 564 | 391 | 1.12× | Fused | Win |
| Canada (prettified) | 1,672 | 1,684 | 1,134 (n/c) | 0.99× | Tie | Win |
| CitmCatalog (minified) | 1,934 | 1,210 | 828 | 1.60× | Fused | Win |
| CitmCatalog (prettified) | 2,807 | 2,719 | 1,623 | 1.03× | Fused | Win |
| Discord (minified) | 1,994 | 1,085 | 828 | 1.84× | Fused | Win |
| Discord (prettified) | 2,300 | 1,616 | 1,341 | 1.42× | Fused | Win |
| Google Maps Response (minified) | 2,108 | 1,012 | 937 | 2.08× | Fused | Win |
| Google Maps Response (prettified) | 3,213 | 2,149 | 2,244 | 1.49× | Fused | Win |
| Instruments (minified) | 2,500 | 1,592 | 1,076 | 1.57× | Fused | Win |
| Instruments (prettified) | 2,545 | 2,358 | 1,933 | 1.08× | Fused | Win |
| Marine IK Reverse (minified) | 589 | 486 | 367 | 1.21× | Fused | Win |
| Marine IK Reverse (prettified) | 2,359 | 2,113 | 1,639 | 1.12× | Fused | Win |
| Marine IK (minified) | 588 | 494 (n/c) | 329 | 1.19× | Fused | Win |
| Marine IK (prettified) | 2,313 (n/c) | 2,077 | 1,478 | 1.11× | Fused | Win |
| Mesh (minified) | 1,006 | 808 | 716 | 1.24× | Fused | Win |
| Mesh (prettified) | 1,340 | 1,438 | 1,303 (n/c) | 0.93× | Two-stage | Win |
| Random (minified) | 1,709 | 866 | 817 | 1.97× | Fused | Win |
| Random (prettified) | 2,255 | 1,240 | 1,444 | 1.82× | Fused | Win |
| Twitter (minified) | 1,611 | 1,506 | 1,481 | 1.07× | Fused | Win |
| Twitter (prettified) | 2,160 | 1,978 | 1,989 | 1.09× | Fused | Win |
| Canada Small (minified) | 653 | 598 | 383 | 1.09× | Fused | Win |
| Canada Small (prettified) | 1,583 | 1,380 | 1,093 | 1.15× | Fused | Win |
| CitmCatalog Small (minified) | 1,217 | 896 | 771 | 1.36× | Fused | Win |
| CitmCatalog Small (prettified) | 2,045 | 1,596 | 1,461 | 1.28× | Fused | Win |
| Discord Small (minified) | 1,657 | 1,094 | 1,074 | 1.51× | Fused | Win |
| Discord Small (prettified) | 1,999 | 1,540 | 1,652 | 1.30× | Fused | Win |
| Google Maps Response Small (minified) | 1,847 | 1,001 | 827 | 1.85× | Fused | Win |
| Google Maps Response Small (prettified) | 2,856 | 1,886 | 2,066 | 1.51× | Fused | Win |
| Instruments Small (minified) | 2,462 | 1,371 | 1,082 | 1.80× | Fused | Win |
| Instruments Small (prettified) | 2,819 | 2,551 | 1,991 | 1.11× | Fused | Win |
| Marine IK Reverse Small (minified) | 546 | 473 | 321 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,141 | 1,938 | 1,468 | 1.10× | Fused | Win |
| Marine IK Small (minified) | 610 | 468 | 317 | 1.30× | Fused | Win |
| Marine IK Small (prettified) | 2,438 | 2,127 | 1,555 | 1.15× | Fused | Win |
| Mesh Small (minified) | 721 | 618 | 417 | 1.17× | Fused | Win |
| Mesh Small (prettified) | 1,147 | 1,190 | 873 | 0.96× | Two-stage | Win |
| Random Small (minified) | 1,952 | 993 | 919 | 1.97× | Fused | Win |
| Random Small (prettified) | 2,465 | 1,416 | 1,632 | 1.74× | Fused | Win |
| Twitter Small (minified) | 2,192 | 1,581 | 1,403 | 1.39× | Fused | Win |
| Twitter Small (prettified) | 1,875 | 1,886 | 1,787 | 0.99× | Tie | Win |

#### Reused objects

**Windows / MSVC 19.44 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 654 | 137 | 128 | 4.77× | Fused | Win |
| Double (POD) | 691 | 84 | 176 | 8.26× | Fused | Win |
| Int64 (POD) | 1,984 | 591 | 404 | 3.35× | Fused | Win |
| String (POD) | 1,594 | 1,004 | 853 | 1.59× | Fused | Win |
| Uint64 (POD) | 1,978 | 637 | 408 | 3.11× | Fused | Win |
| Canada (minified) | 704 | 648 | 399 | 1.09× | Fused | Win |
| Canada (prettified) | 1,801 | 1,947 | 1,241 | 0.93× | Two-stage | Win |
| CitmCatalog (minified) | 1,806 | 1,433 | 559 | 1.26× | Fused | Win |
| CitmCatalog (prettified) | 2,911 | 3,380 | 1,484 | 0.86× | Two-stage | Win |
| Discord (minified) | 1,584 | 1,370 | 764 | 1.16× | Fused | Win |
| Discord (prettified) | 1,780 | 2,168 | 1,226 | 0.82× | Two-stage | Win |
| Google Maps Response (minified) | 1,335 | 1,276 | 556 | 1.05× | Fused | Win |
| Google Maps Response (prettified) | 2,458 | 2,948 | 1,380 | 0.83× | Two-stage | Win |
| Instruments (minified) | 2,569 | 1,597 | 747 | 1.61× | Fused | Win |
| Instruments (prettified) | 2,382 | 2,968 | 1,382 | 0.80× | Two-stage | Win |
| Marine IK Reverse (minified) | 536 | 485 | 37 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | 2,102 | 2,269 | 193 | 0.93× | Two-stage | Win |
| Marine IK (minified) | 546 | 485 | 271 | 1.13× | Fused | Win |
| Marine IK (prettified) | 2,139 | 2,346 | 1,329 | 0.91× | Two-stage | Win |
| Mesh (minified) | 980 | 844 | 457 | 1.16× | Fused | Win |
| Mesh (prettified) | 1,246 | 1,549 | 859 | 0.80× | Two-stage | Win |
| Random (minified) | 1,235 | 1,213 | 645 | 1.02× | Fused | Win |
| Random (prettified) | 1,827 | 2,119 | 1,165 | 0.86× | Two-stage | Win |
| Twitter (minified) | 1,763 | 1,514 | 1,020 | 1.16× | Fused | Win |
| Twitter (prettified) | 1,739 | 2,155 | 1,455 | 0.81× | Two-stage | Win |
| Canada Small (minified) | 713 | 672 | 358 | 1.06× | Fused | Win |
| Canada Small (prettified) | 1,780 | 1,911 | 1,078 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,236 | 1,021 | 589 | 1.21× | Fused | Win |
| CitmCatalog Small (prettified) | 1,820 | 2,227 | 1,258 | 0.82× | Two-stage | Win |
| Discord Small (minified) | 1,461 | 1,249 | 850 | 1.17× | Fused | Win |
| Discord Small (prettified) | 1,578 | 1,837 | 1,178 | 0.86× | Two-stage | Win |
| Google Maps Response Small (minified) | 1,271 | 1,165 | 566 | 1.09× | Fused | Win |
| Google Maps Response Small (prettified) | 2,285 | 2,812 | 1,380 | 0.81× | Two-stage | Win |
| Instruments Small (minified) | 2,283 | 1,457 | 733 | 1.57× | Fused | Win |
| Instruments Small (prettified) | 2,055 | 2,688 | 1,419 | 0.76× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 504 | 462 | 39 | 1.09× | Fused | Win |
| Marine IK Reverse Small (prettified) | 1,980 | 2,212 | 205 | 0.90× | Two-stage | Win |
| Marine IK Small (minified) | 548 | 487 | 271 | 1.12× | Fused | Win |
| Marine IK Small (prettified) | 2,069 | 2,325 | 1,313 | 0.89× | Two-stage | Win |
| Mesh Small (minified) | 826 | 767 | 398 | 1.08× | Fused | Win |
| Mesh Small (prettified) | 1,234 | 1,490 | 815 | 0.83× | Two-stage | Win |
| Random Small (minified) | 1,236 | 1,182 | 628 | 1.05× | Fused | Win |
| Random Small (prettified) | 1,827 | 2,067 | 1,154 | 0.88× | Two-stage | Win |
| Twitter Small (minified) | 1,952 | 1,577 | 1,111 | 1.24× | Fused | Win |
| Twitter Small (prettified) | 1,854 | 2,224 | 1,611 | 0.83× | Two-stage | Win |

**Linux / Clang 24.0 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 2,020 | 336 | 232 | 6.02× | Fused | Win |
| Double (POD) | 1,450 | 426 | 263 | 3.41× | Fused | Win |
| Int64 (POD) | 3,384 | 987 | 586 | 3.43× | Fused | Win |
| String (POD) | 2,735 | 1,532 | 1,473 | 1.79× | Fused | Win |
| Uint64 (POD) | 3,783 | 990 | 574 | 3.82× | Fused | Win |
| Canada (minified) | 1,255 | 1,077 | 766 | 1.17× | Fused | Win |
| Canada (prettified) | 3,219 | 3,203 | 2,287 | 1.01× | Fused | Win |
| CitmCatalog (minified) | 3,054 | 2,198 | 1,449 | 1.39× | Fused | Win |
| CitmCatalog (prettified) | 4,358 | 5,135 | 3,532 | 0.85× | Two-stage | Win |
| Discord (minified) | 3,166 | 2,433 | 1,892 | 1.30× | Fused | Win |
| Discord (prettified) | 2,602 | 3,810 | 3,137 | 0.68× | Two-stage | Win |
| Google Maps Response (minified) | 3,120 | 2,196 | 1,495 | 1.42× | Fused | Win |
| Google Maps Response (prettified) | 3,942 | 4,644 | 3,459 | 0.85× | Two-stage | Win |
| Instruments (minified) | 3,285 | 2,262 | 1,911 | 1.45× | Fused | Win |
| Instruments (prettified) | 3,153 | 3,897 | 3,436 | 0.81× | Two-stage | Win |
| Marine IK Reverse (minified) | 956 | 822 | 670 | 1.16× | Fused | Win |
| Marine IK Reverse (prettified) | 3,846 | 3,798 | 3,127 | 1.01× | Fused | Win |
| Marine IK (minified) | 980 | 841 | 677 | 1.17× | Fused | Win |
| Marine IK (prettified) | 3,823 | 3,858 | 3,151 | 0.99× | Tie | Win |
| Mesh (minified) | 1,541 | 1,275 | 1,296 | 1.21× | Fused | Win |
| Mesh (prettified) | 2,119 | 2,398 | 2,365 | 0.88× | Two-stage | Win |
| Random (minified) | 2,520 | 2,050 | 1,446 | 1.23× | Fused | Win |
| Random (prettified) | 3,005 | 3,482 | 2,549 | 0.86× | Two-stage | Win |
| Twitter (minified) | 2,580 | 2,493 | 2,176 | 1.03× | Fused | Win |
| Twitter (prettified) | 2,368 | 3,278 | 3,056 | 0.72× | Two-stage | Win |
| Canada Small (minified) | 1,303 | 1,162 | 829 | 1.12× | Fused | Win |
| Canada Small (prettified) | 3,085 | 3,309 | 2,450 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,337 | 1,840 | 1,449 | 1.27× | Fused | Win |
| CitmCatalog Small (prettified) | 3,046 | 3,708 | 3,039 | 0.82× | Two-stage | Win |
| Discord Small (minified) | 2,769 | 2,248 | 1,936 | 1.23× | Fused | Win |
| Discord Small (prettified) | 2,443 | 3,200 | 2,830 | 0.76× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,911 | 2,070 | 1,567 | 1.41× | Fused | Win |
| Google Maps Response Small (prettified) | 3,752 | 4,498 | 3,606 | 0.83× | Two-stage | Win |
| Instruments Small (minified) | 2,979 | 2,100 | 1,777 | 1.42× | Fused | Win |
| Instruments Small (prettified) | 2,930 | 3,668 | 3,210 | 0.80× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,024 | 888 | 681 | 1.15× | Fused | Win |
| Marine IK Reverse Small (prettified) | 3,500 | 3,981 | 3,166 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 1,136 | 972 | 674 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 3,720 | 4,341 | 3,131 | 0.86× | Two-stage | Win |
| Mesh Small (minified) | 1,252 | 1,034 | 1,015 | 1.21× | Fused | Win |
| Mesh Small (prettified) | 1,772 | 2,026 | 2,020 | 0.87× | Two-stage | Win |
| Random Small (minified) | 2,860 | 2,165 | 1,450 | 1.32× | Fused | Win |
| Random Small (prettified) | 3,003 | 3,809 | 2,652 | 0.79× | Two-stage | Win |
| Twitter Small (minified) | 2,986 | 2,632 | 2,460 | 1.13× | Fused | Win |
| Twitter Small (prettified) | 2,606 | 3,596 | 3,408 | 0.72× | Two-stage | Win |

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,944 | 277 | 189 | 7.02× | Fused | Win |
| Double (POD) | 1,239 | 349 | 237 | 3.55× | Fused | Win |
| Int64 (POD) | 3,767 | 805 | 533 | 4.68× | Fused | Win |
| String (POD) | 2,718 | 1,328 | 1,258 | 2.05× | Fused | Win |
| Uint64 (POD) | 3,891 | 885 | 529 | 4.40× | Fused | Win |
| Canada (minified) | 1,107 | 1,117 | 952 | 0.99× | Two-stage | Win |
| Canada (prettified) | 2,926 | 3,175 | 2,767 | 0.92× | Two-stage | Win |
| CitmCatalog (minified) | 3,088 | 2,285 | 1,646 | 1.35× | Fused | Win |
| CitmCatalog (prettified) | 4,626 | 5,048 | 4,057 | 0.92× | Two-stage | Win |
| Discord (minified) | 3,346 | 2,513 | 1,835 | 1.33× | Fused | Win |
| Discord (prettified) | 3,167 | 3,751 | 2,883 | 0.84× | Two-stage | Win |
| Google Maps Response (minified) | 3,111 | 2,143 | 1,192 | 1.45× | Fused | Win |
| Google Maps Response (prettified) | 4,419 | 4,557 | 2,791 | 0.97× | Two-stage | Win |
| Instruments (minified) | 4,082 | 2,590 | 1,521 | 1.58× | Fused | Win |
| Instruments (prettified) | 3,800 | 4,308 | 2,732 | 0.88× | Two-stage | Win |
| Marine IK Reverse (minified) | 844 | 798 | 685 | 1.06× | Fused | Win |
| Marine IK Reverse (prettified) | 3,609 | 3,604 | 3,078 | 1.00× | Tie | Win |
| Marine IK (minified) | 860 | 813 | 679 | 1.06× | Fused | Win |
| Marine IK (prettified) | 3,604 | 3,696 | 3,142 | 0.98× | Two-stage | Win |
| Mesh (minified) | 1,142 | 1,227 | 1,319 | 0.93× | Two-stage | Loss |
| Mesh (prettified) | 1,915 | 2,160 | 2,440 | 0.89× | Two-stage | Loss |
| Random (minified) | 2,067 | 1,965 | 1,054 | 1.05× | Fused | Win |
| Random (prettified) | 2,707 | 3,252 | 1,905 | 0.83× | Two-stage | Win |
| Twitter (minified) | 2,652 | 2,163 | 1,586 | 1.23× | Fused | Win |
| Twitter (prettified) | 2,334 | 3,165 | 2,277 | 0.74× | Two-stage | Win |
| Canada Small (minified) | 1,338 | 1,190 | 895 | 1.12× | Fused | Win |
| Canada Small (prettified) | 3,107 | 3,339 | 2,590 | 0.93× | Two-stage | Win |
| CitmCatalog Small (minified) | 2,353 | 1,912 | 1,386 | 1.23× | Fused | Win |
| CitmCatalog Small (prettified) | 3,124 | 3,809 | 2,903 | 0.82× | Two-stage | Win |
| Discord Small (minified) | 2,557 | 2,040 | 1,598 | 1.25× | Fused | Win |
| Discord Small (prettified) | 2,509 | 2,896 | 2,289 | 0.87× | Two-stage | Win |
| Google Maps Response Small (minified) | 2,822 | 2,020 | 1,256 | 1.40× | Fused | Win |
| Google Maps Response Small (prettified) | 4,102 | 4,343 | 2,975 | 0.94× | Two-stage | Win |
| Instruments Small (minified) | 3,776 | 2,382 | 1,359 | 1.59× | Fused | Win |
| Instruments Small (prettified) | 3,507 | 3,998 | 2,483 | 0.88× | Two-stage | Win |
| Marine IK Reverse Small (minified) | 1,006 | 848 | 640 | 1.19× | Fused | Win |
| Marine IK Reverse Small (prettified) | 3,443 | 3,902 | 3,007 | 0.88× | Two-stage | Win |
| Marine IK Small (minified) | 1,128 | 909 | 640 | 1.24× | Fused | Win |
| Marine IK Small (prettified) | 3,735 | 4,081 | 2,909 | 0.92× | Two-stage | Win |
| Mesh Small (minified) | 1,192 | 1,015 | 1,001 | 1.17× | Fused | Win |
| Mesh Small (prettified) | 1,738 | 2,024 | 2,023 | 0.86× | Two-stage | Tie |
| Random Small (minified) | 2,697 | 2,109 | 1,043 | 1.28× | Fused | Win |
| Random Small (prettified) | 3,166 | 3,556 | 1,923 | 0.89× | Two-stage | Win |
| Twitter Small (minified) | 3,363 | 2,488 | 1,720 | 1.35× | Fused | Win |
| Twitter Small (prettified) | 2,602 | 3,445 | 2,453 | 0.76× | Two-stage | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,611 | 235 | 197 | 6.85× | Fused | Win |
| Double (POD) | 1,131 | 356 | 159 | 3.18× | Fused | Win |
| Int64 (POD) | 2,740 | 656 | 483 | 4.18× | Fused | Win |
| String (POD) | 1,985 | 933 | 931 | 2.13× | Fused | Win |
| Uint64 (POD) | 2,491 (n/c) | 594 | 437 | 4.20× | Fused | Win |
| Canada (minified) | 808 | 681 | 464 | 1.19× | Fused | Win |
| Canada (prettified) | 2,011 | 1,853 | 1,361 | 1.09× | Fused | Win |
| CitmCatalog (minified) | 2,367 | 1,405 | 1,118 | 1.69× | Fused | Win |
| CitmCatalog (prettified) | 3,344 | 3,003 | 2,345 | 1.11× | Fused | Win |
| Discord (minified) | 2,161 | 1,361 | 1,133 | 1.59× | Fused | Win |
| Discord (prettified) | 2,468 | 2,006 | 1,717 | 1.23× | Fused | Win |
| Google Maps Response (minified) | 2,050 | 1,165 | 622 (n/c) | 1.76× | Fused | Win |
| Google Maps Response (prettified) | 2,637 | 2,592 | 1,651 | 1.02× | Fused | Win |
| Instruments (minified) | 2,777 | 1,517 | 1,151 | 1.83× | Fused | Win |
| Instruments (prettified) | 3,170 | 2,514 | 1,959 | 1.26× | Fused | Win |
| Marine IK Reverse (minified) | 582 (n/c) | 524 (n/c) | 419 | 1.11× | Fused | Win |
| Marine IK Reverse (prettified) | 2,375 | 2,130 | 1,828 (n/c) | 1.12× | Fused | Win |
| Marine IK (minified) | 479 | 568 | 426 | 0.84× | Two-stage | Win |
| Marine IK (prettified) | 2,164 | 1,995 | 1,987 (n/c) | 1.08× | Tie | Win |
| Mesh (minified) | 972 | 821 | 823 | 1.18× | Fused | Win |
| Mesh (prettified) | 819 | 1,323 | 1,498 | 0.62× | Two-stage | Loss |
| Random (minified) | 1,574 | 1,124 | 692 | 1.40× | Fused | Win |
| Random (prettified) | 2,154 | 1,691 | 1,149 (n/c) | 1.27× | Fused | Win |
| Twitter (minified) | 2,200 | 1,757 | 1,187 | 1.25× | Fused | Win |
| Twitter (prettified) | 2,413 | 2,352 | 1,677 | 1.03× | Fused | Win |
| Canada Small (minified) | 886 | 736 | 425 | 1.20× | Fused | Win |
| Canada Small (prettified) | 1,893 | 2,054 | 1,250 | 0.92× | Two-stage | Win |
| CitmCatalog Small (minified) | 1,604 | 1,208 | 949 | 1.33× | Fused | Win |
| CitmCatalog Small (prettified) | 2,490 | 2,302 | 1,880 | 1.08× | Fused | Win |
| Discord Small (minified) | 2,124 | 1,351 | 915 | 1.57× | Fused | Win |
| Discord Small (prettified) | 1,886 | 1,876 | 1,117 | 1.01× | Tie | Win |
| Google Maps Response Small (minified) | 1,950 | 1,185 | 748 | 1.65× | Fused | Win |
| Google Maps Response Small (prettified) | 2,948 | 2,416 | 1,443 | 1.22× | Fused | Win |
| Instruments Small (minified) | 2,728 | 1,351 | 985 | 2.02× | Fused | Win |
| Instruments Small (prettified) | 2,772 | 2,411 | 1,924 | 1.15× | Fused | Win |
| Marine IK Reverse Small (minified) | 636 | 528 | 290 | 1.20× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,389 | 2,079 | 1,837 | 1.15× | Fused | Win |
| Marine IK Small (minified) | 789 | 607 | 371 | 1.30× | Fused | Win |
| Marine IK Small (prettified) | 2,757 | 2,624 | 1,793 | 1.05× | Fused | Win |
| Mesh Small (minified) | 896 | 762 | 634 | 1.18× | Fused | Win |
| Mesh Small (prettified) | 1,390 | 1,379 | 1,194 | 1.01× | Tie | Win |
| Random Small (minified) | 1,770 | 1,324 | 754 | 1.34× | Fused | Win |
| Random Small (prettified) | 2,259 | 1,759 | 1,336 | 1.28× | Fused | Win |
| Twitter Small (minified) | 2,547 | 1,819 | 1,200 | 1.40× | Fused | Win |
| Twitter Small (prettified) | 2,445 | 2,384 | 1,519 | 1.03× | Fused | Win |

**macOS / Clang 23.1 (Apple M1, NEON)**

| Test | Fused (MB/s) | Two-stage (MB/s) | simdjson (MB/s) | Fused ÷ two-stage | Faster Jsonifier path | Best Jsonifier path vs simdjson |
|---|---|---|---|---|---|---|
| Bool (POD) | 1,161 | 214 | 177 | 5.42× | Fused | Win |
| Double (POD) | 917 | 298 | 161 | 3.08× | Fused | Win |
| Int64 (POD) | 2,447 | 591 (n/c) | 417 | 4.14× | Fused | Win |
| String (POD) | 1,798 | 821 | 1,021 | 2.19× | Fused | Win |
| Uint64 (POD) | 2,313 | 604 | 411 | 3.83× | Fused | Win |
| Canada (minified) | 782 | 696 | 449 | 1.12× | Fused | Win |
| Canada (prettified) | 1,967 | 1,771 | 908 (n/c) | 1.11× | Fused | Win |
| CitmCatalog (minified) | 2,330 | 1,451 | 919 | 1.61× | Fused | Win |
| CitmCatalog (prettified) | 3,323 | 2,777 | 2,038 | 1.20× | Fused | Win |
| Discord (minified) | 2,404 | 1,447 | 1,347 | 1.66× | Fused | Win |
| Discord (prettified) | 2,835 | 1,589 | 1,855 | 1.78× | Fused | Win |
| Google Maps Response (minified) | 2,357 | 1,110 | 915 | 2.12× | Fused | Win |
| Google Maps Response (prettified) | 3,296 | 2,634 | 2,386 | 1.25× | Fused | Win |
| Instruments (minified) | 2,557 | 1,508 | 1,312 | 1.70× | Fused | Win |
| Instruments (prettified) | 2,621 | 2,551 | 1,918 | 1.03× | Fused | Win |
| Marine IK Reverse (minified) | 670 | 559 | 368 | 1.20× | Fused | Win |
| Marine IK Reverse (prettified) | 2,394 | 2,298 | 1,738 | 1.04× | Tie | Win |
| Marine IK (minified) | 696 | 555 | 392 | 1.25× | Fused | Win |
| Marine IK (prettified) | 2,372 (n/c) | 2,066 | 1,701 | 1.15× | Tie | Win |
| Mesh (minified) | 1,043 | 805 | 778 | 1.30× | Fused | Win |
| Mesh (prettified) | 1,492 | 1,303 | 1,433 | 1.15× | Fused | Win |
| Random (minified) | 2,216 | 1,163 | 919 | 1.91× | Fused | Win |
| Random (prettified) | 2,674 | 1,748 | 1,597 | 1.53× | Fused | Win |
| Twitter (minified) | 2,625 | 1,549 | 1,728 | 1.70× | Fused | Win |
| Twitter (prettified) | 2,717 | 2,259 | 2,270 | 1.20× | Fused | Win |
| Canada Small (minified) | 777 | 699 | 418 | 1.11× | Fused | Win |
| Canada Small (prettified) | 2,004 | 1,886 | 1,013 | 1.06× | Fused | Win |
| CitmCatalog Small (minified) | 1,671 | 1,215 | 961 | 1.38× | Fused | Win |
| CitmCatalog Small (prettified) | 2,613 | 2,079 | 1,680 | 1.26× | Fused | Win |
| Discord Small (minified) | 2,462 | 1,488 | 1,546 | 1.65× | Fused | Win |
| Discord Small (prettified) | 2,521 | 2,032 | 2,184 | 1.24× | Fused | Win |
| Google Maps Response Small (minified) | 2,221 | 1,234 | 919 | 1.80× | Fused | Win |
| Google Maps Response Small (prettified) | 3,185 | 2,439 | 2,263 | 1.31× | Fused | Win |
| Instruments Small (minified) | 2,793 | 1,622 | 1,278 | 1.72× | Fused | Win |
| Instruments Small (prettified) | 3,153 | 2,746 | 2,317 | 1.15× | Fused | Win |
| Marine IK Reverse Small (minified) | 725 | 572 | 365 | 1.27× | Fused | Win |
| Marine IK Reverse Small (prettified) | 2,518 | 2,408 | 1,691 | 1.05× | Fused | Win |
| Marine IK Small (minified) | 780 | 664 | 362 | 1.17× | Fused | Win |
| Marine IK Small (prettified) | 2,604 | 2,648 | 1,559 | 0.98× | Two-stage | Win |
| Mesh Small (minified) | 905 | 719 | 647 | 1.26× | Fused | Win |
| Mesh Small (prettified) | 1,375 | 1,401 | 1,303 | 0.98× | Two-stage | Win |
| Random Small (minified) | 2,299 | 1,239 | 1,069 | 1.86× | Fused | Win |
| Random Small (prettified) | 2,758 | 1,544 | 1,720 | 1.79× | Fused | Win |
| Twitter Small (minified) | 2,224 | 1,900 | 1,695 | 1.17× | Fused | Win |
| Twitter Small (prettified) | 2,533 | 2,312 | 1,490 (n/c) | 1.10× | Fused | Win |

### 2.4 What the results show

Which Jsonifier path is faster depends on two things: whether the document is indented, and whether the machine is x86. On the three x86 builds the fused path wins all POD input, 28 of the 30 full-size minified documents and all 30 Small minified ones, while the tape wins 27 of the 30 full-size prettified documents and all 30 Small prettified ones. MSVC splits cleanly: minified to the fused path, prettified to the tape, at both sizes. On the M1 the fused path wins all POD input, 39 of the 40 minified documents (one tie) and 30 of the 40 prettified documents (seven to the tape, three ties).

**POD-type tests: the fused path wins all 25 freshly allocated and all 25 reused, on every build.** It runs them 1.36× (String, Windows/MSVC) to 7.14× (Double, Windows/MSVC) faster than the two-stage path, and 1.59× to 8.26× faster with objects reused, at the same two extremes. These documents are the case §2 argues from: nothing to skip, every value materialized, so the tape is pure overhead. This is the one result that does not depend on the platform.

**Minified documents: the fused path's, at both sizes.** The fused path wins 97 of the 100 minified documents: all 20 under MSVC, all 20 on Linux/Clang, 18 of 20 on Linux/GCC, 19 of 20 on macOS/GCC and all 20 on macOS/Clang. On Linux/GCC the two-stage path wins minified Canada by 1% (932 against 920 MB/s) and minified Mesh by 8% (1,169 against 1,087), and minified Instruments on macOS/GCC is a tie. The fused path's margins run from 1% (Random under MSVC) to 108% (Google Maps Response on macOS/Clang, 2,108 against 1,012 MB/s) on the full-size documents and up to 1.97× on the Small ones (Random Small on macOS/Clang, 1,952 against 993 MB/s). This is the workload the fused key literals of §3 were built for: machine-generated JSON in declared order.

**Prettified documents on x86: the two-stage path wins 57 of 60.** It wins all 20 under MSVC, all 10 Small prettified documents on both Linux builds, and 8 of the 10 full-size prettified documents on Linux/Clang and 9 of 10 on Linux/GCC. The fused path wins prettified Marine IK Reverse on both Linux builds, and prettified Marine IK on Linux/Clang is a tie (3,537 against 3,543 MB/s). The tape's margins run from 1% (Marine IK on Linux/GCC) to 34% (Discord on Linux/Clang, 2,844 against 2,115 MB/s) on the full-size documents and from 5% to 33% on the Small ones, widest on Twitter Small on Linux/Clang. The two Marine IK documents, which are mostly numbers, are the closest on every x86 build, and Discord and Twitter are among the widest. The fused path already predicts the indentation of every line, including the lines that close an object or an array, matches the `": "` after each key as one two-byte constant, and verifies each predicted span with a 16-byte vector loop, a single 8-byte SWAR step and a scalar remainder switch (§3). That keeps the number-heavy documents close, but it does not win most prettified documents on x86. Stage 1 classifies every byte, whitespace included, in vector blocks at a cost that does not depend on layout, while the fused path still does a small amount of branchy work per line: the newline test, the depth multiplication, the span check and the branch on its result. On a heavily indented document that per-line cost appears to outweigh the second pass over the input that the tape costs. We have not isolated this, and state it as a hypothesis. With objects reused the tape wins 56 of the 60 prettified x86 documents, the fused path wins prettified Canada and Marine IK Reverse on Linux/Clang, and two are ties (Marine IK on Linux/Clang and Marine IK Reverse on Linux/GCC).

**Prettified documents on the M1: mostly the fused path.** Of the 40 prettified documents, the fused path wins 30, the tape wins 7 and three are ties. On macOS/GCC the fused path wins 8 of the 10 full-size documents, by 4% (Canada, Discord) to 81% (Random, 1,590 against 879 MB/s), and the tape wins Marine IK by 13% and Mesh by 27% (1,230 against 969 MB/s); among the Small documents it wins 6, the tape wins Canada, Marine IK and Mesh, and Twitter is a tie. On macOS/Clang the fused path wins 8 of the 10 full-size documents, by 3% (CitmCatalog) to 82% (Random, 2,255 against 1,240 MB/s), the tape wins Mesh by 7% (1,438 against 1,340 MB/s) and Canada is a tie; among the Small documents it wins 8, the tape wins Mesh and Twitter is a tie. Reused, the fused path wins 31 of the 40, the tape wins 4 and 5 are ties. NEON escapes most of the prettified penalty seen on x86, and we do not have an explanation for why. One contributing factor is that Jsonifier's stage 1 is comparatively more expensive there, since NEON has no `movemask` and the collectors emulate it with narrowing shifts, but we have not isolated it.

**Mesh is where Jsonifier is weakest.** The two-stage path wins prettified Mesh on all three x86 builds and on both M1 builds, and minified Mesh on Linux/GCC. Mesh is also the document on which both Jsonifier paths lose to simdjson On Demand most often. Freshly allocated, that happens on Linux/GCC in both forms, by 2% minified (1,190 against 1,169 MB/s) and 4% prettified (2,190 against 2,102), and on macOS/GCC prettified by 5% (1,295 against 1,230). With objects reused it loses the same three, by 7% and 13% on Linux/GCC and 13% on macOS/GCC, and prettified Mesh Small on Linux/GCC is a tie. The fused path alone also loses prettified Mesh and Mesh Small to simdjson on Linux/Clang in both runs. We have not profiled what is particular about this document.

**MSVC slows simdjson more than either Jsonifier path.** All libraries run slower under MSVC than under Linux/Clang on the same i9-14900KF, but not by the same amount. Across the 9 full-size minified documents in declared key order, the fused path reaches a median 58% of its Linux/Clang throughput, the two-stage path 61% and simdjson 42%. Because the two Jsonifier paths lose about the same share, MSVC routes as Linux/Clang does: minified to the fused path, prettified to the tape. The spread between documents is wider for the fused path (44% on Google Maps to 73% on Instruments) than for the two-stage path (57% to 70%). That fits the fused path being branch-heavy code whose performance rests on the optimizer while stage 1 is branch-free intrinsic code whose shape is fixed by the source, but we have not isolated it.

**Against simdjson, Jsonifier wins 221 of 225 tests with whichever path is faster and loses four.** The losses are Mesh in both forms on Linux/GCC, prettified Mesh on macOS/GCC, and prettified Twitter Small on Linux/Clang (3,148 against 3,161 MB/s). The fused path alone wins 213, ties 1 and loses 11: those four, plus prettified Discord, Instruments, Mesh and Twitter on Linux/Clang and the Small copies of Discord, Instruments and Mesh there. Under MSVC both paths win all 45. Several of these verdicts rest on non-converged results, marked n/c in §2.3.

**Reverse key order.** Requesting every key in reverse order is meant to force simdjson's On Demand API into the rescanning behavior described in §3, and under MSVC it does: simdjson parses minified Marine IK Reverse at 36 MB/s, against 254 MB/s for the same document in declared order, so the faster Jsonifier path is 13.4× faster than simdjson freshly allocated (14.5× reused). The prettified form shows the same gap, at 194 MB/s against 1,263, and the faster Jsonifier path is 11.0× faster (11.8× reused). The Small copies show it too, at 205 MB/s prettified under MSVC. On the other builds the faster Jsonifier path is 1.19× to 1.61× faster than simdjson on the full-size reversed document, freshly allocated, and 1.17× to 1.82× reused. Why the rescan cost shows up mostly under MSVC has not been profiled.

The routing implication is sharper than §2's rule of thumb. On x86 the fused path is right for POD and minified input, apart from Canada and Mesh on Linux/GCC, and prettified input belongs to the two-stage path on all three compilers, apart from Marine IK Reverse on Linux. On the M1 the fused path is right for POD and minified input and for most prettified documents, but not Mesh, and on macOS/GCC not Marine IK or Canada Small either. Routing each test to its faster path would raise the record against simdjson from 213 wins, 1 tie and 11 losses to 221 wins and 4 losses. The router does not yet take the architecture or the indentation into account.

### 2.5 Stage 1 + reflection: two-stage Jsonifier against simdjson's reflection reader

simdjson 5 adds a C++26 static-reflection reader (`document.get<T>()`): stage 1 builds the structural index, then the reflected type drives materialization. That is the same shape as Jsonifier's two-stage path, a tape plus a compile-time schema, so ranking those two head to head compares the two stage-1 + reflection designs directly. The reader needs P2996 reflection, so it only runs on GCC, and the harness registers it for corpus documents and their Small copies, not the POD-type tests. Non-converged results are kept and marked n/c.

#### Freshly allocated objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 932 | 892 | 1.04× | Win |
| Canada (prettified) | 2,703 | 2,587 | 1.05× | Win |
| CitmCatalog (minified) | 1,904 | 1,431 | 1.33× | Win |
| CitmCatalog (prettified) | 4,349 | 3,531 | 1.23× | Win |
| Discord (minified) | 1,856 | 1,546 | 1.20× | Win |
| Discord (prettified) | 2,861 | 2,502 | 1.14× | Win |
| Google Maps Response (minified) | 1,965 | 1,309 | 1.50× | Win |
| Google Maps Response (prettified) | 4,190 | 3,063 | 1.37× | Win |
| Instruments (minified) | 2,301 | 1,876 | 1.23× | Win |
| Instruments (prettified) | 3,883 | 3,379 | 1.15× | Win |
| Marine IK Reverse (minified) | 728 | 740 | 0.98× | **Loss** |
| Marine IK Reverse (prettified) | 3,334 | 3,388 | 0.98× | **Loss** |
| Marine IK (minified) | 734 | 727 | 1.01× | Win |
| Marine IK (prettified) | 3,332 | 3,334 | 1.00× | Tie |
| Mesh (minified) | 1,169 | 1,161 | 1.01× | Win |
| Mesh (prettified) | 2,102 | 2,156 | 0.97× | **Loss** |
| Random (minified) | 1,402 | 1,191 | 1.18× | Win |
| Random (prettified) | 2,393 | 2,107 | 1.14× | Win |
| Twitter (minified) | 1,740 | 1,812 | 0.96× | **Loss** |
| Twitter (prettified) | 2,580 | 2,591 | 1.00× | **Loss** |
| Canada Small (minified) | 963 | 956 | 1.01× | Win |
| Canada Small (prettified) | 2,760 | 2,735 | 1.01× | Win |
| CitmCatalog Small (minified) | 1,598 | 1,279 | 1.25× | Win |
| CitmCatalog Small (prettified) | 3,276 | 2,732 | 1.20× | Win |
| Discord Small (minified) | 1,629 | 1,350 | 1.21× | Win |
| Discord Small (prettified) | 2,358 | 1,993 | 1.18× | Win |
| Google Maps Response Small (minified) | 1,810 | 1,285 | 1.41× | Win |
| Google Maps Response Small (prettified) | 4,023 | 3,040 | 1.32× | Win |
| Instruments Small (minified) | 2,174 | 1,747 | 1.24× | Win |
| Instruments Small (prettified) | 3,759 | 3,192 | 1.18× | Win |
| Marine IK Reverse Small (minified) | 701 | 754 | 0.93× | **Loss** |
| Marine IK Reverse Small (prettified) | 3,337 | 3,444 | 0.97× | **Loss** |
| Marine IK Small (minified) | 750 | 741 | 1.01× | Win |
| Marine IK Small (prettified) | 3,479 | 3,400 | 1.02× | Win |
| Mesh Small (minified) | 875 | 888 | 0.99× | **Loss** |
| Mesh Small (prettified) | 1,695 | 1,778 | 0.95× | **Loss** |
| Random Small (minified) | 1,776 | 1,273 | 1.40× | Win |
| Random Small (prettified) | 3,110 | 2,339 | 1.33× | Win |
| Twitter Small (minified) | 2,186 | 2,101 | 1.04× | Win |
| Twitter Small (prettified) | 3,092 | 2,914 | 1.06× | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 568 | 525 | 1.08× | Win |
| Canada (prettified) | 1,561 | 1,438 | 1.08× | Win |
| CitmCatalog (minified) | 1,148 | 918 | 1.25× | Win |
| CitmCatalog (prettified) | 2,801 | 2,350 | 1.19× | Win |
| Discord (minified) | 1,039 | 1,003 | 1.04× | Win |
| Discord (prettified) | 1,761 | 1,549 | 1.14× | Win |
| Google Maps Response (minified) | 1,043 | 755 | 1.38× | Win |
| Google Maps Response (prettified) | 1,833 | 1,491 | 1.23× | Win |
| Instruments (minified) | 1,395 | 1,004 | 1.39× | Win |
| Instruments (prettified) | 2,296 | 1,710 | 1.34× | Win |
| Marine IK Reverse (minified) | 403 | 430 | 0.94× | **Loss** |
| Marine IK Reverse (prettified) | 1,794 | 1,755 | 1.02× | Tie |
| Marine IK (minified) | 420 | 405 | 1.04× | Tie |
| Marine IK (prettified) | 2,146 | 1,709 | 1.26× | Win |
| Mesh (minified) | 751 | 560 | 1.34× | Win |
| Mesh (prettified) | 1,230 | 970 | 1.27× | Win |
| Random (minified) | 738 | 550 | 1.34× | Win |
| Random (prettified) | 879 | 1,142 | 0.77× | **Loss** |
| Twitter (minified) | 1,400 | 1,060 | 1.32× | Win |
| Twitter (prettified) | 1,807 | 1,484 | 1.22× | Win |
| Canada Small (minified) | 564 | 431 | 1.31× | Win |
| Canada Small (prettified) | 1,573 | 1,168 | 1.35× | Win |
| CitmCatalog Small (minified) | 956 | 649 | 1.47× | Win |
| CitmCatalog Small (prettified) | 1,833 | 1,509 | 1.21× | Win |
| Discord Small (minified) | 1,129 | 941 | 1.20× | Win |
| Discord Small (prettified) | 994 | 965 | 1.03× | Tie |
| Google Maps Response Small (minified) | 904 | 762 | 1.19× | Win |
| Google Maps Response Small (prettified) | 1,971 | 1,627 | 1.21× | Win |
| Instruments Small (minified) | 1,213 | 1,019 (n/c) | 1.19× | Win |
| Instruments Small (prettified) | 2,376 | 1,779 | 1.34× | Win |
| Marine IK Reverse Small (minified) | 470 | 356 | 1.32× | Win |
| Marine IK Reverse Small (prettified) | 2,017 | 1,671 | 1.21× | Win |
| Marine IK Small (minified) | 441 | 425 | 1.04× | Win |
| Marine IK Small (prettified) | 2,181 | 2,051 | 1.06× | Win |
| Mesh Small (minified) | 603 | 473 | 1.27× | Win |
| Mesh Small (prettified) | 1,252 | 930 | 1.35× | Win |
| Random Small (minified) | 908 | 633 | 1.43× | Win |
| Random Small (prettified) | 1,337 | 1,166 (n/c) | 1.15× | Win |
| Twitter Small (minified) | 1,474 | 1,092 | 1.35× | Win |
| Twitter Small (prettified) | 2,124 | 1,456 | 1.46× | Win |

#### Reused objects

**Linux / GCC 16.2 (i9-14900KF, AVX2)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 1,117 | 1,074 | 1.04× | Win |
| Canada (prettified) | 3,175 | 3,068 | 1.03× | Win |
| CitmCatalog (minified) | 2,285 | 1,687 | 1.35× | Win |
| CitmCatalog (prettified) | 5,048 | 4,096 | 1.23× | Win |
| Discord (minified) | 2,513 | 1,817 | 1.38× | Win |
| Discord (prettified) | 3,751 | 2,834 | 1.32× | Win |
| Google Maps Response (minified) | 2,143 | 1,381 | 1.55× | Win |
| Google Maps Response (prettified) | 4,557 | 3,219 | 1.42× | Win |
| Instruments (minified) | 2,590 | 1,963 | 1.32× | Win |
| Instruments (prettified) | 4,308 | 3,515 | 1.23× | Win |
| Marine IK Reverse (minified) | 798 | 834 | 0.96× | **Loss** |
| Marine IK Reverse (prettified) | 3,604 | 3,722 | 0.97× | **Loss** |
| Marine IK (minified) | 813 | 821 | 0.99× | **Loss** |
| Marine IK (prettified) | 3,696 | 3,693 | 1.00× | Tie |
| Mesh (minified) | 1,227 | 1,234 | 0.99× | **Loss** |
| Mesh (prettified) | 2,160 | 2,292 | 0.94× | **Loss** |
| Random (minified) | 1,965 | 1,463 | 1.34× | Win |
| Random (prettified) | 3,252 | 2,588 | 1.26× | Win |
| Twitter (minified) | 2,163 | 2,106 | 1.03× | Win |
| Twitter (prettified) | 3,165 | 2,969 | 1.07× | Win |
| Canada Small (minified) | 1,190 | 1,111 | 1.07× | Win |
| Canada Small (prettified) | 3,339 | 3,167 | 1.05× | Win |
| CitmCatalog Small (minified) | 1,912 | 1,426 | 1.34× | Win |
| CitmCatalog Small (prettified) | 3,809 | 2,943 | 1.29× | Win |
| Discord Small (minified) | 2,040 | 1,586 | 1.29× | Win |
| Discord Small (prettified) | 2,896 | 2,313 | 1.25× | Win |
| Google Maps Response Small (minified) | 2,020 | 1,359 | 1.49× | Win |
| Google Maps Response Small (prettified) | 4,343 | 3,199 | 1.36× | Win |
| Instruments Small (minified) | 2,382 | 1,790 | 1.33× | Win |
| Instruments Small (prettified) | 3,998 | 3,277 | 1.22× | Win |
| Marine IK Reverse Small (minified) | 848 | 871 | 0.97× | **Loss** |
| Marine IK Reverse Small (prettified) | 3,902 | 3,849 | 1.01× | Win |
| Marine IK Small (minified) | 909 | 856 | 1.06× | Win |
| Marine IK Small (prettified) | 4,081 | 3,887 | 1.05× | Win |
| Mesh Small (minified) | 1,015 | 1,034 | 0.98× | **Loss** |
| Mesh Small (prettified) | 2,024 | 2,109 | 0.96× | **Loss** |
| Random Small (minified) | 2,109 | 1,414 | 1.49× | Win |
| Random Small (prettified) | 3,556 | 2,561 | 1.39× | Win |
| Twitter Small (minified) | 2,488 | 2,256 | 1.10× | Win |
| Twitter Small (prettified) | 3,445 | 3,177 | 1.08× | Win |

**macOS / GCC 16.2 (Apple M1, NEON)**

| Test | Two-stage Jsonifier (MB/s) | simdjson reflection (MB/s) | Two-stage ÷ reflection | Verdict |
|---|---|---|---|---|
| Canada (minified) | 681 | 570 | 1.20× | Win |
| Canada (prettified) | 1,853 | 1,681 | 1.10× | Win |
| CitmCatalog (minified) | 1,405 | 1,066 | 1.32× | Win |
| CitmCatalog (prettified) | 3,003 | 2,272 | 1.32× | Win |
| Discord (minified) | 1,361 | 1,142 | 1.19× | Win |
| Discord (prettified) | 2,006 | 1,716 | 1.17× | Win |
| Google Maps Response (minified) | 1,165 | 862 | 1.35× | Win |
| Google Maps Response (prettified) | 2,592 | 2,060 | 1.26× | Win |
| Instruments (minified) | 1,517 | 1,153 | 1.32× | Win |
| Instruments (prettified) | 2,514 | 1,887 | 1.33× | Win |
| Marine IK Reverse (minified) | 524 (n/c) | 456 | 1.15× | Win |
| Marine IK Reverse (prettified) | 2,130 | 1,946 | 1.09× | Win |
| Marine IK (minified) | 568 | 357 | 1.59× | Win |
| Marine IK (prettified) | 1,995 | 1,424 | 1.40× | Win |
| Mesh (minified) | 821 | 603 | 1.36× | Win |
| Mesh (prettified) | 1,323 | 1,079 | 1.23× | Win |
| Random (minified) | 1,124 | 680 | 1.65× | Win |
| Random (prettified) | 1,691 | 1,109 | 1.52× | Win |
| Twitter (minified) | 1,757 | 1,239 | 1.42× | Win |
| Twitter (prettified) | 2,352 | 1,603 | 1.47× | Win |
| Canada Small (minified) | 736 | 503 | 1.46× | Win |
| Canada Small (prettified) | 2,054 | 1,596 | 1.29× | Win |
| CitmCatalog Small (minified) | 1,208 | 885 | 1.37× | Win |
| CitmCatalog Small (prettified) | 2,302 | 1,922 | 1.20× | Win |
| Discord Small (minified) | 1,351 | 1,030 | 1.31× | Win |
| Discord Small (prettified) | 1,876 | 1,599 (n/c) | 1.17× | Win |
| Google Maps Response Small (minified) | 1,185 | 890 | 1.33× | Win |
| Google Maps Response Small (prettified) | 2,416 | 2,170 | 1.11× | Win |
| Instruments Small (minified) | 1,351 | 1,006 | 1.34× | Win |
| Instruments Small (prettified) | 2,411 | 2,069 | 1.16× | Win |
| Marine IK Reverse Small (minified) | 528 | 414 | 1.28× | Win |
| Marine IK Reverse Small (prettified) | 2,079 | 1,980 | 1.05× | Tie |
| Marine IK Small (minified) | 607 | 530 | 1.15× | Win |
| Marine IK Small (prettified) | 2,624 | 2,384 | 1.10× | Win |
| Mesh Small (minified) | 762 | 582 | 1.31× | Win |
| Mesh Small (prettified) | 1,379 | 1,250 | 1.10× | Win |
| Random Small (minified) | 1,324 | 807 | 1.64× | Win |
| Random Small (prettified) | 1,759 | 1,586 | 1.11× | Win |
| Twitter Small (minified) | 1,819 | 1,389 | 1.31× | Win |
| Twitter Small (prettified) | 2,384 | 1,716 | 1.39× | Win |

In the freshly allocated run, two-stage Jsonifier wins 65 of these 80 tests, ties 4 and loses 11. On the full-size documents it wins 30 of 40: on Linux/GCC it is slower on both Marine IK Reverse documents, prettified Mesh and both forms of Twitter (by 4% at most) and ties prettified Marine IK; on macOS/GCC it loses minified Marine IK Reverse by 6% and prettified Random by 23%, and ties prettified Marine IK Reverse and minified Marine IK. On the Small documents it wins 35 of 40, losing both Marine IK Reverse Small documents and both Mesh Small documents on Linux/GCC (by 1% to 7%) and tying prettified Discord Small on macOS/GCC. Mesh is also where Jsonifier is weakest against simdjson On Demand (§2.4); we have not profiled either result. With the fused path instead of the two-stage one, Jsonifier beats the reflection reader on 63 of the 80, with 5 ties. With objects reused, two-stage Jsonifier wins 70 of 80, ties 2 and loses 8, every loss on Linux/GCC: Marine IK Reverse and Mesh in both forms, minified Marine IK, minified Marine IK Reverse Small and both Mesh Small documents, all by 6% or less, and prettified Marine IK there is a tie. On macOS/GCC it wins 39 of 40 reused, and prettified Marine IK Reverse Small is a tie.

### 2.6 Partial reading: where the tape earns its keep

The harness has one test built for the workload the tape exists for. "Twitter Partial" parses the Twitter document, and its Small copy, into a type that holds three fields per status (`text`, `user.screen_name` and `retweet_count`) and skips everything else. The harness reads the keys without assuming their order, and simdjson On Demand reads the same three fields. The harness requires stage 1 and stage 2 for this test, so both Jsonifier rows run the two-stage path; the table uses the "jsonifier (two-stage)" row. The two rows agree within 8% on 37 of the 40 results; the widest gap is reused prettified Twitter Small on macOS/GCC, where the "jsonifier" row runs at 1,364 MB/s against the two-stage row's 4,039. Results that did not converge are marked n/c.

#### Freshly allocated objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (minified) | 5,588 | 1,918 | 2.91× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (prettified) | 7,168 | 2,655 | 2.70× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (minified) | 4,614 | 2,054 | 2.25× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,138 | 2,723 | 2.25× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,121 | 5,645 | 1.26× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,026 | 7,410 | 1.22× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,313 | 5,265 | 1.01× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,900 | 6,822 | 1.01× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,364 | 5,494 | 1.34× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 8,832 | 7,198 | 1.23× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,862 | 4,701 | 1.25× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,484 | 6,274 | 1.19× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter (minified) | 3,738 | 2,696 | 1.39× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter (prettified) | 4,170 | 3,564 | 1.17× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter Small (minified) | 3,197 | 2,652 | 1.21× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter Small (prettified) | 3,467 | 3,472 | 1.00× | Tie |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter (minified) | 3,666 | 3,091 | 1.19× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter (prettified) | 4,197 | 3,956 | 1.06× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter Small (minified) | 3,263 | 2,333 (n/c) | 1.40× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter Small (prettified) | 3,791 | 3,728 | 1.02× | Win |

#### Reused objects

| Platform / Compiler | Document | Jsonifier stage 1 + 2 (MB/s) | simdjson On Demand (MB/s) | Jsonifier ÷ simdjson | Verdict |
|---|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (minified) | 5,970 | 1,977 | 3.02× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter (prettified) | 7,338 | 2,768 | 2.65× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,149 | 2,008 | 2.56× | Win |
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | Twitter Small (prettified) | 6,591 | 2,815 | 2.34× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (minified) | 7,273 | 5,767 | 1.26× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter (prettified) | 9,195 | 7,537 | 1.22× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (minified) | 5,648 | 5,409 | 1.04× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,218 | 7,014 | 1.03× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (minified) | 7,617 | 5,592 | 1.36× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter (prettified) | 9,064 | 7,281 | 1.24× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (minified) | 6,096 | 4,895 | 1.25× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | Twitter Small (prettified) | 7,776 | 6,428 | 1.21× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter (minified) | 3,772 | 2,902 | 1.30× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter (prettified) | 4,506 | 3,265 | 1.38× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter Small (minified) | 2,869 | 2,363 | 1.21× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | Twitter Small (prettified) | 4,039 | 3,548 | 1.14× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter (minified) | 3,673 | 2,695 | 1.36× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter (prettified) | 4,503 | 4,217 | 1.07× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter Small (minified) | 3,133 | 3,087 | 1.02× | Tie |
| macOS / Clang 23.1 (Apple M1, NEON) | Twitter Small (prettified) | 4,154 | 3,872 | 1.07× | Win |

Jsonifier wins 38 of the 40 results and ties 2, by up to 3.02× (minified Twitter under MSVC, reused). The ties are freshly allocated prettified Twitter Small on macOS/GCC and reused minified Twitter Small on macOS/Clang. Throughput here is counted over the whole document, so these figures run well above the full-parse ones: on minified Twitter the fused path parses at 1,241 MB/s under MSVC and 2,038 MB/s on Linux/Clang when it materializes every value, against 5,588 and 7,121 MB/s here, because the bytes the caller does not ask for are skipped through the tape instead of being walked. The margin over simdjson is 2.25× to 3.02× under MSVC and 1.00× to 1.40× elsewhere, and it is narrower on the Small document than on the full-size one on every build but macOS/Clang. On minified Twitter in the freshly allocated run, simdjson On Demand under MSVC reaches 34% of its Linux/Clang throughput and Jsonifier's stage 1 + 2 path reaches 78%. This is the one test in the sweep where the index is unambiguously the right tool, and it is the reason the two-stage machinery stays in Jsonifier even though full-document parsing does not route through it.

### 2.7 Validation

The sweep's "Validate" test checks a 617 KiB document for well-formedness without materializing anything, against Glaze's validator; simdjson is not registered for it, and it runs freshly allocated only. Jsonifier wins on all five builds:

| Platform / Compiler | Jsonifier (MB/s) | Glaze (MB/s) | Jsonifier ÷ Glaze | Verdict |
|---|---|---|---|---|
| Windows / MSVC 19.44 (i9-14900KF, AVX2) | 2,929 | 2,334 | 1.25× | Win |
| Linux / Clang 24.0 (i9-14900KF, AVX2) | 3,584 | 2,675 | 1.34× | Win |
| Linux / GCC 16.2 (i9-14900KF, AVX2) | 3,019 | 2,452 | 1.23× | Win |
| macOS / GCC 16.2 (Apple M1, NEON) | 1,595 | 1,322 | 1.21× | Win |
| macOS / Clang 23.1 (Apple M1, NEON) | 1,957 | 1,491 | 1.31× | Win |

## 3. The single-pass path: `json_cursor` over raw text

The fused path is implemented as `json_cursor<parseOpts, read_buffer_ptr>` — a set of static navigation primitives over a raw `read_buffer_ptr` cursor, with the end pointer, the detected indentation and the string buffer held in a `parse_context` and the nesting depth passed down the call chain, walking the document exactly once. Several design decisions distinguish it:

**Compile-time key fusion.** For each reflected member, the expected token is baked into the binary at compile time as a fused literal. In minified known-order mode, an entire member header — leading comma, quoted key, and colon — collapses into a single constant:

```cpp
template<uint64_t index, typename literal_type> JSONIFIER_INLINE static constexpr auto makeMemberLiteralNew(const literal_type& keyLiteral) noexcept {
	if constexpr (index > 0) {
		return string_literal{ "," } + string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	} else {
		return string_literal{ "\"" } + keyLiteral + string_literal{ "\"" } + string_literal{ ":" };
	}
}
```

When the document arrives in the expected order — the overwhelmingly common case for machine-generated JSON — matching `,"name":` is one constant-length comparison, and the cursor jumps the entire header in a single add. No tokenizer, no tape, no per-character state machine.

**Adaptive out-of-order recovery.** When the known-order guess misses, the parser falls back to a compile-time hash map over the type's keys (`hash_map<value_type>::findIndex`), dispatches through a generated table, and — crucially — *learns*: a thread-local `antiHashStatesNew` array records which index actually matched at each slot, so a document stream with a consistently permuted key order pays the hash cost once and then rides the corrected fast path.

This is worth dwelling on, because key order is where iterative traversal models pay their hidden tax. simdjson's On Demand API resolves a field lookup by scanning forward through the object from the current cursor position; the project's own documentation (doc/basics.md, field-access guidance) instructs callers to request fields in the order they appear in the document for best performance. When a requested key is *not* next, the cursor scans — and skips — every intervening key/value pair to find it, and a subsequent request for a key that lies *behind* the cursor forces a wrap-around rescan of the object. The degenerate case is a caller requesting fields in an order that consistently mismatches the document: each lookup can traverse a large fraction of the object's raw bytes, turning an O(members) parse into an O(members²) crawl over structural positions — per object, per document, forever. The model has no memory; the millionth permuted document costs exactly what the first one did.

Jsonifier's failure mode for the same situation is: one hash lookup, one dispatch-table indirection, and a learned correction. The mismatch cost is paid once per key slot per thread, not once per object instance. A feed of a billion documents with keys in reversed order parses at effectively the same throughput as a feed in declared order, because after the first document the "expected order" *is* the observed order. The schema-directed model converts key order from a per-document runtime tax into a per-stream calibration.

**Depth-predicted indentation.** The non-minified specialization exploits the fact that pretty-printed JSON indents each line by a fixed unit times its nesting depth. At the root, `collectIndentSize` measures that unit once: the indent character (`wsChar`) and how many of it make one level (`indentSize`). After every `{`, `[` and `,`, `skipWhitespacePredicted` steps over the newline and predicts the next line's indentation as `context.indentSize * depth`. It then verifies the whole predicted span in one call to `spanIsIndent`, which works in three tiers on every architecture: a 16-byte vector loop compares the span against the broadcast indent character while more than 16 bytes remain, a single 8-byte SWAR step (one XOR against the broadcast character in a `uint64_t`) handles 9 to 16 remaining bytes, and a size-class switch with overlapping scalar loads resolves the last 8 or fewer, so no span length needs a byte loop. The tiers were chosen by A/B testing across compilers: a wider 32-byte loop and a remainder case that assembled a 128-bit vector from two 8-byte copies both cost MSVC and GCC measurably, while a pure 8-byte SWAR loop halved prettified Canada's throughput under MSVC because its long spans need the vector tier. Before a closing `}` or `]`, `skipWhitespacePredictedClose` makes the same prediction one level shallower, and after each key `collectObjectColon` matches `": "` as a single two-byte constant. If the span matches and the next byte is not whitespace, the cursor jumps the entire indentation at once. If the prediction misses, the parser falls back to `skipWhitespaceScalar`, a `whitespaceTable` lookup loop that advances one byte per iteration.

The result is a parser whose inner loop is dominated by wide constant comparisons and direct value materialization, with SIMD engaged surgically where it wins (string unescaping, discussed in §6) rather than as a mandatory preprocessing pass.

## 4. Stage 1: structural indexing, Jsonifier style

When the workload *does* justify a structural index, Jsonifier's stage 1 (`simd_string_reader`) implements the Langdale–Lemire bitmask algebra with several architectural departures.

### 4.1 Step geometry as a per-compiler compile-time constant

simdjson classifies input per 64-byte block, and its generic indexer steps through the buffer 128 bytes per iteration — two 64-byte blocks, software-pipelined one block deep, a geometry it retains even on its widest kernel (the Icelake implementation invokes `index<128>`). Jsonifier generalizes the unit of work to a *step* of `simdBlocksPerStep` 64-byte blocks, with the constant chosen per ISA **and per compiler**:

```cpp
template<> struct backend_traits<jsonifier_backend::avx2> : compiler_step_tuning<4, 4, 4, 8, 4, 8> {
	static constexpr const char* name{ "AVX2" };
	using simd_int_t = jsonifier_simd_int_256;
};
```

`compiler_step_tuning` takes (tape step, blocks per step) pairs for Clang, GCC and MSVC in that order and resolves to the current compiler's pair, so the AVX2 traits above carry 4 blocks per step on Clang and 8 on GCC and MSVC, with a tape step of 4 on all three. `simdBlocksPerStep` and `simdTapeStep` are read from the configured backend's traits.

These values are not guesses. They were selected by Cartesian parameter sweeps across five platform/compiler CI targets (Windows/MSVC, Linux/GCC, Linux/Clang, macOS/Clang, plus ARM), validated against popcount histograms of structural density on the benchmark corpus. The finding that Clang and GCC want *different* geometry on the identical ISA reflects real differences in how each compiler schedules the unrolled block bodies: on AVX2 Clang takes four blocks (256 bytes) per step where GCC and MSVC take eight (512 bytes), and on NEON both compilers take four blocks per step with a drain burst of 8. It is only expressible because the entire pipeline is specialized at compile time. A runtime-dispatched kernel gets one shape per ISA; a Cathedral-Architecture kernel gets one shape per (ISA × compiler) cell.

Processing up to 8 blocks (512 bytes) per step before draining amortizes the loop-carried state updates and gives the out-of-order core a deep window of independent block computations — a 2-4x wider scan window than simdjson's fixed 128-byte step.

### 4.2 The classification core: `rope_detector` and the collectors

The per-block bitmask algebra lives in small, composable functor structs. `cmp_eq_op` fans a broadcast comparison across the registers of a block and fuses the per-register movemasks into one `uint64_t` with compile-time shift amounts. The escape/quote/in-string state machine is `rope_detector`, a CRTP mixin over a plain `rope_block` of three masks:

```cpp
JSONIFIER_INLINE void next(const simd_array_t in_01, const jsonifier_simd_int_t bsRegister, const jsonifier_simd_int_t quoteRegister) noexcept {
	const uint64_t escaped = nextEscapeAndTerminalCode(simd::cmp_eq_op::impl(in_01, bsRegister));
	const uint64_t quotes  = (simd::cmp_eq_op::impl(in_01, quoteRegister) & ~escaped);
	rope_block::escaped	   = escaped;
	rope_block::quotes	   = quotes;
	return quotes ? finishNext() : finishNextNoInString();
}
```

The escape logic is the classic odd-length-backslash-run computation with the standard fast exit — a block containing zero backslashes skips the arithmetic and just consumes the carried `nextIsEscaped` bit, the same short-circuit simdjson's `json_escape_scanner` ships by default. Jsonifier extends the principle one level up: the prefix-XOR (`clmul` on x86, a shift-XOR ladder on NEON) that turns the quote mask into an in-string range mask is itself conditional — `next` branches on the quote mask, and a quoteless block bypasses the multiply entirely, inheriting `prevInString` directly via `finishNextNoInString` (the NEON variant names the multiplying branch `finishNextInString`; on AVX it is `finishNext`). Since long stretches of numeric or minified structural data contain no quotes at all, entire regions of such documents never touch the carry-less multiplier.

Whitespace and operator classification use the same `shuffle`-against-lookup-table trick simdjson pioneered, expressed as `ws_collector` and `op_collector` over the block's register array. On NEON, where `movemask` doesn't exist, the collectors use the `vshrn_n_u16`-based 4-bit-per-lane narrowing (with `tzcnt >> 2` index correction in `postCmpTzcnt`) and, in the tuned NEON `op_collector`, a `vqtbl1q_u8` nibble-shuffle keyed on `(byte + 3) >> 4` — the reverse-bits/RBIT strategy is available as a compile-time switch where it profiles faster.

### 4.3 Pseudo-structural promotion

Scalar starts are promoted to structurals exactly as in the original algorithm — `followsNonquoteScalar` carries the cross-block bit — so the tape marks the first byte of every number, `true`/`false`/`null`, and string, giving stage 2 direct seek points to every value:

```cpp
JSONIFIER_INLINE uint64_t getStructurals(const simd_array_t in_01, const jsonifier_simd_int_t opTable, const jsonifier_simd_int_t spaceMask,
	const jsonifier_simd_int_t whitespaceTableLocal) noexcept {
	const uint64_t whitespace  = simd::ws_collector::impl(in_01, whitespaceTableLocal);
	const uint64_t op		   = simd::op_collector::impl(in_01, opTable, spaceMask);
	const uint64_t scalar	   = ~(op | whitespace | simd::rope_detector<rope_block>::quotes);
	const uint64_t follows	   = simd::rope_detector<rope_block>::followsNonquoteScalar(scalar);
	const uint64_t scalarStart = scalar & ~follows;
	return op | simd::rope_detector<rope_block>::quotes | scalarStart;
}
```

Note the overload pair: the minified variant omits the whitespace collector entirely — an entire classification lane deleted at compile time when the caller declares the input minified. This is the same philosophy as the compiler-specific step constants: every fact known before runtime is burned into the instruction stream.

### 4.4 The drain architecture: bits → indices

Extraction — converting each 64-bit structural mask into byte offsets on the tape — is where naive implementations serialize hard, because the classic loop (`tzcnt`, store, `blsr`, repeat) is a loop-carried dependency chain of length popcount.

Jsonifier's answer differs by ISA.

**AVX-512: fully vectorized drain.** With `VBMI2` available, the bitmask never enters a scalar loop at all. `_mm512_maskz_compress_epi8` compresses a constant 0..63 byte ramp under the structural mask, producing the set-bit positions as packed bytes; four `cvtepu8_epi32` widenings plus a broadcast base-add stream up to 64 indices to the tape in a handful of instructions:

```cpp
const __m512i indexes			= _mm512_maskz_compress_epi8(bits,
			  _mm512_set_epi32(0x3f3e3d3c, 0x3b3a3938, 0x37363534, 0x33323130, 0x2f2e2d2c, 0x2b2a2928, 0x27262524, 0x23222120, 0x1f1e1d1c, 0x1b1a1918, 0x17161514, 0x13121110,
				  0x0f0e0d0c, 0x0b0a0908, 0x07060504, 0x03020100));
const __m512i startIndexLocal = _mm512_set1_epi32(base);
__m512i t0					  = _mm512_cvtepu8_epi32(_mm512_castsi512_si128(indexes));
_mm512_storeu_si512(tape, _mm512_add_epi32(t0, startIndexLocal));
```

The widening cascade is guarded by the lane's precomputed popcount (`count > 16`, `> 32`, `> 48`), so sparse blocks pay for one store, not four. simdjson employs the same family of trick: its generic indexer exposes a `CUSTOM_BIT_INDEXER` hook, and the Icelake kernel fills it with its own `VBMI2` compress-based extractor. On this ISA the two libraries again share the inner mechanism, and the performance separation comes from the surrounding step architecture (§4.1, §4.4 drain scheduling below) rather than the extraction primitive.

**AVX2/AVX/NEON: the folded stepped drain.** Without byte-compress, extraction must use the tzcnt chain — but the chain's *structure* is still a compile-time decision. `write_indices_functor` emits one extract-advance pair per index; `write_indices_stepped_functor` groups them into unconditional bursts of `simdTapeStep` writes:

```cpp
template<auto...> struct write_indices_functor {
	using size_type = uint64_t;

	template<uint64_t index> JSONIFIER_INLINE static void impl(size_type base, size_type& bits, write_structural_index_ptr tape) noexcept {
		tape[static_cast<uint64_t>(tag<index>{})] = simd::tape_writer_op::extractIndex(base, bits);
		bits									  = simd::tape_writer_op::advance(bits);
	}
};

template<uint64_t step> struct write_indices_stepped_functor {
	using size_type = uint64_t;
	template<uint64_t index> JSONIFIER_INLINE static bool impl(size_type base, size_type& bits, write_structural_index_ptr tape, uint64_t cnt) noexcept {
		if constexpr (index > 0) {
			if ((index < cnt)) [[unlikely]] {
				functor_runner<write_indices_functor, make_integer_sequence<step>>::impl(base, bits, tape + index);
				return true;
			} else {
				return false;
			}
		} else {
			functor_runner<write_indices_functor, make_integer_sequence<step>>::impl(base, bits, tape + index);
			return true;
		}
	}
};
```

The `functor_runner`'s `implAnd` expands a stepped range sequence `<0, 64, simdTapeStep>` through an `&&`-fold: each group writes `simdTapeStep` indices *unconditionally* (over-writing garbage past the true count is harmless — the tape cursor only advances by the real popcount), and the fold short-circuits the moment a group's start index reaches the count. One predictable branch per `simdTapeStep` extractions instead of one per extraction.

Credit where due: simdjson's current generic `bit_indexer` implements the same stepped-burst pattern — `write_indexes_stepped<START, END, STEP>` via recursive template expansion, unconditional bursts, `simdjson_unlikely` short-circuit checks at each group boundary — with the burst size exposed as a build macro (`SIMDJSON_STRUCTURAL_INDEXER_STEP`, default 4). The two libraries have converged on the extraction inner loop itself. The divergences are in everything around it:

- **Coverage.** simdjson's stepped expansion runs to a fixed `STEP_UNTIL` of 24 set bits and falls back to a plain scalar loop for denser blocks; Jsonifier's fold covers the full 0..64 range at the same stride.
- **Tuning axis.** simdjson's STEP is one global knob; Jsonifier's `simdTapeStep` is swept and pinned per (ISA × compiler) cell, jointly with `simdBlocksPerStep`, because the two constants interact. AVX2 uses a burst of 4 on all three compilers, with 4 blocks per step on Clang and 8 on GCC and MSVC; NEON uses a burst of 8 on both Clang and GCC, at 4 blocks per step (§4.1).
- **Drain scheduling.** This is the structural difference. simdjson pipelines at a depth of one block: each `next()` call drains the *previous* block's structurals while the current block is being classified. Jsonifier defers draining for an entire step — up to eight masks and their popcounts are materialized in `bitsArr`/`cntsArr` before `add_tape_values` drains them back-to-back, each lane's tape destination precomputed from the popcount prefix. Eight independent tzcnt chains with no interleaved classification dependencies, handed to the out-of-order core as one batch.

Above the per-lane drain sits the same fold pattern at block scope. `add_tape_values` drains all blocks of a step, threading the running tape offset through a fold over the lane indices:

```cpp
JSONIFIER_INLINE static void impl(const array<uint64_t, blocksPerStep>& bitsArr, const array<uint64_t, blocksPerStep>& cnts, write_structural_index_ptr tape,
	size_type strIdx) noexcept {
	uint64_t offset = 0;
	(((drainLane<indices>(bitsArr, cnts, tape + offset, strIdx)), offset += cnts[tag<indices>{}]), ...);
}
```

Because per-block popcounts were captured during classification (`cntsArr[I]`), the drains of successive blocks have no data dependence on each other's tzcnt chains — each lane knows its destination offset up front. The classification of blocks *N+1..7* and the drain of block *N* are independent instruction streams the scheduler is free to interleave.

## 5. Stage 2: the tape-driven iterator

Stage 2 is not a separate parser — it is a *specialization* of the same `json_cursor` interface over `write_structural_index_ptr` instead of `read_buffer_ptr`. The reflection-driven parse machinery (`parse_impl`, the dispatch tables, the anti-hash learning) is written once against the cursor interface: `cursor_t<options, context_type>` picks the specialization from the context's `iterator_type`, and the `json_cursor<parseOpts, write_structural_index_ptr>` specialization supplies the token-navigation primitives:

```cpp
template<typename context_type> JSONIFIER_INLINE static bool skipValue(write_structural_index_ptr& iter, context_type& context) noexcept {
	if (iter >= context.endIter) [[unlikely]] {
		return reject<parse_statuses::unexpected_end_of_input>(iter, context);
	}
	const char first = static_cast<char>(*valuePtr(iter, context));
	if (first == '{' || first == '[') {
		int64_t depth{};
		while (iter < context.endIter) {
			depth += nestingDeltaTable[static_cast<uint64_t>(context.stringRoot[*iter])];
			++iter;
			if (depth == 0) {
				return true;
			}
		}
		return reject<parse_statuses::unexpected_string_end>(iter, context);
	}
	++iter;
	return true;
}
```

`nestingDeltaTable` maps `{` and `[` to +1, `}` and `]` to −1 and every other byte to 0, so the loop needs one table load and one add per tape entry and a single branch on the running depth.

This is the payoff that justifies the tape for partial reading: skipping an unwanted value is `++iter`. Skipping an entire unwanted subtree touches only its structural characters — one indexed byte load per structural entry — never the bytes in between. `skipString` is a single increment, because stage 1 already resolved every escape sequence's effect on string extent. In the raw-pointer cursor, by contrast, skipping a string means re-scanning it for an unescaped closing quote (`skipStringImpl`'s memchr-and-check-backslash-parity loop), and skipping a container means walking every byte.

The asymmetry defines the routing rule. When the caller wants *every* value (full-document parse into a reflected type), skips are rare and the tape is overhead. When the caller wants a *few* values from a large document (partial reading), or wants only the structure itself (prettify/minify, where the transformation is literally "copy bytes, adjusting whitespace at structural positions"), skips dominate and the tape converts O(bytes) navigation into O(structurals).

The same routing logic appears in stage 1's own entry point: `reset<minified>` selects between the whitespace-aware and whitespace-free classification pipelines, and the tail-block handling pads with `0x20`, indexes the pad, then retroactively pops any tape entries pointing past the true document length — branchless main loop, exact tape.

## 6. Distributed UTF-8 validation

Let's be precise about what simdjson does, because it is also an in-register scheme: `json_structural_indexer::next` feeds every 64-byte block into `utf8_checker::check_next_input` using the very registers the classifier just loaded. Validation is fused into stage 1, costs no extra pass over memory, and covers the entire input. (The standalone `generic_validate_utf8` exists too, for the buffer-validation API.) On Demand then additionally defers some string-level checks to traversal.

So the distinction is not "in-register versus dedicated pass" — both libraries validate in registers already in flight. The distinction is **scope and location**:

- **simdjson** validates *all input bytes*, in *stage 1*. Elegant and total, but it means the range checker runs over structural characters, whitespace, numbers, and literals — bytes that a JSON parser's own grammar already constrains to ASCII. For a document that is 70% non-string content, 70% of the validation work confirms what token matching would prove for free.
- **Jsonifier** validates *string bytes only*, in the *string parse loop*. Everything outside strings is implicitly ASCII-constrained: structural characters are matched literally, numbers pass through a digit-table parser, `true`/`false`/`null` are compared as packed integer constants — any non-ASCII byte in those positions is a parse error by construction, no range checker required. The only place arbitrary bytes can legally appear is inside strings, and that is exactly — and only — where `utf8_register_validator` runs.

One accounting qualifier belongs in the main text rather than a footnote, because it bounds the size of the claim: simdjson's checker leads with an ASCII fast path — a block whose registers OR to an ASCII-only result skips the multibyte carry chain entirely and pays roughly one reduction and one test. For ASCII-heavy input, then, "validation work proportional to input bytes" is proportional with a small constant, not with the full cost of the multibyte machinery. The scope distinction survives the qualifier — Jsonifier runs *no* validation instructions of any kind over non-string bytes, fast-path or otherwise, and the two approaches still separate cleanly on documents dense in non-ASCII string content — but the honest comparison states the fast path up front rather than burying it.

Jsonifier ships the same range-based algorithm simdjson uses (the `byte1High`/`byte1Low`/`byte2High` nibble-lookup classifier with the `carry`/`tooShort`/`tooLong`/`surrogate` error-bit algebra) in two deployments. The first, the standalone `validateUtf8` built on `utf8_checker`, is the conventional block validator, with the same ASCII fast path (`orAll` the block's registers, one test, skip the multibyte machinery). The second is the distributed one:

```cpp
if (delimiters == static_cast<integer_type>(0)) {
	validator.checkRegister(simdValue);
	string1Start += bytesProcessed;
	string2 += bytesProcessed;
	continue;
}
```

The validator carries `prevInput`/`incompleteRegister` across registers exactly as the block checker carries them across blocks, and `checkPartial` handles the sub-register tail at each quote or backslash boundary by padding with `0x20` (an innocuous ASCII byte) — so multi-byte sequences spanning register boundaries are still caught, and a string ending mid-sequence trips the incomplete carry. Because the string parse loop cascades through progressively narrower SIMD widths, the carry additionally survives width transitions through a compact three-byte `utf8_validation_state` flushed at each width's loop exit and reseeded into the next width's validator — the position-dependent thresholds (last byte ≥ 0xC0, second-to-last ≥ 0xE0, third-to-last ≥ 0xF0) reconstruct exactly the incomplete carry the register-resident validator would have held. The stage-1 companion paper covers this mechanism in full [§6.1 there].

The consequence differs by path. On simdjson's architecture, validation work is proportional to *input bytes* — every block, string or not, at minimum passes through the checker's ASCII test, and any block containing non-ASCII runs the full multibyte carry chain. On Jsonifier's, validation work is proportional to *string bytes*, and on the single-pass full-document path it rides registers the unescaper already loaded, so it never adds a memory pass and never touches non-string content at all. A document that is mostly numbers, structure, and whitespace validates nearly for free; the grammar itself is the validator for everything the range checker skips. There is also a timing difference: simdjson's stage-1 fusion produces its verdict at `check_eof`, after the whole input is scanned; Jsonifier's distributed scheme delivers a strict per-string verdict at each string's closing quote, so an invalid byte fails the parse at the value that contains it. It is always on: there is no option to disable it, because its cost is already confined to string bytes the parser was loading anyway.

## 7. Where the two libraries stand

| Dimension | simdjson | Jsonifier |
|---|---|---|
| Stage-1 usage | Unconditional, all documents | Partial reading, prettify, minify only (benchmarks may force it for comparability; see §2.1) |
| Full-document parse | Stage 1 + On Demand traversal | Single fused pass, schema-directed; faster than its own two-stage path on POD data, on 97 of 100 minified documents and on 30 of 40 prettified documents on the M1; slower on 57 of 60 prettified x86 documents (see §2.2) |
| Target of stage 2 | DOM / lazy generic values | Reflected concrete types via shared cursor interface |
| Out-of-order keys | Forward scan + wrap-around rescan per lookup, per object — no memory across documents | One hash fallback, then learned per-slot order correction (thread-local, per stream) |
| Schema knowledge (known-type workloads) | Exists in caller's traversal code — invisible to the library | Declared once via reflection — consumed by the architecture |
| Step size | 128 bytes (two 64-byte blocks), pipelined one block deep | 256–512 bytes, per-(ISA × compiler) constant |
| Bit extraction | Stepped tzcnt bursts (global STEP macro, scalar tail past 24 bits); VBMI2 compress on Icelake | Same primitives, but full-range folded bursts with per-toolchain-swept stride |
| Drain scheduling | Previous block drained during current block's classification | Whole-step deferred batch drain, lane offsets precomputed from popcounts |
| Escape short-circuit | Yes (zero-backslash fast exit) | Yes (same) |
| In-string prefix-XOR | Computed every block | Skipped for quoteless blocks |
| UTF-8 validation | In-register, fused into stage 1, over all input bytes (ASCII fast path per block); verdict at EOF | In-register, fused into string parsing, over string bytes only, carried across the width cascade; verdict per string |
| Specialization axis | Runtime CPU dispatch through virtual functions (core/DOM API); On Demand mostly fixed at compile time | Runtime tier selection by a cached enum fold over per-tier CRTP instantiations, no virtual calls; everything inside a tier is compile-time (Cathedral Architecture) |

The last row is the root of every other difference. Both libraries can ship one x86 binary that picks its instruction set at run time; they differ in what that choice costs and how much of the code it reaches. Jsonifier's founding constraint is that apart from the data, the only thing decided at run time is the tier, and that it is decided once. The schema, the compiler and the parse options are known at build time, and each ISA tier is a complete compile-time instantiation, so every routing decision in this paper (tape or no tape, whitespace lane or not, validation or not, 4 blocks or 8, burst of 1 or 4) is still resolved before the first byte is read.

On x86, one Jsonifier binary carries every instruction-set tier it was configured for and picks one at run time. The headers are compiled once per tier inside a target-attribute region (`backend_passes.hpp`: `#pragma GCC target` on GCC, `#pragma clang attribute` on Clang), which yields one complete `jsonifier_core_internal<backend>` per tier: AVX-512 (`avx512f`, `avx512bw`, `avx512vbmi2`), AVX2 and AVX. Each of those inherits its tier's parser, serializer, validator, minifier, prettifier and generic iterator through CRTP, so every call inside a tier is a direct, inlinable call into code built for that tier. On first use, `selectSupportedBackend` reads CPUID (leaves 0, 1 and 7, plus the extended leaves for `lzcnt`) and XCR0, so a tier counts only if both the CPU and the operating system support it, and caches the widest supported tier in a function-local `static`. Every public entry point then dispatches with a fold expression over the compiled tiers:

```cpp
static const jsonifier_backend backend{ selectedBackend() };
static_cast<void>(((backend == cores::backendType && (result = cores::template parseJson<options>(internal::forward<value_type>(object), in), true)) || ...));
```

That is a load of a cached enum and at most three compares, once per call. There is no function pointer and no vtable. simdjson's runtime dispatch, by contrast, goes through virtual functions: `implementation` exposes `create_dom_parser_implementation`, `minify` and `validate_utf8` as virtuals, and `dom_parser_implementation` declares `parse`, `stage1`, `stage2` and `parse_string` pure virtual. Its own On Demand documentation notes that the On Demand API "has limited runtime dispatch support" and recommends compiling for a specific x64 target. In Jsonifier the same dispatch covers every API, including typed parsing, serialization and the generic On Demand-style iterator, and inside a tier nothing is virtual, so the per-tier code keeps all of its compile-time specialization.

The scope is x86. ARM builds compile one backend (NEON, or SVE2 when configured), as simdjson's ARM builds do. The set of x86 tiers compiled in is bounded by the configured tier (`JSONIFIER_CONFIGURED_AVX_TIER`), so a build configured for AVX2 carries AVX2 and AVX but not AVX-512. We have not separately measured the cost of the per-call check.

## 8. Conclusion

The two-stage model is a genuinely great algorithm — Jsonifier's stage 1 is an unapologetic descendant of Langdale and Lemire's design, and credits it in source. The contribution here is architectural discipline about *when* to run it. A structural tape is an index, and indexes are worth building exactly when you will not read the whole book. Jsonifier builds it for partial reads and structural transforms, skips it for full parses, validates UTF-8 in the registers it was already holding, and lets the compiler specialize every remaining decision down to per-toolchain loop geometry. §2 shows what skipping the tape is worth, with both paths compiled into one binary and ranked head to head, on both freshly allocated and reused target objects. Across 225 freshly allocated tests on five platforms the fused path wins 154, ties 5 and loses 66, and where it loses is a matter of layout and architecture more than of principle. It wins all 25 POD-type tests, by up to 7.1× (Double under MSVC), 97 of the 100 minified documents, and 30 of the 40 prettified documents on the M1. The two-stage path wins 57 of the 60 prettified documents on x86, all 20 of them under MSVC. The tape, in other words, is not only an index for partial reads: on indented input on x86 it is also the faster way to read the whole book, and the router should learn when. Taking whichever path is faster, Jsonifier beats simdjson On Demand on 221 of those 225 tests, its own stage-1 + reflection path beats simdjson's stage-1 + reflection reader on 65 of 80, and on the partial-reading test, where the index is the right tool, it beats simdjson On Demand on 38 of 40 results and ties the other two. The benchmarks are the receipts.

---

*Jsonifier is MIT-licensed and available at github.com/nihilai-collective/Jsonifier. Benchmark methodology and full sweep data: github.com/nihilai-collective/Json-Performance.*