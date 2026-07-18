cmake_minimum_required(VERSION 3.20)
project(kernel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(kernel
    src/knowledge_kernel.cpp
    src/assertion_log.cpp
    src/storage_engine.cpp
    src/index_manager.cpp
    src/observed_time_index_log.cpp
    src/subject_index_log.cpp
    src/current_index_log.cpp
    src/checksum.cpp
    src/durability.cpp
    src/index_checkpoint.cpp
    src/snapshot_store.cpp
    src/entity_catalog_log.cpp
    src/predicate_catalog_log.cpp
    src/catalog.cpp
    src/payload_store.cpp
    src/provenance_log.cpp
)

target_include_directories(kernel
    PUBLIC include
)

add_executable(kernel_demo
    examples/knowledge_kernel_demo.cpp
)

target_link_libraries(kernel_demo
    PRIVATE kernel
)

add_executable(catalog_usage_example
    examples/catalog_usage.cpp
)

target_link_libraries(catalog_usage_example
    PRIVATE kernel
)

enable_testing()

add_executable(knowledge_kernel_tests
    tests/knowledge_kernel_tests.cpp
)

add_executable(assertion_log_tests
    tests/assertion_log_tests.cpp
)

add_executable(index_manager_tests
    tests/index_manager_tests.cpp
)

add_executable(observed_time_index_log_tests
    tests/observed_time_index_log_tests.cpp
)

add_executable(subject_index_log_tests
    tests/subject_index_log_tests.cpp
)

add_executable(current_index_log_tests
    tests/current_index_log_tests.cpp
)

add_executable(checksum_tests
    tests/checksum_tests.cpp
)

add_executable(durability_tests
    tests/durability_tests.cpp
)

add_executable(index_checkpoint_tests
    tests/index_checkpoint_tests.cpp
)

add_executable(snapshot_store_tests
    tests/snapshot_store_tests.cpp
)

add_executable(entity_catalog_log_tests
    tests/entity_catalog_log_tests.cpp
)

add_executable(predicate_catalog_log_tests
    tests/predicate_catalog_log_tests.cpp
)

add_executable(catalog_tests
    tests/catalog_tests.cpp
)

add_executable(payload_store_tests
    tests/payload_store_tests.cpp
)

add_executable(provenance_log_tests
    tests/provenance_log_tests.cpp
)

target_link_libraries(knowledge_kernel_tests
    PRIVATE kernel
)

target_link_libraries(assertion_log_tests
    PRIVATE kernel
)

target_link_libraries(index_manager_tests
    PRIVATE kernel
)

target_link_libraries(observed_time_index_log_tests
    PRIVATE kernel
)

target_link_libraries(subject_index_log_tests
    PRIVATE kernel
)

target_link_libraries(current_index_log_tests
    PRIVATE kernel
)

target_link_libraries(checksum_tests
    PRIVATE kernel
)

target_link_libraries(durability_tests
    PRIVATE kernel
)

target_link_libraries(index_checkpoint_tests
    PRIVATE kernel
)

target_link_libraries(snapshot_store_tests
    PRIVATE kernel
)

target_link_libraries(entity_catalog_log_tests
    PRIVATE kernel
)

target_link_libraries(predicate_catalog_log_tests
    PRIVATE kernel
)

target_link_libraries(catalog_tests
    PRIVATE kernel
)

target_link_libraries(payload_store_tests
    PRIVATE kernel
)

target_link_libraries(provenance_log_tests
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

add_test(
    NAME index_manager_tests
    COMMAND index_manager_tests
)

add_test(
    NAME observed_time_index_log_tests
    COMMAND observed_time_index_log_tests
)

add_test(
    NAME subject_index_log_tests
    COMMAND subject_index_log_tests
)

add_test(
    NAME current_index_log_tests
    COMMAND current_index_log_tests
)

add_test(
    NAME checksum_tests
    COMMAND checksum_tests
)

add_test(
    NAME durability_tests
    COMMAND durability_tests
)

add_test(
    NAME index_checkpoint_tests
    COMMAND index_checkpoint_tests
)

add_test(
    NAME snapshot_store_tests
    COMMAND snapshot_store_tests
)

add_test(
    NAME entity_catalog_log_tests
    COMMAND entity_catalog_log_tests
)

add_test(
    NAME predicate_catalog_log_tests
    COMMAND predicate_catalog_log_tests
)

add_test(
    NAME catalog_tests
    COMMAND catalog_tests
)

add_test(
    NAME payload_store_tests
    COMMAND payload_store_tests
)

add_test(
    NAME provenance_log_tests
    COMMAND provenance_log_tests
)
