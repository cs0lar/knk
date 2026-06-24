#include <cassert>
#include <iostream>
#include <vector>


#include <bitemporal/assertion_store.hpp>

using namespace bitemporal::assertion_store;

namespace {

constexpr EntityId ALICE = 1;
constexpr EntityId ACME = 100;
constexpr EntityId BETA = 200;
constexpr PredicateId WORKS_AT = 10;

constexpr Timestamp JAN_1_2023 = 1672531200;
constexpr Timestamp JAN_1_2024 = 1704067200;
constexpr Timestamp JUL_1_2024 = 1719792000;
constexpr Timestamp JUL_2_2024 = 1719878400;

void append_and_get_fact() {
	AssertionStore store;

	auto id  = store.append(
	               ALICE,
	               WORKS_AT,
	               ACME,
	               JAN_1_2023,
	               OPEN_ENDED,
	               JUL_2_2024,
	               0.95
	           );

	auto fact = store.get(id);

	assert(fact.has_value());
	assert(fact->id == id);
	assert(fact->subject == ALICE);
	assert(fact->predicate == WORKS_AT);
	assert(fact->object == ACME);
	assert(fact->valid_from == JAN_1_2023);
	assert(fact->valid_to == OPEN_ENDED);
	assert(fact->observed_at == JUL_2_2024);
	assert(fact->confidence == 0.95);
	assert(fact->status == AssertionStatus::Active);
}

void get_unknown_fact_returns_nullopt() {
	AssertionStore store;

	auto fact = store.get(999);

	assert(!fact.has_value());
}

void current_fact_returns_open_ended_fact() {
	AssertionStore store;

	store.append(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_2_2024,
	    0.95
	);

	store.append(
	    ALICE,
	    WORKS_AT,
	    BETA,
	    JUL_1_2024,
	    OPEN_ENDED,
	    JUL_2_2024,
	    0.90
	);

	auto current = store.current_facts(ALICE);

	assert(current.size() == 1);
	assert(current[0].object == BETA);
}

void valid_at_returns_historical_fact() {
	AssertionStore store;

	store.append(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_2_2024,
	    0.95
	);

	store.append(
	    ALICE,
	    WORKS_AT,
	    BETA,
	    JUL_1_2024,
	    OPEN_ENDED,
	    JUL_2_2024,
	    0.90
	);

	auto facts = store.valid_at(ALICE, JAN_1_2024);

	assert(facts.size() == 1);
	assert(facts[0].object == ACME);
}

void valid_at_respects_exclusive_valid_to() {
	AssertionStore store;

	store.append(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_2_2024,
	    0.95
	);

	auto facts = store.valid_at(ALICE, JUL_1_2024);

	assert(facts.empty());
}

void facts_for_subject_returns_all_subject_facts() {
	AssertionStore store;

	store.append(
	    ALICE,
	    WORKS_AT,
	    ACME,
	    JAN_1_2023,
	    JUL_1_2024,
	    JUL_2_2024,
	    0.95
	);

	store.append(
	    ALICE,
	    WORKS_AT,
	    BETA,
	    JUL_1_2024,
	    OPEN_ENDED,
	    JUL_2_2024,
	    0.90
	);

	auto facts = store.facts_for_subject(ALICE);

	assert(facts.size() == 2);
	assert(facts[0].object == ACME);
	assert(facts[1].object == BETA);
}

}

int main()
{
	append_and_get_fact();
	get_unknown_fact_returns_nullopt();
	current_fact_returns_open_ended_fact();
	valid_at_returns_historical_fact();
	valid_at_respects_exclusive_valid_to();
	facts_for_subject_returns_all_subject_facts();

	std::cout << "All assertion_store tests passed.\n";
	return 0;
}