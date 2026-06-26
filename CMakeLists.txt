cmake_minimum_required(VERSION 3.20)
project(kernel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(kernel
    src/knowledge_kernel.cpp
    src/assertion_log.cpp
    src/storage_engine.cpp
)

target_include_directories(kernel
    PUBLIC include
)

add_executable(kernel_demo
    src/main.cpp
)

target_link_libraries(kernel_demo
    PRIVATE kernel
)

enable_testing()

add_executable(knowledge_kernel_tests
    tests/knowledge_kernel_tests.cpp
)

add_executable(assertion_log_tests
    tests/assertion_log_tests.cpp
)

target_link_libraries(knowledge_kernel_tests
    PRIVATE kernel
)

target_link_libraries(assertion_log_tests
    PRIVATE kernel
)

add_test(
    NAME knowledge_kernel_tests
    COMMAND knowledge_kernel_tests
)

add_test(
    NAME assertion_log_tests
    COMMAND assertion_log_tests
)