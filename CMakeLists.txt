cmake_minimum_required(VERSION 3.25)
project(orderbook VERSION 0.1.0 LANGUAGES CXX)

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
  src/book.cpp
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
