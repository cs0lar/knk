#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
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
constexpr PredicateId LIVES_IN = 20;

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

void commit_by_name_interns_names_and_commits() {
    auto root = test_root("commit_by_name_interns_names_and_commits");
    KnowledgeKernel kernel(StorageConfig{root});

    auto id =
        kernel.commit_by_name("Alice", "works_at", Value::of_text("Acme"), JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    auto assertion = kernel.get(id);
    assert(assertion.has_value());

    auto alice_id = kernel.find_entity("Alice");
    auto works_at_id = kernel.find_predicate("works_at");
    auto acme_id = kernel.find_entity("Acme");

    assert(alice_id.has_value() && assertion->subject == *alice_id);
    assert(works_at_id.has_value() && assertion->predicate == *works_at_id);
    assert(acme_id.has_value() && assertion->object == *acme_id);
    assert(assertion->status == AssertionStatus::Active);

    cleanup(root);
}

void commit_by_name_reuses_ids_for_repeated_names() {
    auto root = test_root("commit_by_name_reuses_ids_for_repeated_names");
    KnowledgeKernel kernel(StorageConfig{root});

    auto first =
        kernel.commit_by_name("Alice", "works_at", Value::of_text("Acme"), JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);
    auto second =
        kernel.commit_by_name("Alice", "works_at", Value::of_text("Beta"), JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);

    auto first_assertion = kernel.get(first);
    auto second_assertion = kernel.get(second);

    assert(first_assertion.has_value() && second_assertion.has_value());
    assert(first_assertion->subject == second_assertion->subject);
    assert(first_assertion->predicate == second_assertion->predicate);
    assert(first_assertion->object != second_assertion->object);

    cleanup(root);
}

void commit_by_name_supports_a_literal_object() {
    auto root = test_root("commit_by_name_supports_a_literal_object");
    KnowledgeKernel kernel(StorageConfig{root});

    auto id = kernel.commit_by_name("Alice", "age", Value::of_int64(30), JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    auto assertion = kernel.get(id);
    assert(assertion.has_value());

    auto object_value = kernel.entity_value(assertion->object);
    assert(object_value.has_value());
    assert(object_value->kind == ValueKind::Int64);
    assert(object_value->int64_value == 30);

    cleanup(root);
}

void failed_commit_does_not_burn_id() {
    auto root = test_root("failed_commit_does_not_burn_id");

    KnowledgeKernel kernel(StorageConfig{root});
    // Assertion id 1 always lands in segment index 0, regardless of max_records_per_segment. The
    // segments/ directory doesn't exist yet (no commit has created it), so create_directories (not
    // create_directory) is needed to create both it and the sabotage directory at the segment path.
    auto log_path = StorageConfig{root}.segment_path(0);

    std::filesystem::create_directories(log_path);

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

void changes_since_returns_assertions_observed_at_or_after_cutoff_sorted_by_observed_at() {
    auto root = test_root("changes_since_returns_assertions_observed_at_or_after_cutoff_sorted_by_observed_at");

    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2020, OPEN_ENDED, JAN_1_2024, 0.95);
    AssertionId at_cutoff = kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_1_2024, 0.90);
    AssertionId after_cutoff = kernel.commit(ALICE, WORKS_AT, GAMMA, JUL_8_2024, OPEN_ENDED, JUL_8_2024, 0.90);

    auto changes = kernel.changes_since(JUL_1_2024);

    assert(changes.size() == 2);
    assert(changes[0].id == at_cutoff);
    assert(changes[1].id == after_cutoff);

    cleanup(root);
}

void changes_since_is_status_agnostic_and_spans_multiple_subjects() {
    auto root = test_root("changes_since_is_status_agnostic_and_spans_multiple_subjects");

    KnowledgeKernel kernel(StorageConfig{root});

    constexpr Timestamp SUPERSEDING_AT = JUL_1_2024 + 2 * 86400;
    constexpr Timestamp HYPOTHESIS_AT = JUL_8_2024 + 1;

    // Excluded below: observed before the cutoff, even though its status later changes.
    AssertionId original = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2020, OPEN_ENDED, JAN_1_2024, 0.95);

    AssertionId other_subject = kernel.commit(UNIVERSITY, LIVES_IN, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);

    AssertionId superseding =
        kernel.commit_superseding(ALICE, WORKS_AT, BETA, JUL_8_2024, OPEN_ENDED, SUPERSEDING_AT, 0.90, original);

    AssertionId retraction = kernel.commit_retraction(UNIVERSITY, LIVES_IN, BETA, JAN_1_2023, OPEN_ENDED,
                                                       JUL_8_2024, 0.90, other_subject);

    AssertionId hypothesis = kernel.commit_hypothesis(ALICE, WORKS_AT, GAMMA, JAN_1_2020, OPEN_ENDED, HYPOTHESIS_AT,
                                                       0.5, STARTUP, HYPOTHESIS_AT, "churn_model");

    auto changes = kernel.changes_since(JUL_2_2024);

    assert(changes.size() == 4);

    assert(changes[0].id == other_subject);
    assert(changes[0].status == AssertionStatus::Retracted);
    assert(changes[1].id == superseding);
    assert(changes[2].id == retraction);
    assert(changes[3].id == hypothesis);
    assert(changes[3].status == AssertionStatus::Hypothesis);

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

void write_snapshot_then_restart_uses_snapshot_and_replays_only_the_tail() {
    auto root = test_root("write_snapshot_then_restart_uses_snapshot_and_replays_only_the_tail");

    AssertionId first_id = 0;
    AssertionId superseding_id = 0;
    {
        KnowledgeKernel kernel(StorageConfig{root});

        first_id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_1_2024, 0.95);
        kernel.commit(BETA, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_1_2024, 0.90);

        kernel.write_snapshot();

        // Committed after the snapshot, and supersedes an assertion the snapshot already covers --
        // exercises restore_assertion() mutating the status of a snapshot-seeded assertion.
        superseding_id =
            kernel.commit_superseding(ALICE, WORKS_AT, UNIVERSITY, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.95, first_id);
    }

    KnowledgeKernel reopened(StorageConfig{root});

    auto alice_current = reopened.current(ALICE);
    assert(alice_current.size() == 1);
    assert(alice_current[0].id == superseding_id);
    assert(alice_current[0].object == UNIVERSITY);

    auto beta_current = reopened.current(BETA);
    assert(beta_current.size() == 1);
    assert(beta_current[0].object == GAMMA);

    auto history = reopened.commit_history(ALICE, WORKS_AT);
    assert(history.size() == 2);
    assert(history[0].id == first_id && history[0].status == AssertionStatus::Superseded);
    assert(history[1].id == superseding_id && history[1].status == AssertionStatus::Active);

    cleanup(root);
}

