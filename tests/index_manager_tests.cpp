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

} // namespace

int main() {
    add_indexes_assertions_by_subject();
    add_indexes_active_open_ended_assertions_as_current();
    current_index_excludes_closed_superseded_and_retracted_assertions();
    mark_superseded_removes_only_the_requested_current_assertion();
    mark_retracted_removes_only_the_requested_current_assertion();
    removing_unknown_or_non_current_assertion_is_a_noop();

    std::cout << "All index_manager tests passed.\n";
    return 0;
}
