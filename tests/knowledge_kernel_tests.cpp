#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

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

std::filesystem::path test_root(const std::string& name) {
	auto path = std::filesystem::temp_directory_path() / ("knowledge_kernel_" + name);
	std::filesystem::remove_all(path);
	return path;
}

void cleanup(const std::filesystem::path& path) {
	std::filesystem::remove_all(path);
}

void commit_and_get_assertion() {
	auto root = test_root("commit_and_get_assertion");
	KnowledgeKernel kernel(StorageConfig{root});

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

	cleanup(root);
}

void failed_commit_does_not_burn_id() {
	auto root = test_root("failed_commit_does_not_burn_id");

	KnowledgeKernel kernel(StorageConfig{root});
	auto log_path = StorageConfig{root}.assertion_log_path();

	std::filesystem::create_directory(log_path);

	bool failed = false;
	try {
		kernel.commit(
		    ALICE,
		    WORKS_AT,
		    ACME,
		    JAN_1_2023,
		    OPEN_ENDED,
		    JUL_2_2024,
		    0.95
		);
	}
	catch (const std::runtime_error&) {
		failed = true;
	}

	assert(failed);
	std::filesystem::remove(log_path);

	auto other_id = kernel.commit(
	                    ALICE,
	                    WORKS_AT,
	                    ACME,
	                    JAN_1_2023,
	                    OPEN_ENDED,
	                    JUL_2_2024,
	                    0.95
	                );

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

	KnowledgeKernel other_kernel(StorageConfig{root});

	auto current = other_kernel.current(ALICE);

	assert(current.size() == 1);
	assert(current[0].object == BETA);

	cleanup(root);
}

void current_assertion_returns_open_ended_assertion() {
	auto root = test_root("current_assertion_returns_open_ended_assertion");
	KnowledgeKernel kernel(StorageConfig{root});

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

	cleanup(root);
}

void valid_at_returns_historical_assertion() {
	auto root = test_root("valid_at_returns_historical_assertion");
	KnowledgeKernel kernel(StorageConfig{root});

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

	cleanup(root);
}

void valid_at_is_preserved_across_kernels() {
	auto root = test_root("valid_at_is_preserved_across_kernels");
	KnowledgeKernel kernel(StorageConfig{root});

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

	KnowledgeKernel other_kernel(StorageConfig{root});
	auto assertions = other_kernel.valid_at(ALICE, JAN_1_2024);

	assert(assertions.size() == 1);
	assert(assertions[0].object == ACME);

	cleanup(root);
}

void valid_at_respects_exclusive_valid_to() {
	auto root = test_root("valid_at_respects_exclusive_valid_to");
	KnowledgeKernel kernel(StorageConfig{root});

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

	cleanup(root);
}

void known_at_excludes_future_observed_fact() {
	auto root = test_root("known_at_excludes_future_observed_fact");
	KnowledgeKernel kernel(StorageConfig{root});

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

	cleanup(root);
}

void valid_at_known_at_respects_both_times() {
	auto root = test_root("valid_at_known_at_respects_both_times");
	KnowledgeKernel kernel(StorageConfig{root});

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

	cleanup(root);
}

void assertions_for_subject_returns_all_subject_assertions() {
	auto root = test_root("assertions_for_subject_returns_all_subject_assertions");
	KnowledgeKernel kernel(StorageConfig{root});

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

	cleanup(root);
}

void constructor_replays_assertions_and_continues_ids() {
	auto root = test_root("constructor_replays_assertions_and_continues_ids");

	{
		KnowledgeKernel kernel(StorageConfig{root});
		auto id = kernel.commit(
		              ALICE,
		              WORKS_AT,
		              ACME,
		              JAN_1_2023,
		              OPEN_ENDED,
		              JUL_2_2024,
		              0.95
		          );

		assert(id == 1);
	}

	KnowledgeKernel kernel(StorageConfig{root});

	auto replayed = kernel.get(1);
	assert(replayed.has_value());
	assert(replayed->object == ACME);

	auto next_id = kernel.commit(
	                   ALICE,
	                   WORKS_AT,
	                   BETA,
	                   JUL_1_2024,
	                   OPEN_ENDED,
	                   JUL_3_2024,
	                   0.90
	               );

	assert(next_id == 2);

	cleanup(root);
}

}

int main()
{
	commit_and_get_assertion();
	failed_commit_does_not_burn_id();
	get_unknown_assertion_returns_nullopt();
	current_assertion_is_preserved_across_kernels();
	current_assertion_returns_open_ended_assertion();
	valid_at_returns_historical_assertion();
	valid_at_is_preserved_across_kernels();
	valid_at_respects_exclusive_valid_to();
	valid_at_known_at_respects_both_times();
	known_at_excludes_future_observed_fact();
	assertions_for_subject_returns_all_subject_assertions();
	constructor_replays_assertions_and_continues_ids();

	std::cout << "All assertion_kernel tests passed.\n";
	return 0;
}
