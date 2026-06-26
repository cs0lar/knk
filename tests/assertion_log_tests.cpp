#include <cassert>
#include <filesystem>
#include <iostream>

#include <kernel/assertion_log.hpp>

using namespace knk;

namespace {

void assertion_log_appends_and_reads_assertions() {
	auto path = std::filesystem::temp_directory_path() / "kernel_assertion_log_test.log";
	std::filesystem::remove(path);

	AssertionLog log(path);

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

	auto assertions = log.read_all();

	assert(assertions.size() == 1);
	assert(assertions[0].id == 1);
	assert(assertions[0].subject == 1);
	assert(assertions[0].predicate == 10);
	assert(assertions[0].object == 100);
	assert(assertions[0].confidence == 0.95);
	assert(assertions[0].status == AssertionStatus::Active);

	std::filesystem::remove(path);

}

void assertion_log_returns_empty_when_missing() {
	auto path = std::filesystem::temp_directory_path() / "kernel_missing_assertion_log.log";
	std::filesystem::remove(path);

	AssertionLog log(path);

	auto assertions = log.read_all();

	assert(assertions.empty());
}

}

int main() {
	assertion_log_appends_and_reads_assertions();
	assertion_log_returns_empty_when_missing();

	std::cout << "All assertion_log tests passed.\n";
}