void stale_or_corrupt_snapshot_falls_back_to_full_replay() {
    auto root = test_root("stale_or_corrupt_snapshot_falls_back_to_full_replay");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_1_2024, 0.95);
        kernel.commit(ALICE, WORKS_AT, BETA, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    }

    {
        std::ofstream out(StorageConfig{root}.snapshot_path(), std::ios::binary | std::ios::trunc);
        out.write("garbage!", 8);
    }

    KnowledgeKernel recovered_kernel(StorageConfig{root});

    auto assertions = recovered_kernel.assertions_for_subject(ALICE);
    assert(assertions.size() == 2);

    auto current = recovered_kernel.current(ALICE);
    assert(current.size() == 1);
    assert(current[0].object == BETA);

    cleanup(root);
}

void snapshot_ahead_of_log_is_ignored() {
    auto root = test_root("snapshot_ahead_of_log_is_ignored");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_1_2024, 0.95);
    }

    // Write a snapshot claiming to cover 5 assertions when the log only ever had 1 -- this must be
    // ignored rather than trusted (record_count_hint() catches it), or startup would silently seed
    // assertions_ with 4 assertions that never existed.
    {
        StorageEngine storage(StorageConfig{root});
        std::vector<Assertion> bogus_assertions;
        for (AssertionId id = 1; id <= 5; ++id) {
            bogus_assertions.push_back(Assertion{id, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_1_2024, 0.95,
                                                 AssertionStatus::Active});
        }
        storage.write_snapshot(5, bogus_assertions);
    }

    KnowledgeKernel recovered_kernel(StorageConfig{root});

    auto assertions = recovered_kernel.assertions_for_subject(ALICE);
    assert(assertions.size() == 1);
    assert(assertions[0].id == 1);

    cleanup(root);
}

void intern_entity_is_idempotent_and_returns_the_same_id_for_the_same_name() {
    auto root = test_root("intern_entity_is_idempotent_and_returns_the_same_id_for_the_same_name");
    KnowledgeKernel kernel(StorageConfig{root});

    auto alice_id = kernel.intern_entity("Alice");
    auto alice_id_again = kernel.intern_entity("Alice");
    auto bob_id = kernel.intern_entity("Bob");

    assert(alice_id == alice_id_again);
    assert(alice_id != bob_id);

    cleanup(root);
}

void intern_value_is_idempotent_for_numeric_and_text_values() {
    auto root = test_root("intern_value_is_idempotent_for_numeric_and_text_values");
    KnowledgeKernel kernel(StorageConfig{root});

    auto forty_two_id = kernel.intern_value(Value::of_int64(42));
    auto forty_two_id_again = kernel.intern_value(Value::of_int64(42));

    assert(forty_two_id == forty_two_id_again);

    // A Value::of_text("42") and a Value::of_int64(42) share no meaningful content-equality --
    // they are different kinds, so must resolve to different ids.
    auto forty_two_text_id = kernel.intern_value(Value::of_text("42"));
    assert(forty_two_text_id != forty_two_id);

    cleanup(root);
}

void find_entity_returns_nullopt_for_an_unknown_name() {
    auto root = test_root("find_entity_returns_nullopt_for_an_unknown_name");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.intern_entity("Alice");

    assert(!kernel.find_entity("Bob").has_value());

    cleanup(root);
}

void entity_name_resolves_a_previously_interned_name() {
    auto root = test_root("entity_name_resolves_a_previously_interned_name");
    KnowledgeKernel kernel(StorageConfig{root});

    auto alice_id = kernel.intern_entity("Alice");

    auto name = kernel.entity_name(alice_id);
    assert(name.has_value());
    assert(*name == "Alice");

    // A non-Text value has no name to resolve.
    auto number_id = kernel.intern_value(Value::of_int64(42));
    assert(!kernel.entity_name(number_id).has_value());

    cleanup(root);
}

void predicate_name_resolves_a_previously_interned_predicate() {
    auto root = test_root("predicate_name_resolves_a_previously_interned_predicate");
    KnowledgeKernel kernel(StorageConfig{root});

    auto works_at_id = kernel.intern_predicate("works_at");

    auto name = kernel.predicate_name(works_at_id);
    assert(name.has_value());
    assert(*name == "works_at");

    assert(kernel.find_predicate("works_at") == std::optional<PredicateId>(works_at_id));
    assert(!kernel.find_predicate("lives_in").has_value());

    cleanup(root);
}

void catalog_is_preserved_across_kernel_restarts() {
    auto root = test_root("catalog_is_preserved_across_kernel_restarts");

    EntityId alice_id;
    PredicateId works_at_id;

    {
        KnowledgeKernel kernel(StorageConfig{root});
        alice_id = kernel.intern_entity("Alice");
        works_at_id = kernel.intern_predicate("works_at");
    }

    KnowledgeKernel reopened_kernel(StorageConfig{root});

    // Interning an already-seen name after restart must return the same id as before -- the
    // catalog is authoritative, not rebuilt from assertions.log, so this is the regression test
    // that would catch a replay bug silently minting a second id for the same name.
    assert(reopened_kernel.intern_entity("Alice") == alice_id);
    assert(reopened_kernel.intern_predicate("works_at") == works_at_id);

    auto name = reopened_kernel.entity_name(alice_id);
    assert(name.has_value());
    assert(*name == "Alice");

    auto predicate_name = reopened_kernel.predicate_name(works_at_id);
    assert(predicate_name.has_value());
    assert(*predicate_name == "works_at");

    cleanup(root);
}

