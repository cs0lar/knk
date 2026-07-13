#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_manager.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/status.hpp"
#include "kernel/storage_config.hpp"
#include "kernel/storage_engine.hpp"
#include "kernel/time.hpp"

using namespace knk;

namespace {

constexpr EntityId ALICE = 1;
constexpr EntityId ACME = 100;
constexpr EntityId BETA = 200;
constexpr EntityId GAMMA = 300;
constexpr EntityId UNIVERSITY = 400;
constexpr EntityId STARTUP = 500;
constexpr PredicateId WORKS_AT = 10;

constexpr Timestamp JAN_1_2020 = 1577894012;
constexpr Timestamp JAN_1_2023 = 1672531200;
constexpr Timestamp JAN_1_2024 = 1704067200;
constexpr Timestamp JUL_1_2024 = 1719792000;
constexpr Timestamp JUL_2_2024 = 1719878400;
constexpr Timestamp JUL_3_2024 = 1719961200;
constexpr Timestamp JUL_8_2024 = 1720450412;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("knowledge_kernel_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

// Writes a valid log header followed by a bad record-size field, so the corruption
// exercises record-level validation rather than tripping header validation instead.
void write_corrupt_record_size_after_valid_header(const std::filesystem::path &path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("KNK1", 4);
    uint32_t version = 1;
    out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    uint32_t bad_record_size = 1;
    out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
}

void commit_and_get_assertion() {
    auto root = test_root("commit_and_get_assertion");
    KnowledgeKernel kernel(StorageConfig{root});

    auto id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    auto assertion = kernel.get(id);

    assert(assertion.has_value());
    assert(assertion->id == id);
    assert(assertion->subject == ALICE);
    assert(assertion->predicate == WORKS_AT);
    assert(assertion->object == ACME);
    assert(assertion->valid_from == JAN_1_2023);
    assert(assertion->valid_to == OPEN_ENDED);
    assert(assertion->observed_at == JUL_2_2024);
    assert(assertion->confidence == 0.95);
    assert(assertion->status == AssertionStatus::Active);

    cleanup(root);
}

void failed_commit_does_not_burn_id() {
    auto root = test_root("failed_commit_does_not_burn_id");

    KnowledgeKernel kernel(StorageConfig{root});
    auto log_path = StorageConfig{root}.assertion_log_path();

    std::filesystem::create_directory(log_path);

    bool failed = false;
    try {
        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);
    } catch (const std::runtime_error &) {
        failed = true;
    }

    assert(failed);
    std::filesystem::remove(log_path);

    auto other_id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    assert(other_id == 1);
    cleanup(root);
}

void get_unknown_assertion_returns_nullopt() {
    auto root = test_root("get_unknown_assertion_returns_nullopt");
    KnowledgeKernel kernel(StorageConfig{root});

    auto assertion = kernel.get(999);

    assert(!assertion.has_value());

    cleanup(root);
}

void current_assertion_is_preserved_across_kernels() {
    auto root = test_root("current_assertion_preserves_result_across_kernels");

    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);

    KnowledgeKernel other_kernel(StorageConfig{root});

    auto current = other_kernel.current(ALICE);

    assert(current.size() == 1);
    assert(current[0].object == BETA);

    cleanup(root);
}

void current_assertion_returns_open_ended_assertion() {
    auto root = test_root("current_assertion_returns_open_ended_assertion");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);

    auto current = kernel.current(ALICE);

    assert(current.size() == 1);
    assert(current[0].object == BETA);

    cleanup(root);
}

void superseded_assertion_is_excluded_from_current_queries() {
    auto root = test_root("superseded_assertion_is_excluded_from_current_queries");
    KnowledgeKernel kernel(StorageConfig{root});

    AssertionId id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    kernel.commit_superseding(ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95, id);

    auto current = kernel.current(ALICE);
    assert(current.size() == 1);
    assert(current[0].object == BETA);

    cleanup(root);
}

void failed_supersession_does_not_persist_or_burn_id() {
    auto root = test_root("failed_supersession_does_not_persist_or_burn_id");
    bool exception_thrown = false;

    KnowledgeKernel kernel(StorageConfig{root});

    try {
        kernel.commit_superseding(ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95, 1);
    } catch (std::runtime_error &err) {
        exception_thrown = true;
    }

    assert(exception_thrown);

    auto id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    assert(id == 1);

    cleanup(root);
}

