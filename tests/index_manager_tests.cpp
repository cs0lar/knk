#include <cassert>
#include <iostream>
#include <vector>

#include "kernel/index_manager.hpp"

using namespace knk;

namespace {

constexpr EntityId ALICE = 1;
constexpr EntityId BOB = 2;
constexpr EntityId ACME = 100;
constexpr EntityId BETA = 200;
constexpr PredicateId WORKS_AT = 10;
constexpr PredicateId LIVES_IN = 20;

constexpr Timestamp JAN_1_2023 = 1672531200;
constexpr Timestamp JAN_1_2024 = 1704067200;
constexpr Timestamp JUL_1_2024 = 1719792000;
constexpr Timestamp JUL_2_2024 = 1719878400;

Assertion assertion(AssertionId id, EntityId subject, PredicateId predicate, EntityId object, Timestamp valid_from,
                    Timestamp valid_to, AssertionStatus status = AssertionStatus::Active) {
    return Assertion{
        id, subject, predicate, object, valid_from, valid_to, JUL_2_2024, 0.95, status,
    };
}

void assert_ids_equal(const std::vector<AssertionId> &actual, const std::vector<AssertionId> &expected) {
    assert(actual.size() == expected.size());

    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(actual[i] == expected[i]);
    }
}

void add_indexes_assertions_by_subject() {
    IndexManager indexes;

    indexes.add(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED));
    indexes.add(assertion(2, ALICE, LIVES_IN, BETA, JAN_1_2024, OPEN_ENDED));
    indexes.add(assertion(3, BOB, WORKS_AT, ACME, JAN_1_2024, OPEN_ENDED));

    assert_ids_equal(indexes.assertions_for_subject(ALICE), {1, 2});
    assert_ids_equal(indexes.assertions_for_subject(BOB), {3});
    assert(indexes.assertions_for_subject(999).empty());
}

void add_indexes_active_open_ended_assertions_as_current() {
    IndexManager indexes;

    indexes.add(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED));
    indexes.add(assertion(2, ALICE, WORKS_AT, BETA, JAN_1_2024, OPEN_ENDED));

    assert_ids_equal(indexes.current_assertions(ALICE, WORKS_AT), {1, 2});
    assert(indexes.current_assertions(ALICE, LIVES_IN).empty());
}

void current_index_excludes_closed_superseded_and_retracted_assertions() {
    IndexManager indexes;

    indexes.add(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024));
    indexes.add(assertion(2, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, AssertionStatus::Superseded));
    indexes.add(assertion(3, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, AssertionStatus::Retracted));
    indexes.add(assertion(4, ALICE, WORKS_AT, BETA, JAN_1_2024, OPEN_ENDED));

    assert_ids_equal(indexes.assertions_for_subject(ALICE), {1, 2, 3, 4});
    assert_ids_equal(indexes.current_assertions(ALICE, WORKS_AT), {4});
}

void mark_superseded_removes_only_the_requested_current_assertion() {
    IndexManager indexes;

    indexes.add(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED));
    indexes.add(assertion(2, ALICE, WORKS_AT, BETA, JAN_1_2024, OPEN_ENDED));

    indexes.mark_superseded(1);

    assert_ids_equal(indexes.current_assertions(ALICE, WORKS_AT), {2});
    assert_ids_equal(indexes.assertions_for_subject(ALICE), {1, 2});
}

void mark_retracted_removes_only_the_requested_current_assertion() {
    IndexManager indexes;

    indexes.add(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED));
    indexes.add(assertion(2, ALICE, WORKS_AT, BETA, JAN_1_2024, OPEN_ENDED));

    indexes.mark_retracted(2);

    assert_ids_equal(indexes.current_assertions(ALICE, WORKS_AT), {1});
    assert_ids_equal(indexes.assertions_for_subject(ALICE), {1, 2});
}

void removing_unknown_or_non_current_assertion_is_a_noop() {
    IndexManager indexes;

    indexes.add(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024));
    indexes.add(assertion(2, ALICE, WORKS_AT, BETA, JAN_1_2024, OPEN_ENDED));

    indexes.mark_superseded(1);
    indexes.mark_retracted(999);

    assert_ids_equal(indexes.current_assertions(ALICE, WORKS_AT), {2});
    assert_ids_equal(indexes.assertions_for_subject(ALICE), {1, 2});
}

