#include <cassert>
#include <filesystem>
#include <iostream>

#include <bitemporal/fact_log.hpp>

using namespace bt;

namespace {

void fact_log_appends_and_reads_assertions() {
	auto path = std::filesystem::temp_directory_path() / "bitemporal_fact_log_test.log";
	std::filesystem::remove(path);

	FactLog log(path);

	Assertion a {
		.id = 1,
		.subject = 1,
		.predicate = 10,
		.object = 100,
		.valid_from = 1672531200,
		.valid_to = OPEN_ENDED,
		.observed_at = 1719878400,
		.confidence = 0.95,
		.status = AssertionStatus::Active
	};

	log.append(a);

	auto facts = log.read_all();

	assert(facts.size() == 1);
	assert(facts[0].id == 1);
	assert(facts[0].subject == 1);
	assert(facts[0].predicate == 10);
	assert(facts[0].object == 100);
	assert(facts[0].confidence == 0.95);
	assert(facts[0].status == AssertionStatus::Active);

	std::filesystem::remove(path);

}

void fact_log_returns_empty_when_missing() {
	auto path = std::filesystem::temp_directory_path() / "bitemporal_missing_fact_log.log";
	std::filesystem::remove(path);

	FactLog log(path);

	auto facts = log.read_all();

	assert(facts.empty());
}

}

int main() {
	fact_log_appends_and_reads_assertions();
	fact_log_returns_empty_when_missing();

	std::cout << "All fact_log tests passed.\n";
}