cmake_minimum_required(VERSION 3.25)
project(orderbook VERSION 1.0.0
  DESCRIPTION "Limit order book and matching engine in C++23"
  HOMEPAGE_URL "https://github.com/IkerAbad/orderbook"
  LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

configure_file(
  cmake/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/orderbook/version.hpp
  @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
    -Wimplicit-fallthrough -Wcast-qual -Wwrite-strings -Wpointer-arith)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

if(MINGW)
  add_link_options(-static)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(_GLIBCXX_ASSERTIONS)
endif()

add_library(orderbook_core STATIC
  src/array_book.cpp
  src/book.cpp
  src/pool_book.cpp
  src/trace.cpp
)
target_include_directories(orderbook_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(orderbook_core PUBLIC cxx_std_23)
add_library(orderbook::orderbook_core ALIAS orderbook_core)

option(ORDERBOOK_BUILD_CLI "Build the orderbook command line tool" ON)

if(ORDERBOOK_BUILD_CLI)
  add_executable(orderbook src/main.cpp)
  target_link_libraries(orderbook PRIVATE orderbook_core)
endif()

if(BUILD_TESTING)
  add_library(doctest INTERFACE)
  target_include_directories(doctest SYSTEM INTERFACE third_party/doctest)
  add_library(doctest::doctest ALIAS doctest)

  add_executable(orderbook_tests tests/test_book.cpp)
  target_link_libraries(orderbook_tests PRIVATE orderbook_core doctest::doctest)
  add_test(NAME book COMMAND orderbook_tests)

  add_executable(orderbook_pool_tests tests/test_pool_book.cpp)
  target_link_libraries(orderbook_pool_tests PRIVATE orderbook_core doctest::doctest)
  add_test(NAME pool COMMAND orderbook_pool_tests)

  add_library(orderbook_headers OBJECT
    tests/headers/types.cpp
    tests/headers/book.cpp
    tests/headers/array_book.cpp
    tests/headers/pool_book.cpp
    tests/headers/trace.cpp
    tests/headers/version.cpp)
  target_link_libraries(orderbook_headers PRIVATE orderbook_core)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # doctest 2.4.11 includes <ciso646> for Clang, which emits a #warning
    # under C++20 and later with libstdc++ 15.
    target_compile_options(orderbook_tests PRIVATE "-Wno-#warnings")
    target_compile_options(orderbook_pool_tests PRIVATE "-Wno-#warnings")
  endif()

  set_tests_properties(book pool PROPERTIES LABELS "unit")

  if(ORDERBOOK_BUILD_CLI)
  add_test(NAME cli_zero_frames
    COMMAND orderbook depth --events 10 --seed 1 --frames 0
            --out ${CMAKE_CURRENT_BINARY_DIR}/zero_frames.csv)
  set_tests_properties(cli_zero_frames PROPERTIES
    PASS_REGULAR_EXPRESSION "--frames must be at least 1")

  add_test(NAME cli_zero_runs
    COMMAND orderbook bench --events 10 --seed 1 --runs 0 --impl all)
  set_tests_properties(cli_zero_runs PROPERTIES
    PASS_REGULAR_EXPRESSION "--runs must be at least 1")

  add_test(NAME cli_zero_repeat
    COMMAND orderbook bench --events 10 --seed 1 --runs 1 --repeat 0 --impl all)
  set_tests_properties(cli_zero_repeat PROPERTIES
    PASS_REGULAR_EXPRESSION "--repeat must be at least 1")

  add_test(NAME cli_repeat
    COMMAND orderbook bench --events 100 --seed 1 --runs 1 --repeat 2 --impl all)
  set_tests_properties(cli_repeat PROPERTIES
    PASS_REGULAR_EXPRESSION "median")

  add_test(NAME bench_map
    COMMAND orderbook bench --events 100 --seed 1 --runs 1 --impl map)
  set_tests_properties(bench_map PROPERTIES
    PASS_REGULAR_EXPRESSION "map    ")

  add_test(NAME bench_array
    COMMAND orderbook bench --events 100 --seed 1 --runs 1 --impl array)
  set_tests_properties(bench_array PROPERTIES
    PASS_REGULAR_EXPRESSION "array  ")

  add_test(NAME bench_pool
    COMMAND orderbook bench --events 100 --seed 1 --runs 1 --impl pool)
  set_tests_properties(bench_pool PROPERTIES
    PASS_REGULAR_EXPRESSION "pool   ")

  add_test(NAME bench_both
    COMMAND orderbook bench --events 100 --seed 1 --runs 1 --impl both)
  set_tests_properties(bench_both PROPERTIES
    PASS_REGULAR_EXPRESSION "array  "
    FAIL_REGULAR_EXPRESSION "pool   ")

  add_test(NAME cli_missing_value
    COMMAND orderbook bench --events)
  set_tests_properties(cli_missing_value PROPERTIES
    PASS_REGULAR_EXPRESSION "--events needs a value")

  add_test(NAME cli_unknown_option
    COMMAND orderbook bench --nope)
  set_tests_properties(cli_unknown_option PROPERTIES
    PASS_REGULAR_EXPRESSION "unknown option --nope")

  add_test(NAME cli_help
    COMMAND orderbook --help)
  set_tests_properties(cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "usage:")

  add_test(NAME cli_version
    COMMAND orderbook --version)
  set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "orderbook ${PROJECT_VERSION}")

  add_test(NAME cli_replay_sample
    COMMAND orderbook replay ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.csv)
  set_tests_properties(cli_replay_sample PROPERTIES
    PASS_REGULAR_EXPRESSION "events     6[\r\n]+trades     3[\r\n]+volume     15[\r\n]+cancels    1 / 1 found[\r\n]+resting    0 orders[\r\n]+levels     0 bid, 0 ask")

  add_test(NAME replay_array
    COMMAND orderbook replay ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.csv --impl array)
  set_tests_properties(replay_array PROPERTIES
    PASS_REGULAR_EXPRESSION "trades     3[\r\n]+volume     15")

  add_test(NAME replay_pool
    COMMAND orderbook replay ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.csv --impl pool)
  set_tests_properties(replay_pool PROPERTIES
    PASS_REGULAR_EXPRESSION "trades     3[\r\n]+volume     15")

  add_test(NAME replay_all
    COMMAND orderbook replay ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.csv --impl all)
  set_tests_properties(replay_all PROPERTIES
    PASS_REGULAR_EXPRESSION "map, array and pool agree")

  add_test(NAME cli_stats_sample
    COMMAND orderbook stats ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.csv)
  set_tests_properties(cli_stats_sample PROPERTIES
    PASS_REGULAR_EXPRESSION "events     6[\r\n]+limit      3[\r\n]+cancel     1[\r\n]+market     2[\r\n]+volume     35[\r\n]+min price  9990[\r\n]+max price  10010")

  add_test(NAME depth_all
    COMMAND orderbook depth --events 100 --seed 1 --frames 2 --impl all
            --out ${CMAKE_CURRENT_BINARY_DIR}/depth_all.csv)
  set_tests_properties(depth_all PROPERTIES
    PASS_REGULAR_EXPRESSION "map, array and pool agree")

  add_test(NAME depth_consistency
    COMMAND ${CMAKE_COMMAND}
      -DORDERBOOK=$<TARGET_FILE:orderbook>
      -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_depth.cmake)
  set_tests_properties(depth_consistency PROPERTIES LABELS "cli;bench")

  add_test(NAME cli_generate_reproducible
    COMMAND ${CMAKE_COMMAND}
      -DORDERBOOK=$<TARGET_FILE:orderbook>
      -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_generate.cmake)
  set_tests_properties(cli_generate_reproducible PROPERTIES LABELS "cli")

  add_test(NAME cli_bad_workload
    COMMAND orderbook generate --events 10 --seed 1 --workload nope
            --out ${CMAKE_CURRENT_BINARY_DIR}/bad_workload.csv)
  set_tests_properties(cli_bad_workload PROPERTIES
    PASS_REGULAR_EXPRESSION "--workload must be mixed, limits, cancels or markets")

  add_test(NAME cli_workload_limits
    COMMAND orderbook generate --events 100 --seed 1 --workload limits
            --out ${CMAKE_CURRENT_BINARY_DIR}/workload_limits.csv)
  set_tests_properties(cli_workload_limits PROPERTIES
    PASS_REGULAR_EXPRESSION "100 limit, 0 cancel, 0 market")

  add_test(NAME bench_workload_limits
    COMMAND orderbook bench --events 100 --seed 1 --runs 1 --workload limits --impl pool)
  set_tests_properties(bench_workload_limits PROPERTIES
    PASS_REGULAR_EXPRESSION "limits")

  add_test(NAME bench_trace
    COMMAND orderbook bench --trace ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/sample.csv
            --runs 1 --repeat 2 --impl all)
  set_tests_properties(bench_trace PROPERTIES
    PASS_REGULAR_EXPRESSION "trace .*sample.csv"
    LABELS "cli;bench")

  add_test(NAME cli_zero_levels
    COMMAND orderbook depth --events 10 --seed 1 --frames 1 --levels 0
            --out ${CMAKE_CURRENT_BINARY_DIR}/zero_levels.csv)
  set_tests_properties(cli_zero_levels PROPERTIES
    PASS_REGULAR_EXPRESSION "--levels must be at least 1")

  add_test(NAME depth_levels
    COMMAND ${CMAKE_COMMAND}
      -DORDERBOOK=$<TARGET_FILE:orderbook>
      -DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_depth_levels.cmake)

  set_tests_properties(
    cli_zero_frames cli_zero_runs cli_zero_repeat cli_repeat
    cli_missing_value cli_unknown_option cli_help cli_version
    cli_replay_sample replay_array replay_pool replay_all depth_all
    cli_stats_sample cli_bad_workload cli_workload_limits cli_zero_levels
    depth_levels
    PROPERTIES LABELS "cli")
  set_tests_properties(bench_map bench_array bench_pool bench_both bench_workload_limits
    PROPERTIES LABELS "cli;bench")
  endif()
endif()

option(ORDERBOOK_BUILD_FUZZERS "Build the libFuzzer targets (Clang only)" OFF)

if(ORDERBOOK_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ORDERBOOK_BUILD_FUZZERS requires Clang")
  endif()

  add_executable(fuzz_parse_csv fuzz/parse_csv_fuzz.cpp)
  target_link_libraries(fuzz_parse_csv PRIVATE orderbook_core)
  target_compile_options(fuzz_parse_csv PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_parse_csv PRIVATE -fsanitize=fuzzer)

  add_executable(fuzz_book fuzz/book_fuzz.cpp)
  target_link_libraries(fuzz_book PRIVATE orderbook_core)
  target_compile_options(fuzz_book PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_book PRIVATE -fsanitize=fuzzer)
endif()

install(TARGETS orderbook_core
  EXPORT orderbookTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(ORDERBOOK_BUILD_CLI)
  install(TARGETS orderbook
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/orderbook
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/orderbook/version.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/orderbook)

install(FILES LICENSE README.md CHANGELOG.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR})

install(EXPORT orderbookTargets
  FILE orderbookTargets.cmake
  NAMESPACE orderbook::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/orderbook)

configure_package_config_file(
  cmake/orderbookConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/orderbookConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/orderbook)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/orderbookConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/orderbookConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/orderbookConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/orderbook)

set(CPACK_PACKAGE_NAME "orderbook")
set(CPACK_PACKAGE_VENDOR "Iker Abad")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME
    "orderbook-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "ZIP;TGZ")
set(CPACK_SOURCE_GENERATOR "ZIP;TGZ")
set(CPACK_SOURCE_IGNORE_FILES
    "/build[^/]*/"
    "/install/"
    "/consumer-build/"
    "/[.]git/"
    "/[.]github/"
    "/venv/"
    "/data/"
    "__pycache__/")
include(CPack)
