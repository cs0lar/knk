#include <cassert>
#include <iostream>
#include <vector>


#include <kernel/knowledge_kernel.hpp>

using namespace knk;

namespace {

constexpr EntityId ALICE = 1;
constexpr EntityId ACME = 100;
constexpr EntityId BETA = 200;
constexpr PredicateId WORKS_AT = 10;

constexpr Timestamp JAN_1_2023 = 1672531200;
constexpr Timestamp JAN_1_2024 = 1704067200;
constexpr Timestamp JUL_1_2024 = 1719792000;
constexpr Timestamp JUL_2_2024 = 1719878400;
constexpr Timestamp JUL_3_2024 = 1719961200;

void commit_and_get_assertion() {
	KnowledgeKernel kernel;

	auto id  = kernel.commit(
	               ALICE,
	               WORKS_AT,
	               ACME,
	               JAN_1_2023,
	               OPEN_ENDED,
	               JUL_2_2024,
	               0.95
	           );

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
}

void get_unknown_assertion_returns_nullopt() {
	KnowledgeKernel kernel;

	auto assertion = kernel.get(999);

	assert(!assertion.has_value());
}

void current_assertion_returns_open_ended_assertion() {
	KnowledgeKernel kernel;

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_2_2024,
	    0.95
	);

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    BETA,
	    JUL_1_2024,
	    OPEN_ENDED,
	    JUL_2_2024,
	    0.90
	);

	auto current = kernel.current(ALICE);

	assert(current.size() == 1);
	assert(current[0].object == BETA);
}

void valid_at_returns_historical_assertion() {
	KnowledgeKernel kernel;

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_2_2024,
	    0.95
	);

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    BETA,
	    JUL_1_2024,
	    OPEN_ENDED,
	    JUL_2_2024,
	    0.90
	);

	auto assertions = kernel.valid_at(ALICE, JAN_1_2024);

	assert(assertions.size() == 1);
	assert(assertions[0].object == ACME);
}

void valid_at_respects_exclusive_valid_to() {
	KnowledgeKernel kernel;

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_2_2024,
	    0.95
	);

	auto assertions = kernel.valid_at(ALICE, JUL_1_2024);

	assert(assertions.empty());
}

void known_at_excludes_future_observed_fact() {
	KnowledgeKernel kernel;

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_1_2024,
	    0.95
	);

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    BETA,
	    JUL_1_2024,
	    OPEN_ENDED,
	    JUL_2_2024,
	    0.90
	);

	auto assertions = kernel.known_at(ALICE, JUL_1_2024);

	assert(assertions.size() == 1);
	assert(assertions[0].object == ACME);
}

void valid_at_known_at_respects_both_times() {
	KnowledgeKernel kernel;

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_1_2024,
	    0.95
	);

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    BETA,
	    JUL_1_2024,
	    OPEN_ENDED,
	    JUL_2_2024,
	    0.90
	);

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
}

void assertions_for_subject_returns_all_subject_assertions() {
	KnowledgeKernel kernel;

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_2_2024,
	    0.95
	);

	kernel.commit(
	    ALICE,
	    WORKS_AT,
	    BETA,
	    JUL_1_2024,
	    OPEN_ENDED,
	    JUL_2_2024,
	    0.90
	);

	auto assertions = kernel.assertions_for_subject(ALICE);

	assert(assertions.size() == 2);
	assert(assertions[0].object == ACME);
	assert(assertions[1].object == BETA);
}

}

int main()
{
	commit_and_get_assertion();
	get_unknown_assertion_returns_nullopt();
	current_assertion_returns_open_ended_assertion();
	valid_at_returns_historical_assertion();
	valid_at_respects_exclusive_valid_to();
	valid_at_known_at_respects_both_times();
	known_at_excludes_future_observed_fact();
	assertions_for_subject_returns_all_subject_assertions();

	std::cout << "All assertion_kernel tests passed.\n";
	return 0;
}