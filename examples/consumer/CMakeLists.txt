cmake_minimum_required(VERSION 3.25)
project(orderbook_consumer LANGUAGES CXX)

find_package(orderbook 0.2 CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE orderbook::orderbook_core)