void corrupt_entity_catalog_is_fatal_on_startup() {
    auto root = test_root("corrupt_entity_catalog_is_fatal_on_startup");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.intern_entity("Alice");
    }

    auto catalog_path = StorageConfig{root}.entity_catalog_path();
    write_corrupt_record_size_after_valid_header(catalog_path);

    bool threw = false;
    try {
        KnowledgeKernel recovered_kernel(StorageConfig{root});
    } catch (const std::runtime_error &) {
        threw = true;
    }

    // Unlike the Phase 3 indexes, there is no self-heal fallback -- the catalog is its own source
    // of truth, so non-tail corruption must surface as a fatal, uncaught startup error.
    assert(threw);

    cleanup(root);
}

void corrupt_predicate_catalog_is_fatal_on_startup() {
    auto root = test_root("corrupt_predicate_catalog_is_fatal_on_startup");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.intern_predicate("works_at");
    }

    auto catalog_path = StorageConfig{root}.predicate_catalog_path();
    write_corrupt_record_size_after_valid_header(catalog_path);

    bool threw = false;
    try {
        KnowledgeKernel recovered_kernel(StorageConfig{root});
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    cleanup(root);
}

std::vector<std::byte> make_content(const std::string &text) {
    std::vector<std::byte> content(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        content[i] = static_cast<std::byte>(text[i]);
    }
    return content;
}

void payload_round_trips_large_content() {
    auto root = test_root("payload_round_trips_large_content");
    KnowledgeKernel kernel(StorageConfig{root});

    std::string large(1'000'000, 'x');
    auto content = make_content(large);

    auto id = kernel.intern_document(content);

    auto loaded = kernel.document_content(id);
    assert(loaded.has_value());
    assert(*loaded == content);

    cleanup(root);
}

void intern_document_mints_ids_from_the_shared_entity_id_space() {
    auto root = test_root("intern_document_mints_ids_from_the_shared_entity_id_space");
    KnowledgeKernel kernel(StorageConfig{root});

    auto alice_id = kernel.intern_entity("Alice");
    auto document_id = kernel.intern_document(make_content("a document"));
    auto bob_id = kernel.intern_entity("Bob");

    assert(document_id != alice_id);
    assert(document_id != bob_id);

    cleanup(root);
}

void payload_is_preserved_across_kernel_restarts() {
    auto root = test_root("payload_is_preserved_across_kernel_restarts");

    EntityId document_id;
    auto content = make_content("preserved across restarts");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        document_id = kernel.intern_document(content);
    }

    KnowledgeKernel reopened_kernel(StorageConfig{root});

    auto loaded = reopened_kernel.document_content(document_id);
    assert(loaded.has_value());
    assert(*loaded == content);

    // The document id must not be reused by a later interning call after restart -- id-space
    // continuity is restored from PayloadStore, not just entities.log.
    auto next_id = reopened_kernel.intern_entity("Alice");
    assert(next_id != document_id);

    cleanup(root);
}

void corrupt_payload_is_fatal_on_startup() {
    auto root = test_root("corrupt_payload_is_fatal_on_startup");

    EntityId document_id;
    {
        KnowledgeKernel kernel(StorageConfig{root});
        document_id = kernel.intern_document(make_content("valid"));
    }

    auto payload_path = StorageConfig{root}.payload_path(document_id);
    {
        std::fstream io(payload_path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(0);
        io.write("XXXX", 4);
    }

    bool threw = false;
    try {
        KnowledgeKernel recovered_kernel(StorageConfig{root});
    } catch (const std::runtime_error &) {
        threw = true;
    }

    // Like the catalog logs, PayloadStore is authoritative with nothing to rebuild it from, so
    // corruption must surface as a fatal, uncaught startup error rather than a self-heal.
    assert(threw);

    cleanup(root);
}

void explain_walks_the_supersession_chain_to_its_root() {
    auto root = test_root("explain_walks_the_supersession_chain_to_its_root");
    KnowledgeKernel kernel(StorageConfig{root});

    auto id1 = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    auto id2 = kernel.commit_superseding(ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.92, id1);
    auto id3 = kernel.commit_superseding(ALICE, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_3_2024, 0.95, id2);

    auto chain = kernel.explain(id3);

    // Newest-first: the queried assertion, then the one it superseded, down to the root.
    assert(chain.size() == 3);
    assert(chain[0].id == id3);
    assert(chain[0].object == GAMMA);
    assert(chain[1].id == id2);
    assert(chain[1].object == BETA);
    assert(chain[2].id == id1);
    assert(chain[2].object == ACME);

    // A root assertion links no further -- its chain is just itself.
    auto root_chain = kernel.explain(id1);
    assert(root_chain.size() == 1);
    assert(root_chain[0].id == id1);

    // A retraction record links back to the assertion it retracted.
    auto retraction_id =
        kernel.commit_retraction(ALICE, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_8_2024, 0.95, id3);
    auto retraction_chain = kernel.explain(retraction_id);
    assert(retraction_chain.size() == 4);
    assert(retraction_chain[0].id == retraction_id);
    assert(retraction_chain[1].id == id3);
    assert(retraction_chain[3].id == id1);

    // An unknown id explains to nothing.
    assert(kernel.explain(999).empty());
    assert(kernel.explain(0).empty());
    
    cleanup(root);
}

void provenance_is_recorded_and_resolves_to_a_source_entity() {
    auto root = test_root("provenance_is_recorded_and_resolves_to_a_source_entity");
    KnowledgeKernel kernel(StorageConfig{root});

    // A source is just an interned entity, resolved back through entity_name for free.
    auto source = kernel.intern_entity("ingestion_pipeline");
    auto assertion_id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    kernel.record_provenance(assertion_id, source, JUL_2_2024, "manual_entry");

    auto record = kernel.provenance_for(assertion_id);
    assert(record.has_value());
    assert(record->assertion_id == assertion_id);
    assert(record->source == source);
    assert(record->recorded_at == JUL_2_2024);
    assert(record->method == "manual_entry");

    auto source_name = kernel.entity_name(record->source);
    assert(source_name.has_value());
    assert(*source_name == "ingestion_pipeline");

    // An assertion with no recorded provenance resolves to nullopt.
    auto other_id = kernel.commit(ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
    assert(!kernel.provenance_for(other_id).has_value());
  
    cleanup(root);
}

void find_conflicts_detects_overlapping_active_assertions_for_the_same_subject_predicate() {
    auto root = test_root("find_conflicts_detects_overlapping_active_assertions_for_the_same_subject_predicate");
    KnowledgeKernel kernel(StorageConfig{root});

    // Two open-ended active assertions with different objects for the same subject/predicate overlap
    // in valid time -> a conflict.
    auto acme_id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.80);
    auto beta_id = kernel.commit(ALICE, WORKS_AT, BETA, JAN_1_2024, OPEN_ENDED, JUL_2_2024, 0.80);

    auto conflicts = kernel.find_conflicts(ALICE, WORKS_AT);
    assert(conflicts.size() == 1);
    // Reported in subject-index (commit) order: the earlier assertion first.
    assert(conflicts[0].first.id == acme_id);
    assert(conflicts[0].second.id == beta_id);

    // A third assertion with the SAME object as an existing one is not a conflict with it, even
    // though it overlaps -- same claim, not a contradiction.
    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_3_2024, 0.85);
    conflicts = kernel.find_conflicts(ALICE, WORKS_AT);
    // acme/beta, and the new acme conflicts with beta too (different object, overlapping) -- but the
    // two ACME assertions do not conflict with each other.
    assert(conflicts.size() == 2);
    
    cleanup(root);
}
    
