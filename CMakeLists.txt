cmake_minimum_required(VERSION 3.20)
project(bitemporal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(bitemporal
    src/assertion_store.cpp
    src/fact_log.cpp
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

target_link_libraries(assertion_store_tests
    PRIVATE bitemporal
)

add_test(
    NAME assertion_store_tests
    COMMAND assertion_store_tests
)