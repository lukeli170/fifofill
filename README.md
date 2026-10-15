# FifoFill

A deterministic, single-symbol, single-threaded C++20 matching engine. The
reference implementation uses sorted price maps and FIFO lists; it is the
answer key for the later optimized engine. Project rules live in
[docs/SEMANTICS.md](docs/SEMANTICS.md), with the roadmap in
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md). Steps 01–05 are implemented;
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
