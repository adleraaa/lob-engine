cmake_minimum_required(VERSION 3.20)
project(lob_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build unit tests and the differential test" ON)
option(LOB_BUILD_BENCH "Build the benchmark" ON)
option(LOB_WERROR "Treat compiler warnings as errors" OFF)
option(LOB_SANITIZE "Build everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if(MINGW)
  # Link the C++ runtime statically so the executables do not pick up an
  # incompatible libstdc++-6.dll from some other MinGW earlier on PATH.
  add_link_options(-static)
endif()

if(LOB_SANITIZE)
  # Applied globally so GoogleTest is instrumented too.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# The engine is header-only (the book is a class template over its level
# container), so the library target just carries include paths.
add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

# Warnings for our own code only (not for fetched third-party code).
add_library(lob_warnings INTERFACE)
target_compile_options(lob_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
if(LOB_WERROR)
  target_compile_options(lob_warnings INTERFACE -Werror)
endif()

# Shared by the tests and the benchmark: the synthetic order-flow generator.
add_library(lob_tools INTERFACE)
target_include_directories(lob_tools INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_link_libraries(lob_tools INTERFACE lob)

if(LOB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  add_executable(lob_tests tests/test_order_book.cpp tests/test_containers.cpp)
  target_include_directories(lob_tests PRIVATE tests)
  target_link_libraries(lob_tests PRIVATE lob lob_tools lob_warnings GTest::gtest_main)
  gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 60)

  add_executable(lob_difftest tests/difftest.cpp)
  target_include_directories(lob_difftest PRIVATE tests)
  target_link_libraries(lob_difftest PRIVATE lob lob_tools lob_warnings)
  # A quick run on every `ctest`; the long run is a separate command (README).
  add_test(NAME difftest_quick COMMAND lob_difftest --ops 20000 --seeds 2)
endif()

if(LOB_BUILD_BENCH)
  add_executable(lob_bench bench/bench.cpp)
  target_link_libraries(lob_bench PRIVATE lob lob_tools lob_warnings)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type_upper)
  string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_build_type_upper}} -std=c++20" _flags)
  target_compile_definitions(lob_bench PRIVATE LOB_BUILD_FLAGS="${_flags}")
endif()