void record_provenance_rejects_an_unknown_assertion_target() {
    auto root = test_root("record_provenance_rejects_an_unknown_assertion_target");
    KnowledgeKernel kernel(StorageConfig{root});

    auto source = kernel.intern_entity("ingestion_pipeline");

    bool threw = false;
    try {
        kernel.record_provenance(999, source, JUL_2_2024, "manual_entry");
    } catch (const std::runtime_error &) {
        threw = true;
    }

    // A dangling provenance target must not persist a record; the validation mirrors how
    // commit_retraction validates its target before appending.
    assert(threw);
    assert(!kernel.provenance_for(999).has_value());

    cleanup(root);
}

void find_conflicts_excludes_non_overlapping_and_resolved_assertions() {
    auto root = test_root("find_conflicts_excludes_non_overlapping_and_resolved_assertions");
    KnowledgeKernel kernel(StorageConfig{root});

    // Adjacent, non-overlapping valid intervals ([JAN_1_2023, JAN_1_2024) then [JAN_1_2024, ...)) --
    // half-open, so touching at JAN_1_2024 is not an overlap.
    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JAN_1_2024, JUL_2_2024, 0.80);
    kernel.commit(ALICE, WORKS_AT, BETA, JAN_1_2024, OPEN_ENDED, JUL_2_2024, 0.80);
    assert(kernel.find_conflicts(ALICE, WORKS_AT).empty());

    // A superseded assertion is already resolved and is never a conflict.
    auto original = kernel.commit(BETA, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.80);
    kernel.commit_superseding(BETA, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_3_2024, 0.90, original);
    assert(kernel.find_conflicts(BETA, WORKS_AT).empty());

    // No assertions at all for a subject -> no conflicts.
    assert(kernel.find_conflicts(UNIVERSITY, WORKS_AT).empty());
    
    cleanup(root);
}

void provenance_is_preserved_across_kernel_restarts() {
    auto root = test_root("provenance_is_preserved_across_kernel_restarts");

    AssertionId assertion_id;
    EntityId source;

    {
        KnowledgeKernel kernel(StorageConfig{root});
        source = kernel.intern_entity("ingestion_pipeline");
        assertion_id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);
        kernel.record_provenance(assertion_id, source, JUL_2_2024, "manual_entry");
    }

    KnowledgeKernel reopened_kernel(StorageConfig{root});

    auto record = reopened_kernel.provenance_for(assertion_id);
    assert(record.has_value());
    assert(record->source == source);
    assert(record->recorded_at == JUL_2_2024);
    assert(record->method == "manual_entry");

    cleanup(root);
}

void corrupt_provenance_log_is_fatal_on_startup() {
    auto root = test_root("corrupt_provenance_log_is_fatal_on_startup");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        auto source = kernel.intern_entity("ingestion_pipeline");
        auto assertion_id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);
        kernel.record_provenance(assertion_id, source, JUL_2_2024, "manual_entry");
    }

    auto provenance_path = StorageConfig{root}.provenance_log_path();
    write_corrupt_record_size_after_valid_header(provenance_path);

    bool threw = false;
    try {
        KnowledgeKernel recovered_kernel(StorageConfig{root});
    } catch (const std::runtime_error &) {
        threw = true;
    }

    // Provenance is authoritative like the catalog logs -- assertions.log encodes nothing about it,
    // so non-tail corruption must surface as a fatal, uncaught startup error, not a self-heal.
    assert(threw);

    cleanup(root);
}

