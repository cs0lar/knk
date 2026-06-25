cmake_minimum_required(VERSION 3.20)
project(bitemporal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(bitemporal
    src/assertion_store.cpp
    src/fact_log.cpp
    src/storage_engine.cpp
)

target_include_directories(bitemporal
    PUBLIC include
)

add_executable(bitemporal_demo
    src/main.cpp
)

target_link_libraries(bitemporal_demo
    PRIVATE bitemporal
)

enable_testing()

add_executable(assertion_store_tests
    tests/assertion_store_tests.cpp
)

add_executable(fact_log_tests
    tests/fact_log_tests.cpp
)

target_link_libraries(assertion_store_tests
    PRIVATE bitemporal
)

target_link_libraries(fact_log_tests
    PRIVATE bitemporal
)

add_test(
    NAME assertion_store_tests
    COMMAND assertion_store_tests
)

add_test(
    NAME fact_log_tests
    COMMAND fact_log_tests
)