Assertion assertion_observed_at(AssertionId id, EntityId subject, PredicateId predicate, EntityId object,
                                Timestamp observed_at) {
    return Assertion{
        id, subject, predicate, object, JAN_1_2023, OPEN_ENDED, observed_at, 0.95, AssertionStatus::Active,
    };
}

void observed_before_returns_ids_with_observed_at_at_or_before_the_given_time() {
    IndexManager indexes;

    indexes.add(assertion_observed_at(1, ALICE, WORKS_AT, ACME, JAN_1_2023));
    indexes.add(assertion_observed_at(2, ALICE, LIVES_IN, BETA, JAN_1_2024));
    indexes.add(assertion_observed_at(3, ALICE, WORKS_AT, BETA, JUL_1_2024));

    assert_ids_equal(indexes.observed_before(ALICE, JAN_1_2023), {1});
    assert_ids_equal(indexes.observed_before(ALICE, JAN_1_2024), {1, 2});
    assert_ids_equal(indexes.observed_before(ALICE, JUL_1_2024), {1, 2, 3});
    assert(indexes.observed_before(ALICE, JAN_1_2023 - 1).empty());
}

void observed_before_orders_entries_by_observed_at_regardless_of_insertion_order() {
    IndexManager indexes;

    indexes.add(assertion_observed_at(1, ALICE, WORKS_AT, ACME, JUL_1_2024));
    indexes.add(assertion_observed_at(2, ALICE, LIVES_IN, BETA, JAN_1_2023));
    indexes.add(assertion_observed_at(3, ALICE, WORKS_AT, BETA, JAN_1_2024));

    assert_ids_equal(indexes.observed_before(ALICE, JUL_1_2024), {2, 3, 1});
}

void observed_before_scopes_to_subject_and_handles_unknown_subject() {
    IndexManager indexes;

    indexes.add(assertion_observed_at(1, ALICE, WORKS_AT, ACME, JAN_1_2023));
    indexes.add(assertion_observed_at(2, BOB, WORKS_AT, ACME, JAN_1_2023));

    assert_ids_equal(indexes.observed_before(ALICE, JUL_1_2024), {1});
    assert(indexes.observed_before(999, JUL_1_2024).empty());
}

void is_current_assertion_requires_active_status_and_open_ended_valid_to() {
    assert(is_current_assertion(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED)));
    assert(!is_current_assertion(assertion(2, ALICE, WORKS_AT, ACME, JAN_1_2023, JUL_1_2024)));
    assert(!is_current_assertion(
        assertion(3, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, AssertionStatus::Superseded)));
    assert(!is_current_assertion(
        assertion(4, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, AssertionStatus::Retracted)));
}

void restore_current_index_entry_reproduces_current_index_out_of_band() {
    IndexManager indexes;

    indexes.restore_current_index_entry(ALICE, WORKS_AT, 1, true);
    indexes.restore_current_index_entry(ALICE, WORKS_AT, 2, true);

    assert_ids_equal(indexes.current_assertions(ALICE, WORKS_AT), {1, 2});

    indexes.restore_current_index_entry(ALICE, WORKS_AT, 1, false);

    assert_ids_equal(indexes.current_assertions(ALICE, WORKS_AT), {2});
    assert_ids_equal(indexes.predicates_for_subject(ALICE), {WORKS_AT});

    indexes.restore_current_index_entry(ALICE, WORKS_AT, 2, false);

    assert(indexes.current_assertions(ALICE, WORKS_AT).empty());
    assert(indexes.predicates_for_subject(ALICE).empty());
}

void restore_current_index_entry_removal_of_unknown_assertion_is_a_noop() {
    IndexManager indexes;

    indexes.restore_current_index_entry(ALICE, WORKS_AT, 999, false);

    assert(indexes.current_assertions(ALICE, WORKS_AT).empty());
}