void commit_hypothesis_is_excluded_from_current_and_valid_at() {
    auto root = test_root("commit_hypothesis_is_excluded_from_current_and_valid_at");
    KnowledgeKernel kernel(StorageConfig{root});

    auto source = kernel.intern_entity("predictor_model");
    auto hypothesis_id = kernel.commit_hypothesis(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6,
                                                  source, JUL_2_2024, "predicted_by_model");

    // A real, Active fact for the same subject so current()/valid_at() have something to return --
    // the hypothesis must not appear alongside it.
    kernel.commit(ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    for (const auto &assertion : kernel.current(ALICE)) {
        assert(assertion.id != hypothesis_id);
    }

    for (const auto &assertion : kernel.valid_at(ALICE, JUL_2_2024)) {
        assert(assertion.id != hypothesis_id);
    }

    for (const auto &assertion : kernel.known_at(ALICE, JUL_2_2024)) {
        assert(assertion.id != hypothesis_id);
    }

    for (const auto &assertion : kernel.valid_at_known_at(ALICE, JUL_2_2024, JUL_2_2024)) {
        assert(assertion.id != hypothesis_id);
    }

    // Still visible off the status-agnostic query paths.
    auto stored = kernel.get(hypothesis_id);
    assert(stored.has_value());
    assert(stored->status == AssertionStatus::Hypothesis);

    cleanup(root);
}

void current_by_name_resolves_the_named_subject_and_returns_current_assertions() {
    auto root = test_root("current_by_name_resolves_the_named_subject_and_returns_current_assertions");
    KnowledgeKernel kernel(StorageConfig{root});

    auto id =
        kernel.commit_by_name("Alice", "works_at", Value::of_text("Acme"), JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    auto by_name = kernel.current_by_name("Alice");

    assert(by_name.size() == 1);
    assert(by_name[0].id == id);

    // Agrees with the id-based lookup for the same (interned) subject.
    auto alice_id = kernel.find_entity("Alice");
    assert(alice_id.has_value());
    auto by_id = kernel.current(*alice_id);
    assert(by_id.size() == 1);
    assert(by_id[0].id == by_name[0].id);

    cleanup(root);
}

void current_by_name_returns_empty_for_an_unknown_name() {
    auto root = test_root("current_by_name_returns_empty_for_an_unknown_name");
    KnowledgeKernel kernel(StorageConfig{root});

    // "Alice" was never interned -- unlike commit_by_name, a read-only lookup must not spuriously mint
    // a new entity for a name that was never committed.
    assert(kernel.current_by_name("Alice").empty());
    assert(!kernel.find_entity("Alice").has_value());

    cleanup(root);
}

void current_by_object_returns_active_open_ended_assertions_referencing_the_entity() {
    auto root = test_root("current_by_object_returns_active_open_ended_assertions_referencing_the_entity");

    KnowledgeKernel kernel(StorageConfig{root});

    AssertionId alice_at_acme = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);
    AssertionId bob_at_acme = kernel.commit(2, WORKS_AT, ACME, JAN_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    // Closed-interval and unrelated-object assertions must not show up.
    kernel.commit(ALICE, LIVES_IN, BETA, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.90);
    kernel.commit(2, LIVES_IN, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);

    auto who_works_at_acme = kernel.current_by_object(ACME);

    assert(who_works_at_acme.size() == 2);
    std::vector<AssertionId> ids;
    for (const auto &assertion : who_works_at_acme) {
        ids.push_back(assertion.id);
    }
    assert(std::find(ids.begin(), ids.end(), alice_at_acme) != ids.end());
    assert(std::find(ids.begin(), ids.end(), bob_at_acme) != ids.end());

    assert(kernel.current_by_object(BETA).empty());

    cleanup(root);
}

void current_by_object_excludes_superseded_and_resolves_merged_entities() {
    auto root = test_root("current_by_object_excludes_superseded_and_resolves_merged_entities");

    KnowledgeKernel kernel(StorageConfig{root});

    constexpr EntityId ACME_DUPLICATE = 999;

    AssertionId original = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);
    assert(kernel.current_by_object(ACME).size() == 1);

    kernel.commit_superseding(ALICE, WORKS_AT, BETA, JUL_2_2024, OPEN_ENDED, JUL_2_2024, 0.9, original);
    assert(kernel.current_by_object(ACME).empty());

    // A caller holding an id later merged into ACME transparently sees ACME's current results too.
    kernel.merge_entities(BETA, ACME_DUPLICATE, JUL_2_2024);
    auto via_duplicate = kernel.current_by_object(ACME_DUPLICATE);
    auto via_keep = kernel.current_by_object(BETA);
    assert(via_duplicate.size() == 1);
    assert(via_keep.size() == 1);
    assert(via_duplicate[0].id == via_keep[0].id);

    cleanup(root);
}

void current_by_predicate_returns_every_active_open_ended_assertion_for_the_predicate() {
    auto root = test_root("current_by_predicate_returns_every_active_open_ended_assertion_for_the_predicate");

    KnowledgeKernel kernel(StorageConfig{root});

    AssertionId alice_works = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);
    AssertionId bob_works = kernel.commit(2, WORKS_AT, BETA, JAN_1_2024, OPEN_ENDED, JUL_2_2024, 0.90);
    kernel.commit(ALICE, LIVES_IN, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    // Closed-interval WORKS_AT must not show up.
    kernel.commit(2, WORKS_AT, ACME, JAN_1_2020, JAN_1_2023, JUL_2_2024, 0.90);

    auto every_works_at = kernel.current_by_predicate(WORKS_AT);

    assert(every_works_at.size() == 2);
    std::vector<AssertionId> ids;
    for (const auto &assertion : every_works_at) {
        ids.push_back(assertion.id);
    }
    assert(std::find(ids.begin(), ids.end(), alice_works) != ids.end());
    assert(std::find(ids.begin(), ids.end(), bob_works) != ids.end());

    assert(kernel.current_by_predicate(999).empty());

    cleanup(root);
}

void hypotheses_for_returns_open_predictions() {
    auto root = test_root("hypotheses_for_returns_open_predictions");
    KnowledgeKernel kernel(StorageConfig{root});

    auto source = kernel.intern_entity("predictor_model");
    auto hypothesis_id = kernel.commit_hypothesis(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6,
                                                  source, JUL_2_2024, "predicted_by_model");
    kernel.commit(ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95);

    auto hypotheses = kernel.hypotheses_for(ALICE);
    assert(hypotheses.size() == 1);
    assert(hypotheses[0].id == hypothesis_id);
    assert(hypotheses[0].status == AssertionStatus::Hypothesis);

    assert(kernel.hypotheses_for(UNIVERSITY).empty());

    cleanup(root);
}

void promoting_a_hypothesis_preserves_it_in_commit_history() {
    auto root = test_root("promoting_a_hypothesis_preserves_it_in_commit_history");
    KnowledgeKernel kernel(StorageConfig{root});

    auto source = kernel.intern_entity("predictor_model");
    auto hypothesis_id = kernel.commit_hypothesis(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6,
                                                  source, JUL_2_2024, "predicted_by_model");

    // Promotion is not a new primitive: an ordinary commit_superseding confirms the prediction.
    auto confirmed_id =
        kernel.commit_superseding(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_3_2024, 0.97, hypothesis_id);

    auto history = kernel.commit_history(ALICE, WORKS_AT);
    assert(history.size() == 2);
    // commit_history is sorted by assertion id and unfiltered by status -- the original hypothesis
    // record stays visible, giving a free "predicted, then confirmed" audit trail. Its status flips
    // to Superseded, exactly like ordinary Active->Superseded promotion already works: "left
    // untouched" means the record isn't deleted or rewritten, not that its status is frozen.
    assert(history[0].id == hypothesis_id);
    assert(history[0].status == AssertionStatus::Superseded);
    assert(history[1].id == confirmed_id);
    assert(history[1].status == AssertionStatus::Active);

    // The promoted fact is now current; the hypothesis it replaced is not.
    auto current = kernel.current(ALICE);
    assert(current.size() == 1);
    assert(current[0].id == confirmed_id);

    assert(kernel.hypotheses_for(ALICE).empty());

    // The prediction's provenance survives promotion untouched -- explain() on the confirmed fact
    // walks back to the original, whose provenance still records it as a prediction.
    auto explanation = kernel.explain(confirmed_id);
    assert(explanation.size() == 2);
    assert(explanation[1].id == hypothesis_id);
    auto original_provenance = kernel.provenance_for(hypothesis_id);
    assert(original_provenance.has_value());
    assert(original_provenance->method == "predicted_by_model");

    cleanup(root);
}

void commit_hypothesis_requires_a_source_and_records_provenance() {
    auto root = test_root("commit_hypothesis_requires_a_source_and_records_provenance");
    KnowledgeKernel kernel(StorageConfig{root});

    auto source = kernel.intern_entity("predictor_model");
    auto hypothesis_id = kernel.commit_hypothesis(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6,
                                                  source, JUL_2_2024, "predicted_by_model");

    auto record = kernel.provenance_for(hypothesis_id);
    assert(record.has_value());
    assert(record->source == source);
    assert(record->recorded_at == JUL_2_2024);
    assert(record->method == "predicted_by_model");

    cleanup(root);
}

void hypothesis_is_preserved_across_kernel_restarts() {
    auto root = test_root("hypothesis_is_preserved_across_kernel_restarts");

    AssertionId hypothesis_id;
    EntityId source;

    {
        KnowledgeKernel kernel(StorageConfig{root});
        source = kernel.intern_entity("predictor_model");
        hypothesis_id = kernel.commit_hypothesis(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6, source,
                                                 JUL_2_2024, "predicted_by_model");
    }

    KnowledgeKernel reopened_kernel(StorageConfig{root});

    auto stored = reopened_kernel.get(hypothesis_id);
    assert(stored.has_value());
    assert(stored->status == AssertionStatus::Hypothesis);

    auto hypotheses = reopened_kernel.hypotheses_for(ALICE);
    assert(hypotheses.size() == 1);
    assert(hypotheses[0].id == hypothesis_id);

    assert(reopened_kernel.current(ALICE).empty());

    auto record = reopened_kernel.provenance_for(hypothesis_id);
    assert(record.has_value());
    assert(record->source == source);

    cleanup(root);
}

bool contains(const std::vector<EntityId> &ids, EntityId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void neighbors_respects_max_hops() {
    auto root = test_root("neighbors_respects_max_hops");
    KnowledgeKernel kernel(StorageConfig{root});

    // Chain: ALICE -> ACME -> BETA -> GAMMA
    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
    kernel.commit(ACME, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
    kernel.commit(BETA, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);

    assert(kernel.neighbors(ALICE, 0).empty());

    auto one_hop = kernel.neighbors(ALICE, 1);
    assert(one_hop.size() == 1);
    assert(contains(one_hop, ACME));

    auto two_hops = kernel.neighbors(ALICE, 2);
    assert(two_hops.size() == 2);
    assert(contains(two_hops, ACME));
    assert(contains(two_hops, BETA));

    auto three_hops = kernel.neighbors(ALICE, 3);
    assert(three_hops.size() == 3);
    assert(contains(three_hops, GAMMA));

    cleanup(root);
}

void neighbors_returns_empty_for_unknown_subject() {
    auto root = test_root("neighbors_returns_empty_for_unknown_subject");
    KnowledgeKernel kernel(StorageConfig{root});

    assert(kernel.neighbors(UNIVERSITY, 1).empty());

    cleanup(root);
}

void neighbors_deduplicates_and_avoids_cycles() {
    auto root = test_root("neighbors_deduplicates_and_avoids_cycles");
    KnowledgeKernel kernel(StorageConfig{root});

    // A two-entity cycle plus a third neighbor -- must terminate and not report ALICE itself.
    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
    kernel.commit(ACME, WORKS_AT, ALICE, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
    kernel.commit(ALICE, LIVES_IN, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);

    auto result = kernel.neighbors(ALICE, 2);
    assert(result.size() == 2);
    assert(contains(result, ACME));
    assert(contains(result, BETA));
    assert(!contains(result, ALICE));

    cleanup(root);
}

void neighbors_follows_incoming_edges_reverse_direction() {
    auto root = test_root("neighbors_follows_incoming_edges_reverse_direction");
    KnowledgeKernel kernel(StorageConfig{root});

    // ACME has no outgoing assertions of its own; ALICE is only reachable by following the
    // ALICE -> ACME edge backwards, via the reverse (object -> subject) index.
    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);

    auto result = kernel.neighbors(ACME, 1);
    assert(result.size() == 1);
    assert(contains(result, ALICE));

    cleanup(root);
}

void neighbors_excludes_non_current_edges() {
    auto root = test_root("neighbors_excludes_non_current_edges");
    KnowledgeKernel kernel(StorageConfig{root});

    // Closed-interval active assertion -- not open-ended, so not current.
    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.9);
    // Hypothesis -- never current regardless of interval.
    auto source = kernel.intern_entity("predictor_model");
    kernel.commit_hypothesis(ALICE, LIVES_IN, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6, source, JUL_2_2024,
                             "predicted_by_model");
    // Retracted -- was current, no longer is.
    auto retractable = kernel.commit(ALICE, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
    kernel.commit_retraction(ALICE, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_3_2024, 0.9, retractable);

    assert(kernel.neighbors(ALICE, 1).empty());
    assert(kernel.neighbors(ACME, 1).empty());
    assert(kernel.neighbors(BETA, 1).empty());
    assert(kernel.neighbors(GAMMA, 1).empty());

    cleanup(root);
}

void neighbors_reverse_edges_are_restored_after_kernel_restart() {
    auto root = test_root("neighbors_reverse_edges_are_restored_after_kernel_restart");

    AssertionId superseded_id;

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
        superseded_id = kernel.commit(STARTUP, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
        // Superseded before restart -- the reverse edge for the original target must not reappear.
        kernel.commit_superseding(STARTUP, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_3_2024, 0.95, superseded_id);
    }

    // A clean shutdown leaves every persisted index and the checkpoint consistent, so this restart
    // takes the fast path -- exactly the path whose bulk object-index seed this test exercises.
    KnowledgeKernel reopened_kernel(StorageConfig{root});

    auto acme_neighbors = reopened_kernel.neighbors(ACME, 1);
    assert(acme_neighbors.size() == 1);
    assert(contains(acme_neighbors, ALICE));

    assert(reopened_kernel.neighbors(GAMMA, 1).empty());

    auto beta_neighbors = reopened_kernel.neighbors(BETA, 1);
    assert(beta_neighbors.size() == 1);
    assert(contains(beta_neighbors, STARTUP));

    cleanup(root);
}

void co_occurring_predicates_returns_currently_active_predicates_for_subject() {
    auto root = test_root("co_occurring_predicates_returns_currently_active_predicates_for_subject");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
    kernel.commit(ALICE, LIVES_IN, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);

    auto predicates = kernel.co_occurring_predicates(ALICE);
    assert(predicates.size() == 2);
    assert(std::find(predicates.begin(), predicates.end(), WORKS_AT) != predicates.end());
    assert(std::find(predicates.begin(), predicates.end(), LIVES_IN) != predicates.end());

    assert(kernel.co_occurring_predicates(UNIVERSITY).empty());

    cleanup(root);
}

void co_occurring_predicates_excludes_hypothesis_and_superseded() {
    auto root = test_root("co_occurring_predicates_excludes_hypothesis_and_superseded");
    KnowledgeKernel kernel(StorageConfig{root});

    auto source = kernel.intern_entity("predictor_model");
    kernel.commit_hypothesis(ALICE, LIVES_IN, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6, source, JUL_2_2024,
                             "predicted_by_model");

    auto original = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
    kernel.commit_superseding(ALICE, WORKS_AT, GAMMA, JAN_1_2023, OPEN_ENDED, JUL_3_2024, 0.95, original);

    // WORKS_AT still co-occurs (the superseding assertion is current); LIVES_IN does not, since its
    // only assertion is a hypothesis, never current.
    auto predicates = kernel.co_occurring_predicates(ALICE);
    assert(predicates.size() == 1);
    assert(predicates[0] == WORKS_AT);

    cleanup(root);
}

void merge_entities_makes_queries_for_the_absorbed_id_resolve_to_the_surviving_id() {
    auto root = test_root("merge_entities_makes_queries_for_the_absorbed_id_resolve_to_the_surviving_id");
    KnowledgeKernel kernel(StorageConfig{root});

    constexpr EntityId ALICE_DUPLICATE = 999;

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);

    // Before the merge, the duplicate id is its own, unrelated entity.
    assert(kernel.current(ALICE_DUPLICATE).empty());
    assert(kernel.resolve_entity(ALICE_DUPLICATE) == ALICE_DUPLICATE);

    kernel.merge_entities(ALICE, ALICE_DUPLICATE, JUL_3_2024);

    assert(kernel.resolve_entity(ALICE_DUPLICATE) == ALICE);

    // A caller still holding the absorbed id transparently gets the surviving id's results across
    // every query-path method that takes a subject, without the underlying data ever moving.
    auto via_duplicate = kernel.current(ALICE_DUPLICATE);
    auto via_keep = kernel.current(ALICE);
    assert(via_duplicate.size() == 1);
    assert(via_duplicate[0].id == via_keep[0].id);

    assert(kernel.commit_history(ALICE_DUPLICATE, WORKS_AT).size() == 1);
    assert(kernel.valid_at(ALICE_DUPLICATE, JUL_2_2024).size() == 1);

    cleanup(root);
}

void merged_entity_redirect_is_preserved_across_kernel_restarts() {
    auto root = test_root("merged_entity_redirect_is_preserved_across_kernel_restarts");

    constexpr EntityId ALICE_DUPLICATE = 999;

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
        kernel.merge_entities(ALICE, ALICE_DUPLICATE, JUL_3_2024);
    }

    KnowledgeKernel reopened_kernel(StorageConfig{root});

    assert(reopened_kernel.resolve_entity(ALICE_DUPLICATE) == ALICE);
    assert(reopened_kernel.current(ALICE_DUPLICATE).size() == 1);

    cleanup(root);
}

void assertions_are_not_rewritten_by_a_merge() {
    auto root = test_root("assertions_are_not_rewritten_by_a_merge");
    KnowledgeKernel kernel(StorageConfig{root});

    constexpr EntityId ALICE_DUPLICATE = 999;

    auto id = kernel.commit(ALICE_DUPLICATE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);

    kernel.merge_entities(ALICE, ALICE_DUPLICATE, JUL_3_2024);

    // The stored assertion keeps the exact subject it was committed with -- resolution happens only
    // at the query boundary, never by rewriting assertions_.
    auto stored = kernel.get(id);
    assert(stored.has_value());
    assert(stored->subject == ALICE_DUPLICATE);

    cleanup(root);
}

void archive_segments_before_is_transparent_to_queries_and_survives_restart() {
    auto root = test_root("archive_segments_before_is_transparent_to_queries_and_survives_restart");

    AssertionId last_id = 0;
    {
        // Small segment capacity so a handful of commits span several segments.
        KnowledgeKernel kernel(StorageConfig{root, 2});

        for (int i = 0; i < 5; ++i) {
            last_id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.9);
        }

        // Archiving is compaction, not deletion: every already-committed assertion must still be
        // readable through the ordinary public API, both before and after a restart.
        kernel.archive_segments_before(last_id);

        assert(kernel.get(1).has_value());
        assert(kernel.get(last_id).has_value());
        assert(kernel.commit_history(ALICE, WORKS_AT).size() == 5);
    }

    KnowledgeKernel reopened_kernel(StorageConfig{root, 2});

    assert(reopened_kernel.get(1).has_value());
    assert(reopened_kernel.get(last_id).has_value());
    assert(reopened_kernel.commit_history(ALICE, WORKS_AT).size() == 5);

    cleanup(root);
}

