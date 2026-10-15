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

## Resume point

Next is **Step 06**, the reusable behavioral test matrix and independent
quantity ledger. The reference intentionally allocates during processing.
FastBook, replay/generation, fuzz campaigns, allocation guarantees, benchmarks,
and ITCH remain later work; no speed or correctness-proof claims are made.