void restore_subject_entry_reproduces_subject_index_out_of_band() {
    IndexManager indexes;

    indexes.restore_subject_entry(ALICE, 1);

    assert_ids_equal(indexes.assertions_for_subject(ALICE), {1});
}

void subject_index_entries_returns_a_flat_snapshot_of_the_index() {
    IndexManager indexes;

    indexes.add(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED));
    indexes.add(assertion(2, BOB, WORKS_AT, ACME, JAN_1_2024, OPEN_ENDED));

    auto entries = indexes.subject_index_entries();

    assert(entries.size() == 2);

    bool has_alice = false;
    bool has_bob = false;
    for (const auto &[subject, id] : entries) {
        if (subject == ALICE && id == 1) {
            has_alice = true;
        }
        if (subject == BOB && id == 2) {
            has_bob = true;
        }
    }
    assert(has_alice);
    assert(has_bob);
}

void restore_observed_time_entry_reproduces_sorted_results_out_of_order() {
    IndexManager indexes;

    indexes.restore_observed_time_entry(ALICE, JUL_1_2024, 1);
    indexes.restore_observed_time_entry(ALICE, JAN_1_2023, 2);
    indexes.restore_observed_time_entry(ALICE, JAN_1_2024, 3);

    assert_ids_equal(indexes.observed_before(ALICE, JUL_1_2024), {2, 3, 1});
}

void observed_time_entries_returns_a_flat_snapshot_of_the_index() {
    IndexManager indexes;

    indexes.add(assertion_observed_at(1, ALICE, WORKS_AT, ACME, JAN_1_2023));
    indexes.add(assertion_observed_at(2, BOB, LIVES_IN, BETA, JAN_1_2024));

    auto entries = indexes.observed_time_entries();

    assert(entries.size() == 2);

    bool has_alice = false;
    bool has_bob = false;
    for (const auto &[subject, observed_at, id] : entries) {
        if (subject == ALICE && observed_at == JAN_1_2023 && id == 1) {
            has_alice = true;
        }
        if (subject == BOB && observed_at == JAN_1_2024 && id == 2) {
            has_bob = true;
        }
    }
    assert(has_alice);
    assert(has_bob);
}

void current_index_entries_returns_a_flat_snapshot_of_only_currently_active_entries() {
    IndexManager indexes;

    indexes.add(assertion(1, ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED));
    indexes.add(assertion(2, BOB, LIVES_IN, BETA, JAN_1_2024, OPEN_ENDED));
    indexes.add(assertion(3, ALICE, LIVES_IN, BETA, JAN_1_2023, JUL_1_2024));

    indexes.mark_superseded(1);

    auto entries = indexes.current_index_entries();

    assert(entries.size() == 1);
    auto &[subject, predicate, id] = entries.front();
    assert(subject == BOB);
    assert(predicate == LIVES_IN);
    assert(id == 2);
}

} // namespace

int main() {
    add_indexes_assertions_by_subject();
    add_indexes_active_open_ended_assertions_as_current();
    current_index_excludes_closed_superseded_and_retracted_assertions();
    mark_superseded_removes_only_the_requested_current_assertion();
    mark_retracted_removes_only_the_requested_current_assertion();
    removing_unknown_or_non_current_assertion_is_a_noop();
    observed_before_returns_ids_with_observed_at_at_or_before_the_given_time();
    observed_before_orders_entries_by_observed_at_regardless_of_insertion_order();
    observed_before_scopes_to_subject_and_handles_unknown_subject();
    is_current_assertion_requires_active_status_and_open_ended_valid_to();
    restore_current_index_entry_reproduces_current_index_out_of_band();
    restore_current_index_entry_removal_of_unknown_assertion_is_a_noop();
    restore_observed_time_entry_reproduces_sorted_results_out_of_order();
    observed_time_entries_returns_a_flat_snapshot_of_the_index();
    restore_subject_entry_reproduces_subject_index_out_of_band();
    subject_index_entries_returns_a_flat_snapshot_of_the_index();
    current_index_entries_returns_a_flat_snapshot_of_only_currently_active_entries();

    std::cout << "All index_manager tests passed.\n";
    return 0;
}