void retracted_assertion_is_excluded_from_current_queries() {
    auto root = test_root("retracted_assertion_is_excluded_from_current_queries");
    KnowledgeKernel kernel(StorageConfig{root});

    AssertionId id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    AssertionId other_id =
        kernel.commit_retraction(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95, id);

    auto current = kernel.current(ALICE);

    assert(current.size() == 0);

    auto assertion = kernel.get(id);
    assert(assertion.has_value());
    assert(assertion->status == AssertionStatus::Retracted);

    assertion = kernel.get(other_id);
    assert(assertion.has_value());
    assert(assertion->status == AssertionStatus::Retraction);

    cleanup(root);
}

void recovery_preserves_superseded_state() {

    auto root = test_root("recovery_preserves_superseded_state");
    KnowledgeKernel kernel(StorageConfig{root});

    AssertionId id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    AssertionId other_id =
        kernel.commit_superseding(ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95, id);

    KnowledgeKernel other_kernel(StorageConfig{root});

    auto assertion = other_kernel.get(id);

    assert(assertion.has_value());
    assert(assertion->status == AssertionStatus::Superseded);

    assertion = other_kernel.get(other_id);

    assert(assertion.has_value());
    assert(assertion->supersedes_id == id);

    cleanup(root);
}

void recovery_preserves_retracted_state() {

    auto root = test_root("recovery_preserves_retracted_state");
    KnowledgeKernel kernel(StorageConfig{root});

    AssertionId id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    AssertionId other_id =
        kernel.commit_retraction(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95, id);

    KnowledgeKernel other_kernel(StorageConfig{root});

    auto assertion = other_kernel.get(id);

    assert(assertion.has_value());
    assert(assertion->status == AssertionStatus::Retracted);

    assertion = other_kernel.get(other_id);

    assert(assertion.has_value());
    assert(assertion->retracts_id == id);

    cleanup(root);
}

void valid_at_returns_historical_assertion() {
    auto root = test_root("valid_at_returns_historical_assertion");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);

    auto assertions = kernel.valid_at(ALICE, JAN_1_2024);

    assert(assertions.size() == 1);
    assert(assertions[0].object == ACME);

    cleanup(root);
}

void valid_at_is_preserved_across_kernels() {
    auto root = test_root("valid_at_is_preserved_across_kernels");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);

    KnowledgeKernel other_kernel(StorageConfig{root});
    auto assertions = other_kernel.valid_at(ALICE, JAN_1_2024);

    assert(assertions.size() == 1);
    assert(assertions[0].object == ACME);

    cleanup(root);
}

void valid_at_respects_exclusive_valid_to() {
    auto root = test_root("valid_at_respects_exclusive_valid_to");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.95);

    auto assertions = kernel.valid_at(ALICE, JUL_1_2024);

    assert(assertions.empty());

    cleanup(root);
}

void known_at_excludes_future_observed_fact() {
    auto root = test_root("known_at_excludes_future_observed_fact");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);

    auto assertions = kernel.known_at(ALICE, JUL_1_2024);

    assert(assertions.size() == 1);
    assert(assertions[0].object == ACME);

    cleanup(root);
}

void valid_at_known_at_respects_both_times() {
    auto root = test_root("valid_at_known_at_respects_both_times");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);

    // observed but no longer valid
    auto assertions = kernel.valid_at_known_at(ALICE, JUL_1_2024, JUL_1_2024);

    assert(assertions.empty());

    // valid but not observed yet
    assertions = kernel.valid_at_known_at(ALICE, JUL_3_2024, JUL_1_2024);

    assert(assertions.empty());

    // valid and observed
    assertions = kernel.valid_at_known_at(ALICE, JUL_3_2024, JUL_2_2024);

    assert(assertions.size() == 1);
    assert(assertions[0].object == BETA);

    cleanup(root);
}

void assertions_for_subject_returns_all_subject_assertions() {
    auto root = test_root("assertions_for_subject_returns_all_subject_assertions");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);

    auto assertions = kernel.assertions_for_subject(ALICE);

    assert(assertions.size() == 2);
    assert(assertions[0].object == ACME);
    assert(assertions[1].object == BETA);

    cleanup(root);
}

void constructor_replays_assertions_and_continues_ids() {
    auto root = test_root("constructor_replays_assertions_and_continues_ids");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        auto id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

        assert(id == 1);
    }

    KnowledgeKernel kernel(StorageConfig{root});

    auto replayed = kernel.get(1);
    assert(replayed.has_value());
    assert(replayed->object == ACME);

    auto next_id = kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_3_2024, 0.90);

    assert(next_id == 2);

    cleanup(root);
}

