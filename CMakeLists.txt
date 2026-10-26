cmake_minimum_required(VERSION 3.20)
project(TradingEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

add_library(order_book STATIC src/order_book.cpp)
target_include_directories(order_book PUBLIC src)

add_executable(trading_engine src/main.cpp)
target_link_libraries(trading_engine PRIVATE order_book)

add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE order_book)
add_test(NAME order_book_tests COMMAND order_book_tests)

add_executable(ring_buffer_tests tests/ring_buffer_tests.cpp)
target_include_directories(ring_buffer_tests PRIVATE src)
add_test(NAME ring_buffer_tests COMMAND ring_buffer_tests)

add_executable(matching_engine_tests tests/matching_engine_tests.cpp)
target_link_libraries(matching_engine_tests PRIVATE order_book)
add_test(NAME matching_engine_tests COMMAND matching_engine_tests)
