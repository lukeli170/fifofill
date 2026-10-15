# FifoFill

A deterministic, single-symbol, single-threaded C++20 matching engine. The
reference implementation uses sorted price maps and FIFO lists; it is the
answer key for the later optimized engine. Project rules live in
[docs/SEMANTICS.md](docs/SEMANTICS.md), with the roadmap in
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). Steps 01–07 are implemented;
[docs/PROGRESS.md](docs/PROGRESS.md) records verification and the resume point.

Requires a C++20 Clang or GCC compiler, CMake >= 3.24, Make, and network access
for the first test configuration. GoogleTest 1.17.0 is pinned to commit
`52eb8108c5bdec04579160ae17225d66034bd723` and an archive checksum in CMake;
it stays in the build tree. No system dependency install is performed.
The setup follows the [GoogleTest CMake quickstart](https://google.github.io/googletest/quickstart-cmake.html).

```sh
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug

cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset asan

cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
```

`asan` enables AddressSanitizer and UndefinedBehaviorSanitizer with frame
pointers. Linux CI runs all three presets with Clang. To build the library
without downloading test dependencies, configure with `-DBUILD_TESTING=OFF`.

The behavioral matrix checks exact events and FIFO snapshots, with an independent
event-driven quantity ledger after every command. It includes mirror-image
matching, all modify classes, rejection precedence, capacity, maximum quantities,
and injected bad-output checks. The ledger and diagnostic snapshots allocate;
they belong to correctness tooling, outside future benchmark timing.

Generate a deterministic workload, or replay one with reference invariants and
quantity accounting checked after every command:

```sh
./build/release/fifofill_replay --seed 42 --commands 10000 --scenario mixed --output build/mixed.ff
./build/release/fifofill_replay --engine reference --input build/mixed.ff
./build/release/fifofill_replay --engine reference --input tests/fixtures/sweep.ff
```

Scenarios: `mixed`, `clustered`, `same-price`, `sparse`, `cancel-churn`, `sweeps`,
`modify-heavy`, `tiny`, and `boundaries`. They are synthetic test shapes. The
generator selects live IDs from canonical snapshots and uses `mt19937_64` with
specified rejection sampling, rather than standard-library distributions.

Trace version 1 uses the following records in order. Blank lines and full-line
`#` comments are allowed. `META` stores the seed, generator version (0 for
handwritten traces), and exact command count. All numeric conversions are
checked; malformed records, unsupported format versions, missing records, and
trailing fields fail with a line number before any command executes.

```text
FIFOFILL 1
CONFIG 1 1000 1 10
META 0 0 3
ADD 1 S 100 10
MODIFY 1 101 5
CANCEL 1
```

`CONFIG` fields are minimum price, maximum price, tick size, and resting capacity.
Commands are `ADD id side price quantity`, `MODIFY id price quantity`, and
`CANCEL id`. Side is `B` or `S`; numeric uint8 values preserve deliberately invalid
enums for rejection tests. `M` replaces an add's price for a market order.
Well-formed commands with invalid business values reach the engine and reject
normally. Saved traces are authoritative; future generator versions may differ.

Step 08 is blocked locally by network/DNS access to the Google Benchmark
dependency. Steps 08–10 remain pending; see [progress](docs/PROGRESS.md).
