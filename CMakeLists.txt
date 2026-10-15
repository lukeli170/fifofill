cmake_minimum_required(VERSION 3.24)
project(FifoFill VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FIFOFILL_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(FIFOFILL_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Sanitizers require Clang or GCC")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
endif()

add_library(fifofill src/config.cpp reference/reference_book.cpp)
target_include_directories(fifofill PUBLIC include reference)
target_compile_features(fifofill PUBLIC cxx_std_20)
target_compile_options(fifofill PRIVATE
  $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  # GoogleTest 1.17.0, pinned by commit and archive SHA-256.
  FetchContent_Declare(googletest
    URL https://codeload.github.com/google/googletest/tar.gz/52eb8108c5bdec04579160ae17225d66034bd723
    URL_HASH SHA256=745c55415660044610f7fcd3af7a6420d5de16a7dbb9ebfe2e131275676232be
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)

  add_executable(fifofill_tests tests/build_tests.cpp tests/value_tests.cpp tests/reference_tests.cpp)
  target_link_libraries(fifofill_tests PRIVATE fifofill GTest::gtest_main)
  target_compile_options(fifofill_tests PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
  include(GoogleTest)
  gtest_discover_tests(fifofill_tests)
endif()
