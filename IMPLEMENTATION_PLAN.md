# FifoFill: end-to-end implementation plan

Prepared 2026-10-15 from [PROJECT_BRIEF.md](PROJECT_BRIEF.md) and the repository as it exists today.

This is a build plan, not an implementation. The deliverable is a deterministic, single-symbol, single-threaded C++20 matching engine, an independent reference engine, evidence of correctness, and reproducible performance results. “End to end” means command input through matching, events, replay, tests, benchmarks, CI, and documentation. There is no frontend or network service in this project.

**Implementation update (2026-10-15):** Steps 01–07 are implemented and locally
verified. See [docs/PROGRESS.md](docs/PROGRESS.md) for results, tool setup, and the
Step 08 network roadblock and resume point. The starting-point description below records the original
planning snapshot.

## 1. Starting point and recommended order

The repository currently contains `README.md` with only the project name, `LICENSE`, and the untracked `PROJECT_BRIEF.md`. There is no application, dependency configuration, test suite, or CI to extend. Preserve the brief and license.

The inspected machine is macOS 15.6.1 on ARM64 with Apple Clang 17. `cmake`, `ctest`, and `ninja` were not found on the current shell's PATH. This does not prove they are absent from the machine. Verify and install the missing build tools during setup; Ninja is optional. No tools were installed while preparing this plan.

Build in this order:

1. Resolve the behavioral contract, including cases the brief leaves unspecified.
2. Build and test the readable reference engine.
3. Create replayable inputs and a baseline benchmark before optimization.
4. Introduce the fast engine and compare it against the reference immediately.
5. Change one storage or lookup mechanism at a time, retaining correctness checks.
6. Run larger fuzz campaigns, measure latency and throughput, and publish evidence.
7. Treat ITCH as a separate extension after the core project ships.

Two changes to the brief's schedule are deliberate: establish CI in week one, and start differential testing with the first fast implementation rather than waiting until all optimizations are complete.

### Definition of done

- Limit, market, cancel, and modify obey the contract below on both engines.
- Every command produces the same ordered event values and canonical final book state in both engines.
- Handwritten cases, independent accounting checks, and sanitizer runs pass.
- A recorded campaign exercises millions of commands across multiple seeds and workload shapes; failures are replayable from saved input, not just a seed.
- The final fast engine performs no heap allocations or deallocations during command processing after construction, including event production. Diagnostic snapshots, file I/O, and benchmark reporting are outside that guarantee.
- Throughput and per-command p50/p99/p99.9 latency have reproducible raw results and an explicit measurement boundary.
- README instructions work from a fresh checkout on the supported platforms.
- Correctness is supported by tests and invariants. Do not describe fuzzing as a mathematical proof or claim a speedup before measuring it.

## 2. Freeze these decisions before writing the matcher

The following are recommended project decisions where the brief is ambiguous, not claims about every real exchange. Step 01 records them in `docs/SEMANTICS.md`. If a decision changes, change its examples and tests before either engine.

| Topic | Recommended v1 contract | Why it matters |
|---|---|---|
| Instrument | One symbol per book instance. No symbol field on every order. | Keeps matching and benchmarks focused. |
| Execution order | Commands execute synchronously in input order. No timestamps determine priority. | Arrival order is deterministic without a clock. |
| IDs | Nonzero `uint64_t`; unique among currently resting orders. Reuse is allowed after removal. | Avoids an unbounded history of every ID ever seen. A delayed cancel can target a reused ID, so clients must avoid reuse if that matters. |
| Duplicate add | Reject an ID that is already live, including a market add. | Never overwrite or accidentally match against an order with the same ID. |
| Price representation | Signed 64-bit integer base units; positive prices only. The example `10125` means $101.25 when the input unit is one cent. | The brief calls these “ticks,” but a monetary base unit and the allowed tick increment must be distinguished. |
| Tick validation | Positive `tick_size`; every limit price is divisible by it and inside an inclusive configured range. Range endpoints are positive and aligned. | An integer price can still violate a five-unit tick increment. |
| Market representation | No limit price, represented explicitly with `std::optional<Price>`. | Neither zero nor a very large price should secretly mean “market.” |
| Quantity | Nonzero `uint32_t`; aggregates and accounting use checked `uint64_t`. | Several valid orders can exceed a 32-bit aggregate. |
| Limit remainder | Rest at the submitted limit, at the tail of that price queue. | A partially filled incoming order has no earlier queue position to retain. |
| Market remainder | Cancel the unfilled remainder after consuming available liquidity. Never rest. | Empty-book market orders still have observable outcomes. |
| Trade price | The resting order's price. | The incoming limit only determines eligibility. |
| Modify fields | `modify(id, new_price, new_remaining)`; side and ID cannot change. | “Size” means outstanding size, not original size or lifetime total. |
| Modify priority | Same price and smaller remaining quantity retain position. Same price and equal quantity are an accepted no-op. Any price change or quantity increase loses priority. | Repricing to the same value is not a price change. |
| Crossing modify | After validation, remove the old queue position and process the replacement as an incoming limit order. It may trade immediately. | Simply moving it into a new queue could leave a crossed book. |
| Zero-size modify | Reject without changing the original order. Use cancel explicitly. | Avoids two subtly different cancellation APIs. |
| Unknown cancel/modify | Reject with `UnknownOrder`, whether never seen, previously filled, or previously canceled. | There is no historical order registry. |
| Failed command | Exactly one rejection event; no book-state change. | Especially important for invalid modifications. |
| Self-trade prevention | Not modeled: there are no participants/accounts in v1. | Do not invent ownership or account rules. |

### Capacity is observable behavior

Use a configured maximum number of resting orders, `C`, in **both** engines. Recommended simple admission policy:

- A new limit add at `live_count == C` is rejected before matching, even if it could execute immediately.
- Market adds can execute at capacity because they never rest.
- A modify reuses its existing slot and is allowed at capacity after normal validation.
- Cancel releases capacity immediately for the next command.