void replay_does_not_append_to_log() {
    auto root = test_root("replay_does_not_append_to_log");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        auto id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);
    }

    KnowledgeKernel kernel(StorageConfig{root});

    auto assertions = kernel.commit_history(ALICE, WORKS_AT);

    assert(assertions.size() == 1);

    KnowledgeKernel other_kernel(StorageConfig{root});

    assertions = other_kernel.commit_history(ALICE, WORKS_AT);

    assert(assertions.size() == 1);

    cleanup(root);
}

void conflicting_active_assertions_can_coexist() {
    auto root = test_root("conflicting_active_assertions_can_coexist");

    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, UNIVERSITY, JAN_1_2020, OPEN_ENDED, JAN_1_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, STARTUP, JAN_1_2023, OPEN_ENDED, JUL_1_2024, 0.90);

    auto assertions = kernel.current(ALICE);

    assert(assertions.size() == 2);

    for (EntityId object : {UNIVERSITY, STARTUP}) {
        auto found = std::find_if(assertions.begin(), assertions.end(),
                                  [&object](const Assertion &a) { return a.object == object; });

        assert(found != assertions.end());
    }

    cleanup(root);
}

void valid_time_timeline_only_returns_active_assertions_sorted_by_valid_from() {
    auto root = test_root("valid_time_timeline_only_returns_active_assertions_sorted_by_valid_from");

    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2020, OPEN_ENDED, JAN_1_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_1_2024, 0.90);

    // we discover that alice actually left acme in 2023...
    kernel.commit_superseding(ALICE, WORKS_AT, ACME, JAN_1_2020, JAN_1_2023, JUL_2_2024, 0.90, 1);

    // ...then we discover an omitted job
    kernel.commit(ALICE, WORKS_AT, GAMMA, JAN_1_2023, JUL_1_2024, JUL_8_2024, 0.90);

    auto assertions = kernel.valid_time_timeline(ALICE, WORKS_AT);

    assert(assertions.size() == 3);

    assert(assertions[0].object == ACME);
    assert(assertions[1].object == GAMMA);
    assert(assertions[2].object == BETA);

    cleanup(root);
}

void commit_history_returns_history_of_recorded_assertions() {
    auto root = test_root("commit_history_returns_history_of_recorded_assertions");

    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2020, OPEN_ENDED, JAN_1_2024, 0.95);

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_1_2024, 0.90);

    // we discover that alice actually left acme in 2023...
    kernel.commit_superseding(ALICE, WORKS_AT, ACME, JAN_1_2020, JAN_1_2023, JUL_2_2024, 0.90, 1);

    // ...then we discover an omitted job
    kernel.commit(ALICE, WORKS_AT, GAMMA, JAN_1_2023, JUL_1_2024, JUL_8_2024, 0.90);

    auto assertions = kernel.commit_history(ALICE, WORKS_AT);

    assert(assertions.size() == 4);
    assert(assertions[0].object == ACME);
    assert(assertions[0].status == AssertionStatus::Superseded);
    assert(assertions[1].object == BETA);
    assert(assertions[2].object == ACME);
    assert(assertions[2].status == AssertionStatus::Active);
    assert(assertions[3].object == GAMMA);

    cleanup(root);
}

void observed_time_timeline_only_return_active_assertions_sorted_by_observed_at() {
    auto root = test_root("commit_history_returns_history_of_recorded_assertions");

    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JAN_1_2024, 0.90);

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2020, OPEN_ENDED, JUL_1_2024, 0.95);

    // we discover that alice actually left acme in 2023...
    kernel.commit_superseding(ALICE, WORKS_AT, ACME, JAN_1_2020, JAN_1_2023, JUL_2_2024, 0.90, 2);

    // ...then we discover an omitted job
    kernel.commit(ALICE, WORKS_AT, GAMMA, JAN_1_2023, JUL_1_2024, JUL_8_2024, 0.90);

    auto assertions = kernel.observed_time_timeline(ALICE, WORKS_AT);

    assert(assertions.size() == 3);

    assert(assertions[0].object == BETA);
    assert(assertions[1].object == ACME);
    assert(assertions[2].object == GAMMA);

    cleanup(root);
}

