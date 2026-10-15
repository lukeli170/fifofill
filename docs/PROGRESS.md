# Implementation progress

Recorded 2026-10-15. Requested phases 1–5 were interpreted as Steps 01–05
of [IMPLEMENTATION_PLAN.md](../IMPLEMENTATION_PLAN.md).

| Step | Status | Delivered |
|---|---|---|
| 01 — Contract | Complete | [SEMANTICS.md](SEMANTICS.md): units, validation precedence, capacity, events, modify classifications, exact traces, API lifetimes/failures |
| 02 — Toolchain and CI | Complete locally; hosted CI pending | C++20 CMake >= 3.24, checksum/commit-pinned GoogleTest 1.17.0, CTest, Debug/ASan+UBSan/Release presets, Linux Clang CI |
| 03 — Values and validation | Complete | Scalar/command/event values, checked config and price indices, canonical owning snapshots, boundary/equality tests |
| 04 — Reference matching | Complete | Map/list FIFO book, stable ID handles, maker-price matching, limits, markets, bounded output buffer, exact events/state tests |
| 05 — Cancel and modify | Complete | Cancellation, atomic business rejections, retained/reset priority, crossing replacements, ID reuse, capacity rules, debug invariants |

No human setup or external-service contingency blocked these steps. The brief
and license are unchanged. No system packages were installed.

## Verification

On macOS ARM64, Apple Clang 17.0.0, CMake 3.31.6, Unix Makefiles:

- Configure/build/CTest: **26/26 pass in each of debug, asan, and release**.
- Sanitizer build instruments compilation and linking with AddressSanitizer and
  UndefinedBehaviorSanitizer; UBSan ran with stack traces and halt-on-error.
- Standalone Release library configure/build with `BUILD_TESTING=OFF` passes
  without fetching GoogleTest.
- Workflow YAML parses and its build matrix matches the three CMake presets.
- Build logs show no project warnings. `git diff --check` passes.

CMake was missing from PATH. It was installed only under
`/private/tmp/fifofill-build-tools` using pip. To reuse it while it exists:

```sh
export PATH="/private/tmp/fifofill-build-tools/cmake/data/bin:$PATH"
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
```

That temporary path is machine-local and can be removed by OS cleanup. Other
checkouts need CMake >= 3.24 on PATH, as documented in the README. Hosted CI and
Linux execution have not run in this session.

## Steps 06–10 request

Steps 06 and 07 are complete. Work stopped before Step 08 at the network
roadblock below, following the instruction to stop on roadblocks or contingencies
requiring human approval. No changes to reference matching logic were needed.

| Step | Status | Delivered |
|---|---|---|
| 06 — Behavioral matrix | Complete | Existing exact-event tests converted to a reusable typed matrix, independent event-driven quantity/FIFO ledger, maximum aggregate boundary check, readable fixtures, deliberately perturbed maker-price/FIFO/market-remainder checks |
| 07 — Generation and replay | Complete | Versioned strict trace reader/writer, portable bounded sampling with golden vectors and a golden command sequence, nine workload shapes with stable live-ID selection, reference replay/export CLI, fixture and CLI smoke checks |
| 08 — Reference baseline | Blocked before implementation | Google Benchmark dependency download cannot resolve `codeload.github.com` in the current restricted environment |
| 09 — Flat-list FastBook | Pending | Depends on Step 08's measured baseline |
| 10 — Intrusive FastBook | Pending | Depends on Step 09's parity and measurement gates |

Current local verification on the same macOS ARM64 / Apple Clang 17 setup:

- Configure, build, CTest: **38/38 pass in each of debug, asan, and release**.
- ASan and UBSan run together, with UBSan stack traces and halt-on-error.
- All nine generated scenarios replay with quantity/FIFO accounting; each has
  successful cancels and modifies and controlled rejections (under 25% for seed
  42, 2,000 commands per scenario). This is bounded verification, not the later
  multi-million-command differential campaign.
- Three handwritten traces replay twice with identical complete event values
  and exact final snapshots, without invoking the generator.
- CTest exercises export and replay of a 1,000-command generated trace.
- Release configure/build with `BUILD_TESTING=OFF` also builds the replay tool
  without dependency downloads.
- No project compiler warnings; `git diff --check` passes.

The ledger tracks distinct accepted lifetimes across reused IDs, priority changes,
and checked uint64 per-side accounting (`A + I = L + D + X + V`). It checks maker
price, best-price/FIFO consumption, full executable trade quantities, and market
remainder cancellation. It derives expected snapshots from consumed events;
it does not call reference matching code. Precise expected-event tests remain.

## Resume point and roadblock

Resume at **Step 08**, before adding the optional pinned Google Benchmark target.
A dependency fetch returned `curl: (6) Could not resolve host: codeload.github.com`.
No dependency was installed, no benchmark source/target was added, no escalation
was requested, and no performance result is claimed. Network access for fetching
Google Benchmark, or an accessible local source archive, is needed to continue.
The user's requested Steps 09–10 depend on that baseline and were not started.

The reference intentionally allocates during processing. FastBook, differential
fuzz campaigns, allocation guarantees, latency measurement, and ITCH remain later
work. Replay/generator usage and the trace grammar are in [README.md](../README.md).