This conservative rule avoids a speculative matching pass or rollback machinery. It is a deliberate limit, not an exchange standard. Document it and test it. If accepting aggressive limits at capacity later becomes a requirement, design a preflight that proves the final resting count will fit; do not partially trade and then reject the remainder as if nothing happened.

Construction validates configuration and all allocation-size arithmetic. Invalid configuration fails before a usable engine exists. The reference engine may allocate during processing; allocation failure is fatal to that run, not a business rejection with promised rollback. The final fast engine preallocates everything needed for valid commands and ordinary rejections.

### Event contract

Keep the brief's four event families, with enough data to explain and compare them:

| Event | Required meaning/data |
|---|---|
| `Accepted` | Add or modify operation, ID, side, effective requested price/market marker, requested remaining quantity; modify also reports whether priority was retained. This acknowledges the request, not its final resting state. |
| `Trade` | Buyer ID, seller ID, resting price, executed quantity. No pointer values or wall-clock timestamps. |
| `Canceled` | ID, removed quantity, and reason: explicit cancel or unfilled market remainder. |
| `Rejected` | Operation, ID, stable reason code. Human-readable text is formatted outside the engine. |

Exact order:

- Valid add: `Accepted`, then zero or more `Trade` events in matching order, then `Canceled` only for an unfilled market remainder.
- Valid modify: `Accepted(Modify)`, then trades if the replacement crosses. Do not emit a fictitious cancel/add pair for an internal priority reset.
- Valid cancel: one `Canceled` event containing all remaining quantity.
- Invalid command: one `Rejected` event.
- Full fills need no additional event; the trade quantity and existing state identify completion.

Pin validation precedence so multi-invalid commands cannot disagree: for add, ID validity → side validity → nonzero quantity → limit price/range/tick validity → duplicate ID → capacity. For modify, ID validity → existence → nonzero quantity → price/range/tick validity. For cancel, ID validity → existence. A malformed text record is a parser error before an engine command exists.

### Worked acceptance traces

Translate the brief's displayed dollars into the chosen integer unit consistently. The table below uses the brief's price labels as integers for readability.

| Setup and command | Required result |
|---|---|
| Asks S1 100@100, S2 50@100, S3 200@101; existing bid B1 300@99; buy B2 220@101 | `Accepted(B2)`, trades B2/S1 100@100, B2/S2 50@100, B2/S3 70@101. S3 remains 130@101; B1 is unchanged. |
| Same initial book, independently; buy B2 400@100 | Two trades totaling 150@100, then B2 rests 250@100. S3 and B1 are unchanged. |
| Empty book; market buy M1 10 | `Accepted(M1)`, `Canceled(M1,10,MarketRemainder)`; empty book. |
| Buy A then B at 100, each 10; shrink A to 5 | A remains ahead of B. A sell for 6 fills A for 5, then B for 1. |
| Same starting queue; increase A to 15 | B becomes first. A sell for 11 fills B for 10, then A for 1. |
| A has 60 remaining after partial fills; modify remaining to 80 | Priority is lost because 80 > 60, regardless of A's original size. |
| Bid A at 99; ask S 5@101; modify A to 10@102 | A trades 5@101, then rests 5@102 if no more asks cross. |
| Invalid reprice or zero-size modify | One rejection; quantity, price, position, and all other orders remain unchanged. |

## 3. Architecture and file responsibilities

Use two concrete engine classes with matching public operations. Tests and benchmarks can instantiate a small templated harness; there is no need for a virtual engine hierarchy or a generic matching framework.

```text
typed commands / trace reader
              |
       replay or test harness
         /               \
ReferenceBook          FastBook
map + list             flat levels + intrusive nodes
standard ID map        fixed order pool + fixed ID index
         \               /
 ordered events + canonical snapshots
              |
 exact comparison / independent ledger / benchmark output
```

Share command types, event types, configuration, and test infrastructure. Keep matching, queue mutation, and priority decisions independently implemented. Sharing the matcher would make the “reference” unable to catch the most important bugs. Test shared validation against explicit expected outcomes because differential testing alone cannot expose a bug shared by both implementations.

Suggested final layout; add files only when their step needs them:

```text
include/fifofill/
  order.hpp                  scalar types, side, Add/Cancel/Modify commands
  events.hpp                 event values, reason codes, equality
  book.hpp                   config, public FastBook API, snapshot value types
src/
  book.cpp                   fast matching and mutation
  order_pool.hpp             fixed node storage and free list
  id_index.hpp               fixed-capacity ID lookup
  price_bitmap.hpp           non-empty level search
reference/
  reference_book.hpp/.cpp    readable independent implementation
tests/
  book_tests.cpp             shared behavioral cases
  storage_tests.cpp          pool, ID index, bitmap boundaries
  differential_tests.cpp     fast vs reference and independent accounting
  allocation_tests.cpp       isolated hot-path allocation guard
  fixtures/                  small handwritten and minimized command traces
bench/
  book_bench.cpp              Google Benchmark throughput comparisons
  latency_bench.cpp           per-command latency sampler
  results/                   small result files plus metadata
tools/
  workload.hpp/.cpp           seeded command generation
  trace.hpp/.cpp              one trace format, reader and writer
  replay.cpp                 reference/fast/compare modes
  fuzz.cpp                   long seeded campaigns and failure artifacts
  itch/                      optional, added only after the core ships
docs/
  SEMANTICS.md               authoritative project choices and examples
  BENCHMARKS.md               methodology and raw-result interpretation
  ITCH_REPLAY_RESEARCH.md     researched limits and extension design
.github/workflows/ci.yml
CMakeLists.txt
CMakePresets.json
README.md
```

The intrusive `prev`/`next` fields belong to internal resting-order storage, not public input objects. No caller receives a node pointer. Do not add original quantity, accounts, timestamps, persistence, or a sequence field unless an actual contract or checker needs it.

### Small public boundary

The intended shape is `process(Command) -> std::span<const Event>`, plus a diagnostic snapshot and invariant check. The returned span refers to a reusable engine-owned buffer and is invalidated by the next `process` call or destruction. Consumers must consume or copy it before then. Snapshots own their values and have a canonical price/FIFO order.