void known_at_is_restored_from_persisted_observed_time_index_across_kernels() {
    auto root = test_root("known_at_is_restored_from_persisted_observed_time_index_across_kernels");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);
        kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    }

    KnowledgeKernel other_kernel(StorageConfig{root});

    auto assertions = other_kernel.known_at(ALICE, JUL_1_2024);

    assert(assertions.size() == 1);
    assert(assertions[0].object == ACME);

    assertions = other_kernel.valid_at_known_at(ALICE, JUL_3_2024, JUL_2_2024);

    assert(assertions.size() == 1);
    assert(assertions[0].object == BETA);

    cleanup(root);
}

void corrupt_observed_time_index_falls_back_to_replay_and_self_heals() {
    auto root = test_root("corrupt_observed_time_index_falls_back_to_replay_and_self_heals");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);
        kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    }

    auto index_path = StorageConfig{root}.observed_time_index_path();
    write_corrupt_record_size_after_valid_header(index_path);

    KnowledgeKernel recovered_kernel(StorageConfig{root});

    auto assertions = recovered_kernel.known_at(ALICE, JUL_1_2024);
    assert(assertions.size() == 1);
    assert(assertions[0].object == ACME);

    // the self-heal rewrite above should have replaced the corrupt file, so a further
    // reopen still recovers correctly (this time via direct restore, not fallback).
    KnowledgeKernel reopened_kernel(StorageConfig{root});

    assertions = reopened_kernel.known_at(ALICE, JUL_1_2024);
    assert(assertions.size() == 1);
    assert(assertions[0].object == ACME);

    cleanup(root);
}

void assertions_for_subject_is_restored_from_persisted_subject_index_across_kernels() {
    auto root = test_root("assertions_for_subject_is_restored_from_persisted_subject_index_across_kernels");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);
        kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    }

    KnowledgeKernel other_kernel(StorageConfig{root});

    auto assertions = other_kernel.assertions_for_subject(ALICE);

    assert(assertions.size() == 2);
    assert(assertions[0].object == ACME);
    assert(assertions[1].object == BETA);

    cleanup(root);
}

void corrupt_subject_index_falls_back_to_replay_and_self_heals() {
    auto root = test_root("corrupt_subject_index_falls_back_to_replay_and_self_heals");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);
        kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    }

    auto index_path = StorageConfig{root}.subject_index_path();
    write_corrupt_record_size_after_valid_header(index_path);

    KnowledgeKernel recovered_kernel(StorageConfig{root});

    auto assertions = recovered_kernel.assertions_for_subject(ALICE);
    assert(assertions.size() == 2);

    // the self-heal rewrite above should have replaced the corrupt file, so a further
    // reopen still recovers correctly (this time via direct restore, not fallback).
    KnowledgeKernel reopened_kernel(StorageConfig{root});

    assertions = reopened_kernel.assertions_for_subject(ALICE);
    assert(assertions.size() == 2);

    cleanup(root);
}

void corrupt_all_persisted_indexes_falls_back_to_full_replay_and_self_heals() {
    auto root = test_root("corrupt_all_persisted_indexes_falls_back_to_full_replay_and_self_heals");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);
        kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    }

    auto config = StorageConfig{root};
    for (const auto &index_path :
         {config.observed_time_index_path(), config.subject_index_path(), config.current_index_path()}) {
        write_corrupt_record_size_after_valid_header(index_path);
    }

    KnowledgeKernel recovered_kernel(StorageConfig{root});

    auto assertions = recovered_kernel.assertions_for_subject(ALICE);
    assert(assertions.size() == 2);

    auto known = recovered_kernel.known_at(ALICE, JUL_1_2024);
    assert(known.size() == 1);
    assert(known[0].object == ACME);

    auto current = recovered_kernel.current(ALICE);
    assert(current.size() == 1);
    assert(current[0].object == BETA);

    cleanup(root);
}

void current_assertion_is_restored_from_persisted_current_index_across_kernels() {
    auto root = test_root("current_assertion_is_restored_from_persisted_current_index_across_kernels");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);
        kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    }

    KnowledgeKernel other_kernel(StorageConfig{root});

    auto assertions = other_kernel.current(ALICE);

    assert(assertions.size() == 1);
    assert(assertions[0].object == BETA);

    cleanup(root);
}

