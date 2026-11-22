cmake_minimum_required(VERSION 3.25)
project(orderbook VERSION 0.2.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
target_include_directories(orderbook_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(orderbook_core PUBLIC cxx_std_23)
add_library(orderbook::orderbook_core ALIAS orderbook_core)

add_executable(orderbook src/main.cpp)
target_link_libraries(orderbook PRIVATE orderbook_core)

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

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # doctest 2.4.11 includes <ciso646> for Clang, which emits a #warning
    # under C++20 and later with libstdc++ 15.
    target_compile_options(orderbook_tests PRIVATE "-Wno-#warnings")
    target_compile_options(orderbook_pool_tests PRIVATE "-Wno-#warnings")
  endif()

  add_test(NAME cli_zero_frames
    COMMAND orderbook depth --events 10 --seed 1 --frames 0
            --out ${CMAKE_CURRENT_BINARY_DIR}/zero_frames.csv)
  set_tests_properties(cli_zero_frames PROPERTIES
    PASS_REGULAR_EXPRESSION "--frames must be at least 1")

  add_test(NAME cli_zero_runs
    COMMAND orderbook bench --events 10 --seed 1 --runs 0 --impl all)
  set_tests_properties(cli_zero_runs PROPERTIES
    PASS_REGULAR_EXPRESSION "--runs must be at least 1")
endif()

install(TARGETS orderbook_core
  EXPORT orderbookTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS orderbook
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/orderbook
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES LICENSE README.md
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
