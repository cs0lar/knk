#include <iostream>

#include <bitemporal/assertion_store.hpp>

using namespace bitemporal::assertion_store;

int main(int argc, char const *argv[])
{
	AssertionStore store;

	EntityId alice = 1;
	EntityId acme = 100;
	EntityId beta = 200;

	PredicateId works_at = 10;

	store.append(
	    alice,
	    works_at,
	    acme,
	    1672531200, // 2023-01-01
	    1719792000, // 2024-07-01
	    1719878400, // observed 2024-07-02
	    0.95
	);

	store.append(
	    alice,
	    works_at,
	    beta,
	    1719792000, // 2024-07-01
	    OPEN_ENDED,
	    1719878400,
	    0.90
	);

	auto current = store.current_facts(alice);

	std::cout << "Current facts for Alice\n";
	for (const auto& fact : current) {
		std::cout
		        << "subject=" << fact.subject
		        << " predicate=" << fact.predicate
		        << " object=" << fact.object
		        << " confidence=" << fact.confidence
		        << "\n";
	}

	auto historical = store.valid_at(alice, 1704067200);

	std::cout << "\nFacts valid on 2024-01-01:\n";
	for (const auto& fact : historical) {
		std::cout
		        << "subject=" << fact.subject
		        << " predicate=" << fact.predicate
		        << " object=" << fact.object
		        << "\n";
	}
}