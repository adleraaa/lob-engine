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

set(LOB_LINK_FLAGS "")
if(MINGW)
  # Link the C++ runtime statically so the executables do not pick up an
  # incompatible libstdc++-6.dll from some other MinGW earlier on PATH.
  add_link_options(-static)
  set(LOB_LINK_FLAGS "-static")
endif()

# Source revision recorded in the results files. Read at configure time; the
# configure step re-runs automatically when HEAD, the current branch or the
# git index changes. "-dirty" marks uncommitted changes to tracked files.
set(LOB_GIT_SHA "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE LOB_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _git_status OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_git_status)
    string(APPEND LOB_GIT_SHA "-dirty")
  endif()
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD" _git_head)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/index")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.git/index")
  endif()
  if(_git_head MATCHES "^ref: (.+)")
    string(STRIP "${CMAKE_MATCH_1}" _git_ref)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/${_git_ref}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/.git/${_git_ref}")
    endif()
  endif()
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

# Shared by the tests and the benchmark: the synthetic order-flow generator,
# CLI helpers and the build-provenance macros that go into results files.
add_library(lob_tools INTERFACE)
target_include_directories(lob_tools INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_link_libraries(lob_tools INTERFACE lob)
string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type_upper)
string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_build_type_upper}} -std=c++20" _flags)
target_compile_definitions(lob_tools INTERFACE
  LOB_GIT_SHA="${LOB_GIT_SHA}"
  LOB_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  LOB_BUILD_FLAGS="${_flags}"
  LOB_LINK_FLAGS="${LOB_LINK_FLAGS}")

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
  # A correctness gate must not turn a typo into "0 requests, OK".
  add_test(NAME difftest_rejects_bad_number COMMAND lob_difftest --ops abc)
  add_test(NAME difftest_rejects_missing_value COMMAND lob_difftest --ops)
  set_tests_properties(difftest_rejects_bad_number difftest_rejects_missing_value
    PROPERTIES PASS_REGULAR_EXPRESSION "bad value|missing value")
endif()

if(LOB_BUILD_BENCH)
  add_executable(lob_bench bench/bench.cpp)
  target_link_libraries(lob_bench PRIVATE lob lob_tools lob_warnings)
endif()