Reserve space for at least `C + 2` events at construction, with checked arithmetic. One incoming command can trade against at most C resting orders; add one acceptance and one possible market-remainder cancellation. This gives a simple output bound without an allocating callback or silent truncation. Keep event objects free of owning strings or heap-backed payloads.

The engine is neither reentrant nor thread-safe. Disable copying/moving the fast book initially so internal pointer ownership cannot accidentally become ambiguous. Debug snapshots may allocate and scan the book; they must not be part of measured command processing.

## 4. Hard parts, investigations, and concrete checks

### 4.1 Modify is a matching operation, not a field assignment

**Failure mode:** modifying a partially filled order uses original size, shrinking moves it to the back, an invalid price deletes it, or a crossing replacement rests without trading.

**Implementation approach:** first validate every field without mutation. Save the old remaining quantity and classify the request. A same-price shrink/no-op updates in place. Otherwise unlink the old queue position, keep ownership of its storage, and match the replacement against the opposite side using its new limit. Reinsert any remainder at the tail. Do not route through the public `cancel` and `add` methods: their event and capacity semantics differ.

**Investigation/check:** write exact traces for all four combinations of same/changed price and lower/equal/higher remaining size, before and after partial fills. Include a replacement that fully trades, one that partly trades, and one rejected at an invalid price. Snapshot the entire book before a rejected request and require exact equality afterward.

### 4.2 Flat prices need a bounded universe

**Failure mode:** treating arbitrary `int64_t` prices as array indices either wastes impossible amounts of memory or permits out-of-bounds access. Negative prices converted to `size_t` are particularly dangerous.

**Implementation approach:** validate once against immutable `min_price`, `max_price`, and `tick_size`; only then compute:

```text
level_count = (max_price - min_price) / tick_size + 1
level_index = (price - min_price) / tick_size
```

Both engines enforce the same range. Reject out-of-range commands; no moving window, recentering, or tree fallback in v1. Validate allocation sizes before conversion/multiplication and reject configurations that do not fit the selected index type or allocation sizes.

**Investigation/check:** calculate memory from actual `sizeof` values: two level arrays + C nodes + ID table + event buffer + bitmap + free-list storage. For illustration only, 100,001 levels at 24 bytes per level on each side cost about 4.8 MB before orders; do not assume the actual ABI uses that size. Benchmark dense and sparse occupancy across narrow and wider ranges. The array can lose to a tree when its working set is large.

### 4.3 A pool alone does not make the engine allocation-free