void superseded_assertion_remains_excluded_from_current_after_restart() {
    auto root = test_root("superseded_assertion_remains_excluded_from_current_after_restart");

    AssertionId superseding_id = 0;
    {
        KnowledgeKernel kernel(StorageConfig{root});

        AssertionId id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_1_2024, 0.95);
        superseding_id = kernel.commit_superseding(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.95, id);
    }

    KnowledgeKernel other_kernel(StorageConfig{root});

    auto assertions = other_kernel.current(ALICE);

    assert(assertions.size() == 1);
    assert(assertions[0].id == superseding_id);
    assert(assertions[0].object == BETA);

    cleanup(root);
}

void corrupt_current_index_falls_back_to_replay_and_self_heals() {
    auto root = test_root("corrupt_current_index_falls_back_to_replay_and_self_heals");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);
        kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    }

    auto index_path = StorageConfig{root}.current_index_path();
    write_corrupt_record_size_after_valid_header(index_path);

    KnowledgeKernel recovered_kernel(StorageConfig{root});

    auto assertions = recovered_kernel.current(ALICE);
    assert(assertions.size() == 1);
    assert(assertions[0].object == BETA);

    // the self-heal rewrite above should have replaced the corrupt file, so a further
    // reopen still recovers correctly (this time via direct restore, not fallback).
    KnowledgeKernel reopened_kernel(StorageConfig{root});

    assertions = reopened_kernel.current(ALICE);
    assert(assertions.size() == 1);
    assert(assertions[0].object == BETA);

    cleanup(root);
}

void crash_between_assertion_append_and_index_append_recovers_via_checkpoint() {
    auto root = test_root("crash_between_assertion_append_and_index_append_recovers_via_checkpoint");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_1_2024, 0.95);
    }

    // Simulate a crash between the assertion-log append and the index-log appends of a second
    // commit: append the assertion directly through a raw StorageEngine, bypassing
    // KnowledgeKernel::commit entirely, so none of its index entries or checkpoint update happen.
    {
        StorageEngine storage(StorageConfig{root});
        Assertion second{2, BETA, WORKS_AT, GAMMA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90, AssertionStatus::Active};
        storage.append_assertion(second);
    }

    KnowledgeKernel recovered_kernel(StorageConfig{root});

    // Without the checkpoint mismatch forcing a full rebuild, this would incorrectly return
    // empty: the index logs never got an entry for BETA, even though assertions.log durably has
    // it and get(2) would find it.
    auto assertions = recovered_kernel.assertions_for_subject(BETA);
    assert(assertions.size() == 1);
    assert(assertions[0].id == 2);
    assert(assertions[0].object == GAMMA);

    auto current = recovered_kernel.current(BETA);
    assert(current.size() == 1);
    assert(current[0].id == 2);

    cleanup(root);
}

} // namespace

int main() {
    commit_and_get_assertion();
    failed_commit_does_not_burn_id();
    get_unknown_assertion_returns_nullopt();
    current_assertion_is_preserved_across_kernels();
    current_assertion_returns_open_ended_assertion();
    superseded_assertion_is_excluded_from_current_queries();
    failed_supersession_does_not_persist_or_burn_id();
    retracted_assertion_is_excluded_from_current_queries();
    recovery_preserves_superseded_state();
    recovery_preserves_retracted_state();
    valid_at_returns_historical_assertion();
    valid_at_is_preserved_across_kernels();
    valid_at_respects_exclusive_valid_to();
    valid_at_known_at_respects_both_times();
    known_at_excludes_future_observed_fact();
    assertions_for_subject_returns_all_subject_assertions();
    constructor_replays_assertions_and_continues_ids();
    replay_does_not_append_to_log();
    conflicting_active_assertions_can_coexist();
    valid_time_timeline_only_returns_active_assertions_sorted_by_valid_from();
    commit_history_returns_history_of_recorded_assertions();
    observed_time_timeline_only_return_active_assertions_sorted_by_observed_at();
    known_at_is_restored_from_persisted_observed_time_index_across_kernels();
    corrupt_observed_time_index_falls_back_to_replay_and_self_heals();
    assertions_for_subject_is_restored_from_persisted_subject_index_across_kernels();
    corrupt_subject_index_falls_back_to_replay_and_self_heals();
    corrupt_all_persisted_indexes_falls_back_to_full_replay_and_self_heals();
    current_assertion_is_restored_from_persisted_current_index_across_kernels();
    superseded_assertion_remains_excluded_from_current_after_restart();
    corrupt_current_index_falls_back_to_replay_and_self_heals();
    crash_between_assertion_append_and_index_append_recovers_via_checkpoint();

    std::cout << "All assertion_kernel tests passed.\n";
    return 0;
}
