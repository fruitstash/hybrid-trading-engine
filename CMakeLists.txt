cmake_minimum_required(VERSION 3.20)
project(TradingEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies come from vcpkg.json (manifest mode); configure with the vcpkg toolchain file.
find_package(cppzmq CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

enable_testing()

# ---- libraries ----
add_library(order_book STATIC src/order_book.cpp)
target_include_directories(order_book PUBLIC src)

# config loading, HMAC and parameter validation (no networking)
add_library(risk STATIC src/risk_config.cpp src/hmac.cpp src/param_validator.cpp)
target_include_directories(risk PUBLIC src)
target_link_libraries(risk PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto)

add_library(ipc STATIC src/ipc_listener.cpp)
target_link_libraries(ipc PUBLIC risk PRIVATE cppzmq)

# ---- executables ----
add_executable(trading_engine src/main.cpp)
target_link_libraries(trading_engine PRIVATE order_book)

add_executable(trading_engine_live src/live_main.cpp)
target_link_libraries(trading_engine_live PRIVATE order_book ipc)

# ---- tests ----
add_executable(order_book_tests tests/order_book_tests.cpp)
target_link_libraries(order_book_tests PRIVATE order_book)
add_test(NAME order_book_tests COMMAND order_book_tests)

add_executable(ring_buffer_tests tests/ring_buffer_tests.cpp)
target_include_directories(ring_buffer_tests PRIVATE src)
add_test(NAME ring_buffer_tests COMMAND ring_buffer_tests)

add_executable(matching_engine_tests tests/matching_engine_tests.cpp)
target_link_libraries(matching_engine_tests PRIVATE order_book)
add_test(NAME matching_engine_tests COMMAND matching_engine_tests)

add_executable(param_store_tests tests/param_store_tests.cpp)
target_include_directories(param_store_tests PRIVATE src)
add_test(NAME param_store_tests COMMAND param_store_tests)

add_executable(validator_tests tests/validator_tests.cpp)
target_link_libraries(validator_tests PRIVATE risk nlohmann_json::nlohmann_json)
target_compile_definitions(validator_tests PRIVATE HTE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME validator_tests COMMAND validator_tests)