**Failure mode:** orders come from a pool, but new ID entries, trade vectors, error strings, or logging still allocate. Calling `unordered_map::reserve` does not reserve all future element storage: its specified operation concerns rehashing/bucket count. [C++ unordered-container requirements](https://eel.is/c++draft/unord.req)

**Implementation approach:** retain standard containers in the reference and initial fast baseline, then eliminate one allocation source at a time. The final fast book needs fixed level arrays, a fixed order pool, a fixed ID index, and a reserved event buffer. It does not log from inside `process`.

**Investigation/check:** an isolated test executable counts allocations and deallocations only during `process`, after construction and any harness setup. Cover fresh adds, long churn, crossing adds, markets with large event fanout, modifications, cancellation, and rejection paths. Avoid counting GoogleTest formatting or snapshot construction. Run storage correctness under sanitizers separately if global allocation interception conflicts with instrumentation.

### 4.4 Fixed ID lookup must survive cancellation churn

**Failure mode:** stale IDs reference recycled nodes; deleting an open-addressed entry breaks a collision chain; tombstones eventually consume the whole table despite a small live book.

**Recommended final design:** a small, purpose-specific open-addressed table of `{id, Order*}` with explicit empty state, deterministic integer hashing, bounded probes, and enough slots to keep maximum live load at or below 50%. Use backward-shift deletion with a written wraparound argument; do not casually mark a deleted bucket empty. Do not expose bucket addresses to other structures because deletion may relocate entries.

This custom structure is justified by the explicit allocation-free requirement, not by a presumed speedup. Keep the standard map until this step. A general-purpose container library, allocator framework, or dynamic resize policy is unnecessary.

**Investigation/check:** compare insert/find/erase behavior with `std::unordered_map` in a standalone randomized test. Force collisions, wrap clusters across the last/first bucket, delete first/middle/last cluster members, reuse IDs, and run many more operations than capacity. Verify absence lookups terminate. Test high-bit IDs and `UINT64_MAX`; ID zero is invalid by contract. Expected lookup time is constant under the chosen load/hash assumptions; do not claim a worst-case O(1) hash lookup.

### 4.5 Intrusive queues and recycled storage can fail silently

**Failure mode:** removing the head forgets to repair `prev`; removing the tail leaves a dangling tail; a freed node remains in the ID index; a partial maker fill moves to the back; a growing vector invalidates every pointer.

**Implementation approach:** one queue append/unlink helper inside the fast engine, stable node addresses, and a pool whose storage never grows. On full removal, unlink and remove the ID mapping before recycling. On reprioritization, reuse the node rather than free/reacquire it. The reference uses `std::list` and does not share these helpers.

**Investigation/check:** exercise zero/one/two/many node queues and removal at every position. Debug validation traverses no more than C nodes, checks reciprocal links, head/tail null boundaries, unique membership, index-to-node identity, and free/live disjointness. A stale pointer into a still-allocated pool can evade ordinary use-after-free detection; membership and accounting checks are required even when AddressSanitizer passes.

### 4.6 Best-price search has word-boundary bugs

**Failure mode:** empty best levels remain cached; buy/sell scans go the wrong direction; a bitmap shift by 64 is undefined; a padding bit selects a nonexistent price.

**Implementation approach:** start with cached best indices and linear scanning when a best level empties. Later add one bit per nonempty price on each side. Search words toward lower indices for bids and higher indices for asks, then locate the bit with C++20 bit operations. Keep a simple scan across bitmap words first; add a summary bitmap only if measurements justify it.

The standard specifies `std::countl_zero`/`std::countr_zero` on unsigned types and defines the zero input result. Still test a word for nonzero before treating the count as a valid bit index; a zero-word result is not a usable level. [C++ bit-counting requirements](https://eel.is/c++draft/bit.count)

**Investigation/check:** sizes 1, 63, 64, 65, 127, 128, and 129; empty sides; min/max price; final partial bitmap word; whole-word gaps; deletion of the last best level; empty-to-nonempty transitions. Compare every search result to a simple linear oracle. A single bitmap still has an O(level_count / word_bits) worst-case word scan, not universal O(1).

### 4.7 Differential agreement is necessary but insufficient

**Failure mode:** both engines share a wrong rule and agree, event comparison misses a latent FIFO divergence, or aggregate checks count one trade twice incorrectly.

**Implementation approach:** combine handwritten expected events, independent engine implementations, exact canonical snapshots, and an external quantity ledger. State invariants apply at observable command boundaries, after all synchronous events for that command. Individual trade events can be checked against a pre-command model as they are consumed; do not demand that transient unlink/relink internals look like a finished book.

For cumulative accounting from an initially empty book, define:

```text
A = quantities admitted by successful adds, including markets
I = positive changes to remaining quantity from successful modifies
D = reductions to remaining quantity from successful modifies
X = quantities in explicit-cancel and market-remainder-cancel events
V = sum of trade quantities, counted once per trade
L = sum of remaining quantities currently resting on both sides

A + I = L + D + X + 2*V
```

A reprice with unchanged remaining contributes neither I nor D. Rejected requests contribute nothing. Use checked wide arithmetic and reset accounting between independent scenarios. Per-side accounting consumes V once on each side. Across reused IDs, track distinct accepted lifetimes in the checker rather than merging unrelated orders.

Check after every command: no crossed book; no empty stored levels; positive remaining sizes; correct price/side membership; valid unique live IDs; best prices agree with contents; queues and index agree; pool accounting balances. Compare FIFO order, not just total volume at each price.

### 4.8 A seed alone is not a durable replay artifact

**Failure mode:** a seed produces a different stream after changing the generator, changing standard libraries, or iterating through an unordered map differently. C++ distribution algorithms are implementation-defined, so a fixed PRNG seed plus a standard distribution does not promise identical draws across all libraries. [C++ random-distribution requirements](https://eel.is/c++draft/rand.dist.general)

**Implementation approach:** use a specified PRNG such as `std::mt19937_64`, deterministic bounded-integer sampling with test vectors, a generator version, and stable selection from the live-ID population. Generate each command once and feed that exact value to both engines. Save configuration, seed, generator version, operation index, and the input prefix needed to reproduce a failure. The full saved trace is authoritative.

**Investigation/check:** replay a saved fixture without invoking the generator. Test generator output against a short golden sequence. Verify the mismatch reporter with an intentionally perturbed event in a harness test. Start with reproducible failures and manual minimization; automated trace reduction is optional if diagnosis becomes slow.

### 4.9 Random input must exercise the engine rather than mostly reject

Maintain an explicit live-order population for choosing successful cancels and modifies, and keep invalid requests as a separate controlled component. Update the generator's view from the reference only after both engines agree on that command. Do not independently generate inputs for each implementation.

Use several workloads: clustered prices; many orders at one price; wide sparse prices; cancel-heavy churn; aggressive multi-level sweeps; modify-heavy traffic; tiny capacities; boundary and malformed values. Report realized successful/rejected/crossing proportions, peak live count, and trade fanout. A proposed “60% adds / 25% cancels / 10% modifies / 5% markets” mix is a tunable synthetic scenario, not a verified description of real markets. Keep the generated book from simply filling up and turning the run into rejection benchmarks.

For millions of commands, use bounded live populations so full state comparison and invariants remain affordable. Separate a large-capacity performance run from the fully checked bounded-state correctness campaign; clearly label any reduced validation cadence in larger diagnostic experiments.

### 4.10 Throughput statistics are not tail latency

Google Benchmark supports warmup, repetitions, output files, and statistics over repeated benchmark runs. Those statistics do not automatically give the per-command p99/p99.9 distribution required here. Use its harness for throughput and a separate per-command sampling executable for latency. [Google Benchmark user guide](https://github.com/google/benchmark/blob/main/docs/user_guide.md)

**Throughput boundary:** time processing a pre-generated command batch, including required event production and equivalent observable result consumption for both engines. Exclude generation, parsing, book construction, seeding, snapshots, and output serialization. Recreate/reset the initial state between iterations outside timing so repeated input does not become duplicate-ID rejection traffic. Report commands/second and events/trades per command; “orders/second” alone is ambiguous when cancels and modifies are included.

**Latency boundary:** surround one `process` call with `std::chrono::steady_clock` reads, store durations in preallocated storage, then consume/check its events outside that timed interval. Label the difference from the throughput boundary. Measure empty clock-pair overhead and report it without blindly subtracting it from each sample. Sort outside timing and compute nearest-rank percentiles, index `ceil(p*N)-1` for p in (0,1]. Report sample count for every category.

Separate noncrossing add, crossing add by trade fanout, market, cancel, shrink/no-op modify, priority-losing modify, and rejection. Aim for at least one million samples in each category whose p99.9 is emphasized; otherwise disclose the limited tail sample count. A million total commands split across many categories is not a million samples per category.

Run optimized builds with sanitizers/invariant scans disabled. Keep both engines on identical command sequences and configuration, compiler, flags, machine, event handling, and measurement boundaries. Record CPU model, OS, compiler, commit, build flags, seed/trace hash, capacity, price range, load, warmup, and repetitions. On this ARM Mac, avoid x86-only cycle counters and Linux-only affinity assumptions. This is engine service time under a single-threaded driver, not network latency or queueing latency under offered load.

### 4.11 ITCH needs a different interpretation

Nasdaq's feed describes book changes and executions; it is not a log of FifoFill's incoming commands. Its execution message identifies an existing order, whereas the missing aggressor can contain information the feed does not reveal. A generated market order can consume a different visible queue head than the reported execution. Therefore “translate each fill into a market order and compare to the exchange” is not a correctness oracle. [Nasdaq TotalView-ITCH specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf)

The optional extension should first reconstruct a source book from feed messages, then produce an explicitly labeled derived workload only where its assumptions hold. Keep feed reconstruction and engine comparison as separate questions. The detailed evidence, parser risks, and stop/go criteria are in [ITCH_REPLAY_RESEARCH.md](docs/ITCH_REPLAY_RESEARCH.md). No real-data throughput claim is justified until a file has actually been parsed and its supported coverage measured.

## 5. Step-by-step implementation prompts

Run one prompt per focused change. Every step includes its prerequisite, scope, and completion gate. The prompts describe proposed future work; none of the commands or targets below exists yet unless that step creates it.

Use this short preamble with each prompt:

```text
Read PROJECT_BRIEF.md, IMPLEMENTATION_PLAN.md, and the current code relevant to
this step. Follow docs/SEMANTICS.md once it exists. Implement only the requested
step, preserving prior behavior. Inspect affected callers, tests, fixtures,
build targets, and documentation. Use the smallest complete change. Run the
step's checks and report actual commands/results and any unmet gate. Do not
invent performance numbers or mark an unrun check as passed.
```

### Step 01 — Turn the brief into an executable contract

**Depends on:** nothing. **Touches:** `docs/SEMANTICS.md`; no engine code.

```text
Write docs/SEMANTICS.md using the proposed decisions in section 2 of
IMPLEMENTATION_PLAN.md. Define units, ticks, IDs and reuse, validation order,
capacity admission, modify remaining-size semantics, priority rules, event
fields/order, market remainder cancellation, output span lifetime, and failure
behavior. Include exact input/event/final-book traces for the brief's two
examples and every modify classification. Mark these as project rules rather
than universal exchange behavior. Resolve inconsistencies in writing now;
do not implement the engine or broaden the scope.
```

**Gate:** the worked traces can be evaluated manually without guessing quantities, IDs, prices, rejection precedence, or queue order. The brief and license remain unchanged.

### Step 02 — Establish C++20, tests, and early CI

**Depends on:** 01. **Touches:** CMake files, `.gitignore`, first test target, CI, setup instructions.

```text
Create the minimal CMake C++20 project with target-scoped warnings, extensions
disabled, GoogleTest, CTest discovery, and Debug, sanitizer, and Release
presets. Verify local compiler/build-tool availability; report missing tools.
Pin dependency sources to immutable revisions/checksums and record the chosen
versions. Start with CMake 3.24 or newer as a project minimum, raising it only
if a selected dependency requires it. Enable ASan plus UBSan for supported
Clang/GCC builds, with compile and link flags and useful stack traces. Add a
small meaningful C++20/build test and GitHub Actions Linux test/sanitizer jobs.
Do not install dependencies into system locations or add application behavior
as part of this scaffold. Do not fetch Google Benchmark until its step needs it.
```

**Gate:** configure, build, and `ctest` work locally when prerequisites are available; CI is syntactically valid and tests run when pushed. Local execution is not a claim that hosted CI has passed. GoogleTest's official quickstart uses CMake FetchContent, and CMake's `gtest_discover_tests` discovers parameterized tests from the executable. [GoogleTest quickstart](https://github.com/google/googletest/blob/main/docs/quickstart-cmake.md), [CMake test discovery](https://cmake.org/cmake/help/latest/module/GoogleTest.html)

### Step 03 — Define values and trust-boundary validation

**Depends on:** 01–02. **Touches:** public types/events/config, validation tests.

```text
Define Price, Qty, OrderId, Side, Add/Cancel/Modify command values, and the four
event families specified by docs/SEMANTICS.md. Represent market orders with an
absent limit price. Define BookConfig and checked price-to-index/config sizing
validation. Add fieldwise event equality and canonical snapshot value types.
Keep node links out of public command types and owning strings out of events.
Test zero and extreme values, invalid sides, tick alignment, min/max range,
capacity/config overflow, and event value equality. Do not add matching yet.
```

**Gate:** error precedence and integer conversions have explicit tests. No signed arithmetic is performed on an unchecked price difference; no negative input is silently cast to an unsigned size.

### Step 04 — Reference adds, matching, and markets

**Depends on:** 03. **Touches:** reference engine, add/matching tests, output buffer.

```text
Implement ReferenceBook using sorted price maps, std::list FIFO queues, and a
standard ID lookup containing stable order handles. Implement limit and market
adds with independent straightforward matching logic, maker-price trades,
partial fills, duplicate validation, conservative capacity admission, and the
exact event contract. Reserve the bounded event buffer at construction. Provide
an owning canonical snapshot for tests. Add handwritten tests for the brief's
two examples, both sides, same-price FIFO, sweeps, empty books, and market
remainder cancellation. Favor readable code over a forced 200-line target.
```

**Gate:** expected events and every remaining order match handwritten answers. No zero-size order or empty price level remains stored. Filling a maker completely also removes its ID lookup.

### Step 05 — Reference cancellation and modification

**Depends on:** 04. **Touches:** reference engine, priority/rejection tests, invariants.

```text
Implement ReferenceBook cancel and modify exactly as specified. Validate a
modify before mutation; distinguish in-place shrink/no-op from priority-losing
replacement; allow replacements to match immediately. Preserve ID and side.
Return the defined events without implementing modify as public cancel+add.
Add debug book invariants and tests for queue-head/middle/tail cancellation,
unknown and completed IDs, ID reuse, invalid modifications, partial-fill
modification, crossing replacements, and behavior at capacity.
```

**Gate:** every rejected command preserves the complete snapshot. Priority tests use multiple orders at a price so a wrong queue position is observable.

### Step 06 — Complete the behavioral test matrix

**Depends on:** 05. **Touches:** shared test harness, fixtures, accounting checker.

```text
Turn the contract cases into reusable tests that can later instantiate either
engine without sharing matching logic. Add an independent quantity ledger using
the equation in section 4.7, exact canonical snapshot comparisons, and tests
for maximum Qty, multi-order aggregate overflow boundaries, rejection precedence,
and maximum event fanout. Cover mirror-image buy/sell behavior, empty/nonempty
transitions, and all modify classes. Store a few readable golden traces. Do not
replace precise expected-value tests with only invariant or snapshot assertions.
```

**Gate:** the reference passes the whole matrix under normal and sanitizer builds. Deliberately changing a maker price or priority in a test double/fixture causes the harness to fail.

### Step 07 — Deterministic generation and replay

**Depends on:** 06. **Touches:** workload generator, trace reader/writer, replay tool and tests.

```text
Implement a versioned, line-oriented integer command trace format with a config
header and optional comment lines; use the same reader/writer for fixtures and
fuzz failures. Parse strictly with checked integer conversion and clear line
errors, rejecting trailing garbage and overflow before engine execution. Add
seeded workload generation with stable live-ID selection and deterministic
bounded sampling. Provide reference replay and workload export modes. Record
seed, generator version, config, and operation count. Test a golden generator
sequence, round-trip traces, malformed input, and replay independent of RNG.
```

**Gate:** the same trace reproduces the same events twice; a malformed record is not silently skipped. The generator produces successful cancels/modifies as well as explicit invalid scenarios.

### Step 08 — Measure the reference baseline

**Depends on:** 07. **Touches:** Google Benchmark target, scenarios, benchmark notes.

```text
Add Google Benchmark as a pinned optional build dependency and a reference
throughput harness. Pre-generate deterministic batches, seed the initial book
outside timing, reset between iterations outside timing, and consume output
equivalently using an observable checksum. Include clustered steady-state flow,
same-price depth, sparse prices, cancel churn, and multi-level sweeps. Add a
short benchmark smoke run and docs/BENCHMARKS.md describing the exact timing
boundary and metadata. Save raw baseline output if measurements can actually
run; otherwise document the missing prerequisite without a placeholder number.
```

**Gate:** rerunning a batch starts from the same state and has the same outcome counts. Benchmarks do not gradually turn into duplicate rejections. Record the baseline commit or exact source state for later comparisons.

### Step 09 — Fast baseline plus differential checking

**Depends on:** 08. **Touches:** FastBook, flat price storage, shared tests, compare replay/fuzz.

```text
Implement FastBook with the same public contract, replacing price maps with
fixed flat arrays of standard FIFO lists. Keep the standard ID lookup for now.
Cache best indices and scan linearly only when needed; do not add a bitmap or
pool yet. Implement matching independently from ReferenceBook. Instantiate all
behavioral tests for both engines. Extend replay/fuzz to compare every event
field in order, canonical FIFO snapshots, and independent invariants after
every command, and save a replayable input prefix on disagreement. Run fixed
seeds and all workload shapes, then measure this version against the reference.
```

**Gate:** full behavior parity, useful replayable mismatch reports, and one measured storage-change result. Validate the reporter with an injected harness mismatch; do not leave a deliberate engine defect in production code.

### Step 10 — Replace list nodes with intrusive order links

**Depends on:** 09. **Touches:** fast queues/nodes, ID handles, storage tests.

```text
Replace only FastBook's std::list queues with internal intrusive Order nodes and
head/tail links per price level. Keep allocation ordinary for this step and
make ownership/destruction explicit. Centralize queue append/unlink within the
fast implementation; do not reuse it in the reference. Keep partially filled
makers in place. Add bounded debug traversal, reciprocal-link/membership checks,
and tests for every queue-removal position and reprioritization. Run all shared
tests, seeded differential checks, and sanitizers; record the benchmark delta.
```

**Gate:** no leaks, lost nodes, duplicate queue membership, or event changes. Copy/move policy is explicit; no accidental shallow copy owns the same nodes.

### Step 11 — Preallocate and recycle orders

**Depends on:** 10. **Touches:** order pool, fast node ownership, capacity/churn tests.

```text
Replace dynamic FastBook order allocation with fixed storage sized at
construction and a nonallocating free list. Never grow or relocate that storage.
Remove ID mappings before recycling completed/canceled nodes; retain a node
during priority-losing modification. Enforce the already agreed capacity policy.
Test full capacity, market matching at capacity, modify at capacity, repeated
reuse, and free/live accounting. Verify that order storage no longer allocates,
while explicitly recording that the standard ID map may still allocate. Run
parity and sanitizer checks and record the incremental benchmark result.
```

**Gate:** `free_count + live_count == C` at command boundaries and all live/index/queue memberships agree. No claim of a fully allocation-free engine yet.

### Step 12 — Remove ID-index hot-path allocations

**Depends on:** 11. **Touches:** fixed ID index, storage tests, allocation test executable.

```text
Replace only the fast ID map with the fixed open-addressed index designed in
section 4.4. Write down collision probing, deletion wraparound, maximum load,
and termination invariants. Use deterministic hashing, bounded probes, and
backward-shift deletion; no growth or tombstone buildup. First test this index
against std::unordered_map through collisions and long churn, then integrate it.
Add isolated process-only allocation/deallocation checks covering every command
and rejection path plus maximum event fanout. Run full parity/sanitizer checks
and record both allocation evidence and the performance change.
```

**Gate:** zero command-processing heap activity after construction, no lost collision-chain entries, stable memory usage after millions of inserts/deletes. A speed regression must be reported even if the fixed-allocation goal is achieved.

### Step 13 — Accelerate best-level discovery

**Depends on:** 12. **Touches:** bitmap helper, nonempty-level transitions, best caches.

```text
Add per-side nonempty-level bitmaps to FastBook while preserving the existing
best-price contract. Set/clear bits only when a queue crosses empty/nonempty.
Use C++20 unsigned bit operations, guard word boundaries and padding, and scan
words in the correct direction. Compare the helper with a linear oracle across
all boundary sizes from section 4.6. Do not add hierarchical summaries yet.
Run all engine tests, differential fuzz, allocation checks, and sanitizer jobs.
Measure narrow/dense and wide/sparse scenarios and record the change honestly.
```

**Gate:** bitmap, queue contents, and best caches agree after every command. No performance claim assumes a bitmap always beats a short linear scan.

### Step 14 — Run and harden the correctness campaign

**Depends on:** 13. **Touches:** fuzz scenarios, regression traces, CI artifacts and schedules.

```text
Expand differential campaigns to millions of commands across several seeds and
all listed workload shapes, including tiny capacities and integer boundaries.
Keep full event/state/invariant comparison after every command in the bounded
correctness campaigns. Save seed/version/config/index plus the replayable prefix
and both outputs on failure; exit nonzero. Add small fixed campaigns to normal
CI and larger manual/scheduled runs with failure artifacts. Reproduce each real
failure and keep a minimal regression case before fixing the shared root cause.
Record actual seed/count coverage and sanitizer status, not just elapsed time.
```

**Gate:** the documented campaign completes without divergence, invariant failure, or sanitizer errors; a saved failure can be replayed using one documented command. No claim that random testing exhausts the state space.

### Step 15 — Implement trustworthy latency measurements

**Depends on:** 14. **Touches:** latency executable, percentile tests, result metadata.

```text
Implement the separate steady_clock per-command latency harness described in
section 4.10. Preallocate samples and generate/seed outside timing. Measure
process including event production; consume events afterward. Report empty
clock-pair overhead, sample counts, p50/p99/p99.9, operation/result categories,
and trade fanout. Unit-test nearest-rank percentiles on known values and empty/
small inputs. Compare both engines on identical traces and report the exact
timing boundary. Collect enough samples for every emphasized tail category,
and report undersampled categories explicitly. Do not use sanitizer builds.
```

**Gate:** percentiles are computed over individual commands, not benchmark-run averages; raw output retains counts and metadata. Clock overhead and warmup are disclosed.

### Step 16 — Profile and keep only justified refinements

**Depends on:** 15. **Touches:** only a measured bottleneck, associated regression checks/results.

```text
Inspect the baseline and incremental results and profile representative fast
engine workloads using an available platform profiler. Identify one measured
bottleneck and state a testable hypothesis. Make at most one targeted refinement
at a time; compare correctness, allocations, memory, throughput, and tails under
the same conditions. Keep it only if the benefit or required guarantee justifies
the complexity. Consider a summary bitmap only if empty-word scanning matters.
If no additional change is justified, document that result and finish this step
without adding code. Do not add multithreading, SIMD, custom assembly, or a new
container dependency to chase an arbitrary headline number.
```

**Gate:** each retained change has a comparable result row and tests; inconclusive or negative results are labeled. Do not retain many runtime implementation variants solely for the README—use recorded commits and raw results.

### Step 17 — Finish CI, reproducibility, and the public README

**Depends on:** 16. **Touches:** documentation, CI completion, build cleanup, small results artifacts.

```text
Complete the README with one paragraph, a book diagram, build/test/replay/fuzz/
benchmark commands, verified results, correctness evidence, design tradeoffs,
and explicit limitations. Link SEMANTICS and BENCHMARKS. Finish Linux GCC/Clang
and macOS compile/test coverage appropriate to available runners, a Clang
ASan+UBSan job, fixed fuzz smoke checks, and benchmark build/smoke only. Keep
performance thresholds off shared CI runners. Verify a fresh checkout build and
all documented commands; report hosted jobs separately from local checks.
Record dependency/toolchain versions and raw benchmark metadata. Preserve the
license, keep large generated files out of git, and do not present future ITCH
work or unrun benchmarks as completed features.
```

**Gate:** the core definition of done in section 1 is met. If any item is unmet, list it explicitly and keep this milestone incomplete. The README explains integer prices, maker pricing, single-threaded ownership, differential verification, the measured bottleneck, range/capacity policy, and what the benchmark excludes.

### Step 18 — Optional ITCH feasibility spike

**Depends on:** 17. **Touches:** optional research/manifest; only bounded parsing experiments.

```text
Read docs/ITCH_REPLAY_RESEARCH.md and recheck the official Nasdaq specification,
file framing documentation, and current sample listing. Select one symbol and
a bounded input experiment; record source filename/date/hash, units, framing,
supported message types, and file size before any large download. Determine
whether complete source-book state can be reconstructed and whether candidate
execution translations satisfy the mini-engine's maker-price/FIFO assumptions.
Record mismatches and supported coverage. Choose explicitly between structural
book replay and a labeled feed-derived matching workload. Do not promise exact
reconstruction of missing aggressor orders or alter the core matcher to make
incompatible feed events appear correct.
```

**Gate:** written feasibility decision and measured coverage. If a faithful matching translation is unsupported, retain that finding and proceed only with structural replay or a clearly bounded derived workload.

### Step 19 — Optional binary decoder and source-book reconstruction

**Depends on:** successful, scoped 18. **Touches:** `tools/itch/`, byte fixtures, optional data manifest.

```text
Implement the chosen ITCH subset as a separate tool. Decode bytes with explicit
length checks and big-endian integer reads; do not cast file bytes to packed C++
structs. Keep source price precision lossless. Reconstruct visible source orders
with exact add, execution, partial-cancel, delete, and replacement semantics,
including new IDs on replacements. Process source order rather than sorting by
timestamp, build state before any measured interval, and track unsupported
message classes and discontinuities. Test hand-built binary fixtures, truncation,
wrong lengths, unknown references, ID replacement, and quantity underflow. Keep
feed reconstruction rules out of ReferenceBook and FastBook.
```

**Gate:** known fixtures reconstruct expected state; malformed data cannot overread; real-data coverage and assumptions are reported. Compression/framing/parsing cost is distinguishable from book mutation cost.

### Step 20 — Optional derived workload and honest real-data reporting

**Depends on:** 19 plus a feasible conversion policy. **Touches:** adapter, parity fixtures, benchmark notes.

```text
Implement only the conversion policy validated by the spike. Keep source-book
state separate from the matching-engine state. Verify FIFO target and maker
price compatibility before translating a reported execution; stop or segment
the experiment at incompatible book-affecting events using a documented state
reconstruction policy. Never silently skip one and continue claiming exact
replay. Use collision-free synthetic IDs for invented aggressors. Feed the same
derived commands to both engines, verify them differentially, and report source
coverage, exclusions, reconstruction boundaries, and checksums. Benchmark
decompression/parsing, source reconstruction, and engine commands separately.
Label derived workload results as derived, not the original exchange command
stream. If conversion is unsuitable, ship source reconstruction alone with an
accurate scope statement.
```

**Gate:** the real-data claim is exactly as strong as the observed evidence. The synthetic-only core release remains complete without this extension.

## 6. Checks, target commands, and result format

Agree these executable/preset names during setup and keep documentation aligned. The following are intended final commands, **not commands run while making this plan**:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure

cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure

cmake --preset release
cmake --build --preset release
./build/release/fifofill_replay --engine compare --input tests/fixtures/sweep.ff
./build/release/fifofill_fuzz --seed 42 --commands 1000000 --scenario mixed
./build/release/fifofill_bench --benchmark_out=bench/results/throughput.json
./build/release/fifofill_latency --input bench/workloads/mixed.ff --output bench/results/latency.csv
```

The fixture/workload names are proposed, and the large benchmark trace should be generated locally, with its seed/config/hash recorded rather than committed. Add the exact workload-generation command when the tool exists. Build presets must use matching output paths. Google Benchmark JSON and the latency CSV should be accompanied by a small metadata file, not hand-copied numbers alone.

### Test matrix to keep visible during implementation

| Layer | Essential cases | When it runs |
|---|---|---|
| Contract | Maker price, FIFO, both sides, multi-level sweeps, partial/full fill, markets, modify classes, rejects | Every change |
| Boundaries | Tick/range, zero/max quantity, invalid enum, ID reuse/duplicates, capacity, checked sizing | Every change |
| Storage | Intrusive links, pool recycling, hash collision/deletion churn, bitmap word boundaries | Every storage change and CI |
| Differential | Exact events plus FIFO snapshots and independent accounting | From first FastBook onward |
| Sanitizers | Reference, fast storage, replay parsing, fixed fuzz campaigns | Local risky changes and CI |
| Allocation | No process-time heap activity after construction | Final storage changes and CI |
| Benchmarks | Fresh-state throughput, per-command tails, memory, scenario/outcome counts | Baseline and every optimization |
| Optional ITCH | Binary bounds, source semantics, unsupported coverage, adapter compatibility | Only extension work |

Use compile and link instrumentation together for AddressSanitizer and frame pointers for useful stack traces. UBSan covers errors such as signed overflow and invalid shifts; ordinary unsigned wrap is not automatically caught by `-fsanitize=undefined`, so quantity checks remain necessary. [Clang AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html), [Clang UBSan](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)

### Results table template

Do not populate with estimated or borrowed numbers. Each comparison needs the same workload and machine, and a raw-result reference.

| Version/change | Commit | Scenario/config | Commands/s | p50 ns | p99 ns | p99.9 ns | Allocations/command | Resident/storage memory | Raw results |
|---|---|---|---:|---:|---:|---:|---:|---:|---|
| Reference baseline | TBD | TBD | — | — | — | — | — | — | — |
| Flat levels + cached best | TBD | same | — | — | — | — | — | — | — |
| Intrusive FIFO | TBD | same | — | — | — | — | — | — | — |
| Order pool | TBD | same | — | — | — | — | — | — | — |
| Fixed ID index | TBD | same | — | — | — | — | — | — | — |
| Bitmap search | TBD | same | — | — | — | — | — | — | — |

Earlier steps initially collect throughput and allocation evidence; fill their latency columns later by rebuilding the recorded versions under the same final harness, or leave them explicitly unmeasured. Do not mix latency categories into a single unexplained percentile. Report speedup as a ratio of like-for-like measurements, and distinguish theoretical owned storage from process RSS.

## 7. Six-week working schedule and scope control

This is a planning estimate, not a deadline guarantee. The custom ID index, failure minimization, and trustworthy tail measurements are the likely schedule risks.

| Week | Steps | Exit condition |
|---|---|---|
| 1 | 01–06 | Contract, toolchain/CI, complete tested reference |
| 2 | 07–09 | Replays, baseline measurement, first fast engine with differential checks |
| 3 | 10–13 | Intrusive queues, pool, fixed lookup, best-level bitmap |
| 4 | 14 | Multi-seed million-command correctness evidence and regressions fixed |
| 5 | 15–16 | Reproducible throughput/tails, measured targeted refinement if justified |
| 6 | 17 | Clean-checkout verification, final CI, README and raw results |
| Later | 18–20 | Separately scoped ITCH reconstruction/derived replay |

If time slips, defer ITCH, extra profiler experiments, automated trace minimization, and summary bitmaps. Do not cut validation, modify correctness, event determinism, independent checking, storage safety, or honest measurement. The initial reference's approximate 200-line goal should encourage readability; it must not force omission of required behavior.

No GUI, network protocol, real money, exchange connectivity, trading strategy, threads, crypto, auctions, advanced order types, persistence/recovery service, or multi-symbol orchestration belongs to this core release. A future real exchange would need many of those operational and market-rule decisions; this project makes a narrower engineering claim.

## 8. Evidence used and remaining uncertainty

- Repository and available local compiler/build commands were inspected on 2026-10-15. No application code existed to build or benchmark.
- External technical claims above were checked against official tool documentation, the C++ working draft, and Nasdaq documentation. Proposed data structures and project policies are recommendations, not measured findings.
- CMake's compile-feature mechanism can express the C++20 requirement; explicitly disable extensions and verify the actual chosen compiler supports the used features. [CMake compile features](https://cmake.org/cmake/help/latest/manual/cmake-compile-features.7.html)
- ITCH sample availability and format limits were researched; no multi-gigabyte sample was downloaded, parsed, or validated. See the linked research note for the bounded extension plan.
- No runtime tests, fuzz campaigns, allocation checks, benchmarks, or hosted CI were executed because this task produced a plan, not an engine.
