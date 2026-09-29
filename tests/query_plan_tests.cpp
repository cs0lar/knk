// Phase 16: cost-based planning and explain_query.
//
// What needs pinning here is not "is the answer right" -- the differential suites already require every
// plan to produce identical rows -- but "is the *choice* right, and is the explanation of it true". The
// headline behaviour change from Phase 11 is that an index is no longer taken merely because it applies:
// an index row costs several times a scanned column row, so a predicate matching most of the corpus is
// cheaper to scan than to look up.

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/knowledge_kernel.hpp"
#include "kernel/query_plan.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

constexpr size_t SUBJECT_COUNT = 200;
constexpr PredicateId WORKS_AT = 1; // interned first, so id 1
constexpr PredicateId LIVES_IN = 2;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("query_plan_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

// 200 subjects with one WORKS_AT each, plus a handful of LIVES_IN rows: WORKS_AT covers most of the
// corpus (the case an index should lose), LIVES_IN covers very little (the case it should win).
void seed(KnowledgeKernel &kernel) {
    std::vector<PendingAssertion> rows;
    for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
        rows.push_back({static_cast<EntityId>(i + 1), WORKS_AT, 90'001, 0, OPEN_ENDED, static_cast<Timestamp>(i), 0.9});
    }
    for (size_t i = 0; i < 5; ++i) {
        rows.push_back(
            {static_cast<EntityId>(i + 1), LIVES_IN, 90'002, 0, OPEN_ENDED, static_cast<Timestamp>(1000 + i), 0.8});
    }
    kernel.commit_batch(rows);
}

const PlanOption *option_for(const QueryPlan &plan, PlanSource source) {
    for (const auto &option : plan.considered) {
        if (option.source == source) {
            return &option;
        }
    }
    return nullptr;
}

Query current_shaped() {
    Query query;
    query.statuses = {AssertionStatus::Active};
    query.open_ended_only = true;
    return query;
}

void a_selective_index_is_chosen_over_a_scan() {
    auto root = test_root("selective_index_is_chosen");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    Query query = current_shaped();
    query.subject = 3; // one WORKS_AT and one LIVES_IN row

    auto plan = kernel.explain_query(query);
    assert(plan.chosen == PlanSource::SubjectIndex);

    // Exact, not sampled: the planner reads the index bucket's size rather than estimating from
    // cardinality, which is why it can be compared against a scan with any confidence at all.
    const PlanOption *subject_index = option_for(plan, PlanSource::SubjectIndex);
    assert(subject_index != nullptr);
    assert(subject_index->rows == 2);
    assert(plan.estimated_rows == 2);
    assert(plan.total_rows == SUBJECT_COUNT + 5);

    cleanup(root);
}

void a_common_predicate_is_scanned_rather_than_looked_up() {
    auto root = test_root("common_predicate_is_scanned");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    // WORKS_AT covers 200 of 205 rows. Phase 11 would have taken the predicate index because it applied;
    // costing it says otherwise, since an index row is a random access into a record while a scanned
    // column row is streamed.
    Query common = current_shaped();
    common.predicate = WORKS_AT;

    auto plan = kernel.explain_query(common);
    assert(plan.chosen == PlanSource::ColumnarScan);

    const PlanOption *predicate_index = option_for(plan, PlanSource::PredicateCurrentIndex);
    assert(predicate_index != nullptr);
    assert(predicate_index->rejected_because.empty()); // usable, just not cheapest
    assert(predicate_index->rows == SUBJECT_COUNT);
    assert(predicate_index->cost > plan.estimated_cost);

    // A rare predicate flips the decision back, which is the whole point of costing rather than guessing.
    Query rare = current_shaped();
    rare.predicate = LIVES_IN;

    auto rare_plan = kernel.explain_query(rare);
    assert(rare_plan.chosen == PlanSource::PredicateCurrentIndex);
    assert(rare_plan.estimated_rows == 5);

    cleanup(root);
}

void the_smaller_of_two_usable_indexes_wins() {
    auto root = test_root("smaller_index_wins");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    // Both the object and predicate current indexes apply; the object one holds 5 rows against 200.
    Query query = current_shaped();
    query.predicate = WORKS_AT;
    query.object = 90'002; // the LIVES_IN object, 5 rows

    auto plan = kernel.explain_query(query);
    assert(plan.chosen == PlanSource::ObjectCurrentIndex);
    assert(plan.estimated_rows == 5);

    const PlanOption *predicate_index = option_for(plan, PlanSource::PredicateCurrentIndex);
    assert(predicate_index != nullptr && predicate_index->rows == SUBJECT_COUNT);

    cleanup(root);
}

void an_observed_bound_prefers_the_observed_time_index() {
    auto root = test_root("observed_bound_prefers_observed_index");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    Query query;
    query.subject = 1;
    query.observed_to = 0; // only the first of subject 1's two rows was observed this early

    auto plan = kernel.explain_query(query);
    assert(plan.chosen == PlanSource::ObservedTimeIndex);
    assert(plan.estimated_rows == 1);

    // The subject index was usable too, just larger -- a prefix is never worse.
    const PlanOption *subject_index = option_for(plan, PlanSource::SubjectIndex);
    assert(subject_index != nullptr && subject_index->rows == 2);

    cleanup(root);
}

void rejected_options_say_why() {
    auto root = test_root("rejected_options_say_why");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    // Not current-shaped: the current-state indexes hold only Active open-ended rows, so using one would
    // silently lose rows. That is a correctness rejection, and the explanation says so.
    Query audit;
    audit.predicate = WORKS_AT;

    auto audit_plan = kernel.explain_query(audit);
    const PlanOption *predicate_index = option_for(audit_plan, PlanSource::PredicateCurrentIndex);
    assert(predicate_index != nullptr);
    assert(predicate_index->rejected_because.find("current-shaped") != std::string::npos);

    // No subject named.
    const PlanOption *subject_index = option_for(audit_plan, PlanSource::SubjectIndex);
    assert(subject_index != nullptr);
    assert(subject_index->rejected_because.find("no subject") != std::string::npos);

    // The diagnostic switches are reported as rejections too, so an explanation of a forced plan is
    // still a true account of why the others were not taken.
    Query forced = current_shaped();
    forced.subject = 3;
    forced.force_scan = true;
    forced.force_row_scan = true;

    auto forced_plan = kernel.explain_query(forced);
    assert(forced_plan.chosen == PlanSource::RowScan);
    assert(option_for(forced_plan, PlanSource::SubjectIndex)->rejected_because == "force_scan");
    assert(option_for(forced_plan, PlanSource::ColumnarScan)->rejected_because == "force_row_scan");

    // The row scan is always available, which is what makes it the fallback.
    assert(option_for(forced_plan, PlanSource::RowScan)->rejected_because.empty());

    cleanup(root);
}

void a_filter_is_reported_as_per_row_work() {
    auto root = test_root("filter_reported_as_per_row");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    Query plain = current_shaped();
    plain.subject = 3;
    assert(!kernel.explain_query(plain).filter_evaluated_per_row);

    Query filtered = plain;
    filtered.filter = Filter::compare(FilterField::Confidence, CompareOp::Gte, Value::of_double(0.5));

    auto plan = kernel.explain_query(filtered);
    assert(plan.filter_evaluated_per_row);
    // Same source, higher cost: the filter does not change where rows come from, only what they cost.
    assert(plan.chosen == PlanSource::SubjectIndex);
    assert(plan.estimated_cost > kernel.explain_query(plain).estimated_cost);

    cleanup(root);
}

void explaining_rejects_exactly_what_running_rejects() {
    auto root = test_root("explaining_rejects_what_running_rejects");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    auto both_throw = [&kernel](const Query &query) {
        bool explain_threw = false;
        bool query_threw = false;
        try {
            kernel.explain_query(query);
        } catch (const std::runtime_error &) {
            explain_threw = true;
        }
        try {
            kernel.query(query);
        } catch (const std::runtime_error &) {
            query_threw = true;
        }
        return explain_threw && query_threw;
    };

    // Explaining a query that could never run would be worse than useless, so validation is identical.
    Query bad_version;
    bad_version.ir_version = QUERY_IR_VERSION + 1;
    assert(both_throw(bad_version));

    Query bad_filter;
    bad_filter.filter = Filter::compare(FilterField::Confidence, CompareOp::Eq, Value::of_int64(1));
    assert(both_throw(bad_filter));

    cleanup(root);
}

void the_plan_is_stable_and_never_changes_the_answer() {
    auto root = test_root("plan_is_stable_and_answer_preserving");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    std::vector<Query> queries;
    Query by_subject = current_shaped();
    by_subject.subject = 7;
    queries.push_back(by_subject);

    Query by_common_predicate = current_shaped();
    by_common_predicate.predicate = WORKS_AT;
    queries.push_back(by_common_predicate);

    Query by_rare_predicate = current_shaped();
    by_rare_predicate.predicate = LIVES_IN;
    queries.push_back(by_rare_predicate);

    Query audit;
    audit.observed_from = 0;
    queries.push_back(audit);

    for (const auto &query : queries) {
        // Planning twice must give the same plan, or an explanation is not a prediction.
        auto first = kernel.explain_query(query);
        auto second = kernel.explain_query(query);
        assert(first.chosen == second.chosen);
        assert(first.estimated_rows == second.estimated_rows);
        assert(first.estimated_cost == second.estimated_cost);

        // And whatever it chose, the rows are the rows: the same query forced down the other two paths
        // answers identically. Selection changes cost, never results.
        auto planned = kernel.query(query);

        Query scanned = query;
        scanned.force_scan = true;
        auto columnar = kernel.query(scanned);

        Query rows_only = scanned;
        rows_only.force_row_scan = true;
        auto rows = kernel.query(rows_only);

        assert(planned.assertions.size() == columnar.assertions.size());
        assert(planned.assertions.size() == rows.assertions.size());
        for (size_t i = 0; i < planned.assertions.size(); ++i) {
            assert(planned.assertions[i].id == columnar.assertions[i].id);
            assert(planned.assertions[i].id == rows.assertions[i].id);
        }
    }

    cleanup(root);
}

void a_tiny_corpus_is_scanned_rather_than_indexed() {
    auto root = test_root("tiny_corpus_is_scanned");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(1, WORKS_AT, 90'001, 0, OPEN_ENDED, 0, 0.9);

    // Surprising at first glance, and correct: an index lookup is a hash probe plus building a vector of
    // ids, which costs more than streaming a one-row column. A planner that reached for an index here
    // because one "applies" would be slower than no planner at all. This test exists because it caught
    // the author expecting the opposite.
    Query query = current_shaped();
    query.predicate = WORKS_AT;

    auto plan = kernel.explain_query(query);
    assert(plan.chosen == PlanSource::ColumnarScan);
    assert(option_for(plan, PlanSource::PredicateCurrentIndex)->rejected_because.empty()); // usable, dearer

    cleanup(root);
}

void planning_follows_the_corpus_as_it_grows() {
    auto root = test_root("planning_follows_the_corpus");
    KnowledgeKernel kernel(StorageConfig{root});

    // A corpus where WORKS_AT is rare: 200 LIVES_IN rows against 5 WORKS_AT.
    std::vector<PendingAssertion> initial;
    for (size_t i = 0; i < 200; ++i) {
        initial.push_back(
            {static_cast<EntityId>(i + 1), LIVES_IN, 90'002, 0, OPEN_ENDED, static_cast<Timestamp>(i), 0.8});
    }
    for (size_t i = 0; i < 5; ++i) {
        initial.push_back(
            {static_cast<EntityId>(i + 1), WORKS_AT, 90'001, 0, OPEN_ENDED, static_cast<Timestamp>(i), 0.9});
    }
    kernel.commit_batch(initial);

    Query query = current_shaped();
    query.predicate = WORKS_AT;

    auto rare = kernel.explain_query(query);
    assert(rare.chosen == PlanSource::PredicateCurrentIndex);
    assert(rare.estimated_rows == 5);

    // Grow WORKS_AT until it covers most of the corpus. The plan has to move on its own: a planner that
    // cannot change its mind as the data changes is a heuristic with extra steps.
    std::vector<PendingAssertion> growth;
    for (size_t i = 0; i < 500; ++i) {
        growth.push_back(
            {static_cast<EntityId>(i + 300), WORKS_AT, 90'001, 0, OPEN_ENDED, static_cast<Timestamp>(i), 0.9});
    }
    kernel.commit_batch(growth);

    auto common = kernel.explain_query(query);
    assert(common.chosen == PlanSource::ColumnarScan);
    assert(common.total_rows == 705);
    assert(option_for(common, PlanSource::PredicateCurrentIndex)->rows == 505);

    cleanup(root);
}

} // namespace

int main() {
    a_selective_index_is_chosen_over_a_scan();
    a_common_predicate_is_scanned_rather_than_looked_up();
    the_smaller_of_two_usable_indexes_wins();
    an_observed_bound_prefers_the_observed_time_index();
    rejected_options_say_why();
    a_filter_is_reported_as_per_row_work();
    explaining_rejects_exactly_what_running_rejects();
    the_plan_is_stable_and_never_changes_the_answer();
    a_tiny_corpus_is_scanned_rather_than_indexed();
    planning_follows_the_corpus_as_it_grows();

    std::cout << "All query_plan tests passed.\n";
    return 0;
}