void corrupt_entity_merge_log_is_fatal_on_startup() {
    auto root = test_root("corrupt_entity_merge_log_is_fatal_on_startup");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.merge_entities(ALICE, 999, JUL_3_2024);
    }

    auto merge_log_path = StorageConfig{root}.entity_merge_log_path();
    write_corrupt_record_size_after_valid_header(merge_log_path);

    bool threw = false;
    try {
        KnowledgeKernel recovered_kernel(StorageConfig{root});
    } catch (const std::runtime_error &) {
        threw = true;
    }

    // Like the catalog logs, entity_merges.log is authoritative with no self-heal fallback -- non-tail
    // corruption must surface as a fatal, uncaught startup error.
    assert(threw);

    cleanup(root);
}

} // namespace

int main() {
    commit_and_get_assertion();
    commit_by_name_interns_names_and_commits();
    commit_by_name_reuses_ids_for_repeated_names();
    commit_by_name_supports_a_literal_object();
    failed_commit_does_not_burn_id();
    get_unknown_assertion_returns_nullopt();
    current_assertion_is_preserved_across_kernels();
    current_assertion_returns_open_ended_assertion();
    current_by_name_resolves_the_named_subject_and_returns_current_assertions();
    current_by_name_returns_empty_for_an_unknown_name();
    current_by_object_returns_active_open_ended_assertions_referencing_the_entity();
    current_by_object_excludes_superseded_and_resolves_merged_entities();
    current_by_predicate_returns_every_active_open_ended_assertion_for_the_predicate();
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
    changes_since_returns_assertions_observed_at_or_after_cutoff_sorted_by_observed_at();
    changes_since_is_status_agnostic_and_spans_multiple_subjects();
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
    write_snapshot_then_restart_uses_snapshot_and_replays_only_the_tail();
    stale_or_corrupt_snapshot_falls_back_to_full_replay();
    snapshot_ahead_of_log_is_ignored();
    intern_entity_is_idempotent_and_returns_the_same_id_for_the_same_name();
    intern_value_is_idempotent_for_numeric_and_text_values();
    find_entity_returns_nullopt_for_an_unknown_name();
    entity_name_resolves_a_previously_interned_name();
    predicate_name_resolves_a_previously_interned_predicate();
    catalog_is_preserved_across_kernel_restarts();
    corrupt_entity_catalog_is_fatal_on_startup();
    corrupt_predicate_catalog_is_fatal_on_startup();
    payload_round_trips_large_content();
    intern_document_mints_ids_from_the_shared_entity_id_space();
    payload_is_preserved_across_kernel_restarts();
    corrupt_payload_is_fatal_on_startup();
    provenance_is_recorded_and_resolves_to_a_source_entity();
    record_provenance_rejects_an_unknown_assertion_target();
    provenance_is_preserved_across_kernel_restarts();
    corrupt_provenance_log_is_fatal_on_startup();
    commit_hypothesis_is_excluded_from_current_and_valid_at();
    hypotheses_for_returns_open_predictions();
    promoting_a_hypothesis_preserves_it_in_commit_history();
    commit_hypothesis_requires_a_source_and_records_provenance();
    hypothesis_is_preserved_across_kernel_restarts();
    neighbors_respects_max_hops();
    neighbors_returns_empty_for_unknown_subject();
    neighbors_deduplicates_and_avoids_cycles();
    neighbors_follows_incoming_edges_reverse_direction();
    neighbors_excludes_non_current_edges();
    neighbors_reverse_edges_are_restored_after_kernel_restart();
    co_occurring_predicates_returns_currently_active_predicates_for_subject();
    co_occurring_predicates_excludes_hypothesis_and_superseded();
    merge_entities_makes_queries_for_the_absorbed_id_resolve_to_the_surviving_id();
    merged_entity_redirect_is_preserved_across_kernel_restarts();
    assertions_are_not_rewritten_by_a_merge();
    archive_segments_before_is_transparent_to_queries_and_survives_restart();
    corrupt_entity_merge_log_is_fatal_on_startup();

    std::cout << "All assertion_kernel tests passed.\n";
    return 0;
}
