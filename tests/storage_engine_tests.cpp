#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "kernel/storage_engine.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

// Helper function: Create an isolated temporary test directory
std::filesystem::path test_root(const std::string &name) {
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}

// Helper function: Generate mock assertion data
Assertion make_assertion(AssertionId id, EntityId subject) {
    return Assertion{.id = id,
                     .subject = subject,
                     .predicate = 10,
                     .object = 100,
                     .valid_from = 1672531200,
                     .valid_to = OPEN_ENDED,
                     .observed_at = 1719878400,
                     .confidence = 0.95,
                     .status = AssertionStatus::Active};
}

// Test 1: Verify basic append and read functionality
void storage_engine_appends_and_reads_assertions() {
    auto dir = test_root("kernel_se_appends_and_reads");
    
    // Use C++20 designated initializers to maintain consistency with the codebase style
    StorageConfig config{.root = dir, .max_records_per_segment = 100};
    StorageEngine engine(config);

    engine.append_assertion(make_assertion(1, 1));
    auto assertions = engine.load_assertions();

    assert(assertions.size() == 1);
    assert(assertions[0].id == 1);
    assert(assertions[0].subject == 1);

    std::filesystem::remove_all(dir);
}

// Test 2: Verify data recovery via replay after a simulated crash/restart
void storage_engine_recovers_data_on_reopen() {
    auto dir = test_root("kernel_se_recovers_data");
    StorageConfig config{.root = dir, .max_records_per_segment = 100};

    {
        // First run: Append two records
        StorageEngine engine(config);
        engine.append_assertion(make_assertion(1, 1));
        engine.append_assertion(make_assertion(2, 2));
    } // Out of scope: engine is destroyed, storage_lock_ is released

    {
        // Second run: Simulate restart using the directory with existing data
        StorageEngine engine_reopened(config);
        auto assertions = engine_reopened.load_assertions();
        
        // Verify that data was successfully recovered
        assert(assertions.size() == 2);
        assert(assertions[0].id == 1);
        assert(assertions[1].id == 2);
    }

    std::filesystem::remove_all(dir);
}

// Test 3: Verify delegation of archiving and hints to downstream logs
void storage_engine_delegates_archiving_and_hints() {
    auto dir = test_root("kernel_se_delegates_archiving");
    StorageConfig config{.root = dir, .max_records_per_segment = 2}; // Set capacity to 2 to force segment creation
    
    StorageEngine engine(config);
    engine.append_assertion(make_assertion(1, 1));
    engine.append_assertion(make_assertion(2, 2));
    engine.append_assertion(make_assertion(3, 3));

    // Verify the propagation of the record count hint
    assert(engine.assertion_log_record_count_hint() == 3);

    // Verify the delegation of the archiving function
    engine.archive_segments_before(3);
    auto assertions = engine.load_assertions_after(2);
    
    assert(assertions.size() == 1);
    assert(assertions[0].id == 3);

    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    storage_engine_appends_and_reads_assertions();
    storage_engine_recovers_data_on_reopen();
    storage_engine_delegates_archiving_and_hints();

    std::cout << "All storage_engine tests passed.\n";
    return 0;
}