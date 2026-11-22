cmake_minimum_required(VERSION 3.25)
project(orderbook VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

if(MINGW)
  add_link_options(-static-libgcc -static-libstdc++)
endif()

add_library(orderbook_core STATIC
  src/array_book.cpp
  src/book.cpp
  src/pool_book.cpp
  src/trace.cpp
)
target_include_directories(orderbook_core PUBLIC include)

add_executable(orderbook src/main.cpp)
target_link_libraries(orderbook PRIVATE orderbook_core)

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE third_party/doctest)
add_library(doctest::doctest ALIAS doctest)

enable_testing()
add_executable(orderbook_tests tests/test_book.cpp)
target_link_libraries(orderbook_tests PRIVATE orderbook_core doctest::doctest)
add_test(NAME book COMMAND orderbook_tests)

add_executable(orderbook_pool_tests tests/test_pool_book.cpp)
target_link_libraries(orderbook_pool_tests PRIVATE orderbook_core doctest::doctest)
add_test(NAME pool COMMAND orderbook_pool_tests)

add_test(NAME cli_zero_frames
  COMMAND orderbook depth --events 10 --seed 1 --frames 0
          --out ${CMAKE_CURRENT_BINARY_DIR}/zero_frames.csv)
set_tests_properties(cli_zero_frames PROPERTIES
  PASS_REGULAR_EXPRESSION "--frames must be at least 1")

add_test(NAME cli_zero_runs
  COMMAND orderbook bench --events 10 --seed 1 --runs 0 --impl all)
set_tests_properties(cli_zero_runs PROPERTIES
  PASS_REGULAR_EXPRESSION "--runs must be at least 1")
