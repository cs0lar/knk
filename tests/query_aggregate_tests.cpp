// Phase 12: aggregation and grouping.
//
// The selection half of an AggregateQuery is an ordinary Query, so what these tests mostly pin down is
// the part that is genuinely new: how rows fold into groups, what happens to rows that have no value to
// aggregate, and which caller mistakes are rejected rather than silently answered. The randomized
// differential suite (tests/query_differential_tests.cpp) checks aggregates against a brute-force
// implementation; this file checks the specific behaviors those random queries would only hit by luck.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/aggregate.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("query_aggregate_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

struct Fixture {
    EntityId alice;
    EntityId bob;
    EntityId carol;
    EntityId salary_120k;
    EntityId salary_90k;
    EntityId salary_150k;
    EntityId salary_100k;
    EntityId eng;
    EntityId sales;
    PredicateId salary;
    PredicateId dept;

    AssertionId old_salary; // Superseded
    AssertionId alice_salary;
    AssertionId bob_salary;
    AssertionId carol_salary;
    AssertionId alice_dept;
    AssertionId bob_dept;
    AssertionId carol_dept;
    AssertionId early_dept; // negative observed_at, for bucket floor behavior
};

// Numeric object values (salaries) so sum/avg/min/max have something real to read, alongside text
// object values (departments) that those same functions must skip rather than treat as zero.
Fixture seed(KnowledgeKernel &kernel) {
    Fixture f;
    f.alice = kernel.intern_entity("Alice");
    f.bob = kernel.intern_entity("Bob");
    f.carol = kernel.intern_entity("Carol");
    f.salary = kernel.intern_predicate("salary");
    f.dept = kernel.intern_predicate("dept");
    f.salary_100k = kernel.intern_value(Value::of_int64(100000));
    f.salary_120k = kernel.intern_value(Value::of_int64(120000));
    f.salary_90k = kernel.intern_value(Value::of_int64(90000));
    f.salary_150k = kernel.intern_value(Value::of_int64(150000));
    f.eng = kernel.intern_entity("eng");
    f.sales = kernel.intern_entity("sales");

    f.old_salary = kernel.commit(f.alice, f.salary, f.salary_100k, 0, OPEN_ENDED, 50, 0.80);
    f.alice_salary =
        kernel.commit_superseding(f.alice, f.salary, f.salary_120k, 0, OPEN_ENDED, 100, 0.90, f.old_salary);
    f.bob_salary = kernel.commit(f.bob, f.salary, f.salary_90k, 0, OPEN_ENDED, 200, 0.60);
    f.carol_salary = kernel.commit(f.carol, f.salary, f.salary_150k, 0, OPEN_ENDED, 300, 0.95);

    f.alice_dept = kernel.commit(f.alice, f.dept, f.eng, 0, OPEN_ENDED, 100, 1.0);
    f.bob_dept = kernel.commit(f.bob, f.dept, f.eng, 0, OPEN_ENDED, 200, 1.0);
    f.carol_dept = kernel.commit(f.carol, f.dept, f.sales, 0, OPEN_ENDED, 300, 1.0);
    f.early_dept = kernel.commit(f.carol, f.dept, f.eng, 0, OPEN_ENDED, -50, 1.0);

    return f;
}

Query active_salaries(const Fixture &f) {
    Query selection;
    selection.predicate = f.salary;
    selection.statuses = {AssertionStatus::Active};
    return selection;
}

// Every aggregate is run with index selection and with force_scan, and the two must agree -- the same
// property the row queries hold, extended to the aggregate path, which selects candidates the same way.
AggregateResult run_both_ways(const KnowledgeKernel &kernel, AggregateQuery query) {
    query.selection.force_scan = false;
    auto indexed = kernel.aggregate(query);

    query.selection.force_scan = true;
    auto scanned = kernel.aggregate(query);

    assert(indexed.groups.size() == scanned.groups.size());
    for (size_t i = 0; i < indexed.groups.size(); ++i) {
        assert(indexed.groups[i].key == scanned.groups[i].key);
        assert(indexed.groups[i].row_count == scanned.groups[i].row_count);
        assert(indexed.groups[i].values.size() == scanned.groups[i].values.size());
        for (size_t j = 0; j < indexed.groups[i].values.size(); ++j) {
            assert(indexed.groups[i].values[j].count == scanned.groups[i].values[j].count);
            assert(indexed.groups[i].values[j].number.has_value() == scanned.groups[i].values[j].number.has_value());
            if (indexed.groups[i].values[j].number.has_value()) {
                assert(std::abs(*indexed.groups[i].values[j].number - *scanned.groups[i].values[j].number) < 1e-9);
            }
        }
    }

    return indexed;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

void aggregate_without_grouping_is_one_global_group() {
    auto root = test_root("aggregate_without_grouping_is_one_global_group");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    AggregateQuery query;
    query.selection = active_salaries(f);
    query.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue},
                          {AggregateFunction::Sum, AggregateTarget::ObjectValue},
                          {AggregateFunction::Avg, AggregateTarget::ObjectValue},
                          {AggregateFunction::Min, AggregateTarget::ObjectValue},
                          {AggregateFunction::Max, AggregateTarget::ObjectValue}};

    auto result = run_both_ways(kernel, query);

    assert(result.groups.size() == 1);
    assert(result.groups[0].key.empty()); // a global aggregate has no key
    assert(result.groups[0].row_count == 3);

    // The superseded 100k row is excluded by the status filter, so these are 120k + 90k + 150k.
    assert(result.groups[0].values[0].count == 3);
    assert(near(*result.groups[0].values[1].number, 360000.0));
    assert(near(*result.groups[0].values[2].number, 120000.0));
    assert(near(*result.groups[0].values[3].number, 90000.0));
    assert(near(*result.groups[0].values[4].number, 150000.0));

    cleanup(root);
}

void aggregate_groups_by_subject_status_and_object() {
    auto root = test_root("aggregate_groups_by_subject_status_and_object");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    AggregateQuery by_subject;
    by_subject.selection = active_salaries(f);
    by_subject.group_by = {{GroupField::Subject, 0}};
    by_subject.aggregations = {{AggregateFunction::Max, AggregateTarget::ObjectValue}};

    auto subjects = run_both_ways(kernel, by_subject);
    assert(subjects.groups.size() == 3);
    // Keys are ordered deterministically, and subject ids were interned in Alice/Bob/Carol order.
    assert(subjects.groups[0].key[0] == Value::of_int64(static_cast<int64_t>(f.alice)));
    assert(near(*subjects.groups[0].values[0].number, 120000.0));
    assert(near(*subjects.groups[2].values[0].number, 150000.0));

    // Every status, grouped: the supersession left one Superseded row behind.
    AggregateQuery by_status;
    by_status.group_by = {{GroupField::Status, 0}};
    by_status.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};

    auto statuses = run_both_ways(kernel, by_status);
    assert(statuses.groups.size() == 2);
    assert(statuses.groups[0].key[0] == Value::of_text("Active"));
    assert(statuses.groups[0].values[0].count == 7);
    assert(statuses.groups[1].key[0] == Value::of_text("Superseded"));
    assert(statuses.groups[1].values[0].count == 1);

    // Grouping by object over departments: two in eng, one in sales, plus Carol's earlier eng row.
    AggregateQuery by_object;
    by_object.selection.predicate = f.dept;
    by_object.selection.statuses = {AssertionStatus::Active};
    by_object.group_by = {{GroupField::Object, 0}};
    by_object.aggregations = {{AggregateFunction::CountDistinct, AggregateTarget::Subject}};

    auto objects = run_both_ways(kernel, by_object);
    assert(objects.groups.size() == 2);
    // eng was interned before sales, so its id sorts first.
    assert(objects.groups[0].key[0] == Value::of_int64(static_cast<int64_t>(f.eng)));
    assert(objects.groups[0].row_count == 3);       // Alice, Bob, and Carol's early row
    assert(objects.groups[0].values[0].count == 3); // three distinct subjects
    assert(objects.groups[1].values[0].count == 1);

    cleanup(root);
}

void aggregate_buckets_time_including_before_the_epoch() {
    auto root = test_root("aggregate_buckets_time_including_before_the_epoch");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    AggregateQuery query;
    query.group_by = {{GroupField::ObservedAtBucket, 100}};
    query.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};

    auto result = run_both_ways(kernel, query);

    // Buckets are floor(t / width) * width, so observed_at -50 belongs to -100 rather than 0 -- the
    // behavior truncating division would get wrong.
    assert(result.groups.size() == 5);
    assert(result.groups[0].key[0] == Value::of_timestamp(-100));
    assert(result.groups[0].values[0].count == 1);
    assert(result.groups[1].key[0] == Value::of_timestamp(0));
    assert(result.groups[1].values[0].count == 1); // the 50 row
    assert(result.groups[2].key[0] == Value::of_timestamp(100));
    assert(result.groups[2].values[0].count == 2);

    cleanup(root);
}

void aggregate_skips_rows_with_no_number_rather_than_counting_zero() {
    auto root = test_root("aggregate_skips_rows_with_no_number_rather_than_counting_zero");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    // Departments have text object values, so a numeric aggregate has nothing to read from any of them.
    AggregateQuery text_objects;
    text_objects.selection.predicate = f.dept;
    text_objects.aggregations = {{AggregateFunction::Avg, AggregateTarget::ObjectValue},
                                 {AggregateFunction::Sum, AggregateTarget::ObjectValue},
                                 {AggregateFunction::Count, AggregateTarget::ObjectValue}};

    auto result = run_both_ways(kernel, text_objects);
    assert(result.groups.size() == 1);
    assert(result.groups[0].row_count == 4);                // the rows are there...
    assert(result.groups[0].values[2].count == 4);          // ...and count sees them...
    assert(!result.groups[0].values[0].number.has_value()); // ...but avg has nothing to average
    // A sum over nothing is null, not 0.0: reporting zero would be a claim about data never seen.
    assert(!result.groups[0].values[1].number.has_value());

    // Mixed: across every predicate, only the salary rows contribute a number, so row_count and the
    // average's denominator deliberately differ.
    AggregateQuery mixed;
    mixed.selection.statuses = {AssertionStatus::Active};
    mixed.aggregations = {{AggregateFunction::Avg, AggregateTarget::ObjectValue}};

    auto mixed_result = run_both_ways(kernel, mixed);
    assert(mixed_result.groups[0].row_count == 7);
    assert(near(*mixed_result.groups[0].values[0].number, 120000.0)); // (120k + 90k + 150k) / 3

    cleanup(root);
}

void aggregate_honors_the_full_selection_surface() {
    auto root = test_root("aggregate_honors_the_full_selection_surface");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    // A filter tree restricts what is aggregated exactly as it restricts what is returned.
    AggregateQuery filtered;
    filtered.selection = active_salaries(f);
    filtered.selection.filter = Filter::compare(FilterField::ObjectValue, CompareOp::Gte, Value::of_int64(100000));
    filtered.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue},
                             {AggregateFunction::Avg, AggregateTarget::ObjectValue}};

    auto result = run_both_ways(kernel, filtered);
    assert(result.groups[0].values[0].count == 2); // Bob's 90k is filtered out
    assert(near(*result.groups[0].values[1].number, 135000.0));

    // An observed-time window, which also exercises the observed-index candidate path.
    AggregateQuery windowed;
    windowed.selection.subject = f.alice;
    windowed.selection.observed_to = 100;
    windowed.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};

    auto windowed_result = run_both_ways(kernel, windowed);
    assert(windowed_result.groups[0].values[0].count == 3); // old salary, new salary, dept

    cleanup(root);
}

void aggregate_rejects_caller_mistakes() {
    auto root = test_root("aggregate_rejects_caller_mistakes");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    auto rejects = [&kernel](AggregateQuery query) {
        try {
            kernel.aggregate(query);
        } catch (const std::runtime_error &) {
            return true;
        }
        return false;
    };

    AggregateQuery no_aggregations;
    assert(rejects(no_aggregations));

    AggregateQuery bucket_without_width;
    bucket_without_width.group_by = {{GroupField::ObservedAtBucket, 0}};
    bucket_without_width.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};
    assert(rejects(bucket_without_width));

    AggregateQuery too_many_fields;
    too_many_fields.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};
    for (size_t i = 0; i < MAX_GROUP_BY_FIELDS + 1; ++i) {
        too_many_fields.group_by.push_back({GroupField::Subject, 0});
    }
    assert(rejects(too_many_fields));

    // Row-shaping fields mean nothing for an aggregate, so they are rejected rather than ignored.
    for (int which = 0; which < 5; ++which) {
        AggregateQuery shaped;
        shaped.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};
        if (which == 0) {
            shaped.selection.limit = 5;
        } else if (which == 1) {
            shaped.selection.offset = 5;
        } else if (which == 2) {
            shaped.selection.order = QueryOrder::ObservedAt;
        } else if (which == 3) {
            shaped.selection.newest_first = true;
        } else {
            shaped.selection.resolve_names = true;
        }
        assert(rejects(shaped));
    }

    AggregateQuery bad_version;
    bad_version.ir_version = QUERY_IR_VERSION + 1;
    bad_version.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};
    assert(rejects(bad_version));

    // A malformed filter is rejected here exactly as it is for a row query.
    AggregateQuery bad_filter;
    bad_filter.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};
    bad_filter.selection.filter = Filter::compare(FilterField::Confidence, CompareOp::Eq, Value::of_int64(1));
    assert(rejects(bad_filter));

    // Exceeding the group cap errors: half an aggregate is a wrong answer, not a smaller one.
    AggregateQuery capped;
    capped.group_by = {{GroupField::Subject, 0}};
    capped.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};
    capped.max_groups = 2;
    assert(rejects(capped)); // three subjects

    capped.max_groups = 3;
    assert(!rejects(capped)); // exactly at the cap is fine

    assert(f.alice != 0);
    cleanup(root);
}

void aggregate_is_empty_when_nothing_matches() {
    auto root = test_root("aggregate_is_empty_when_nothing_matches");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    AggregateQuery query;
    query.selection.subject = 424242; // never interned
    query.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue},
                          {AggregateFunction::Sum, AggregateTarget::ObjectValue}};

    // No rows means no groups at all, rather than one group of zero -- there is nothing to key it by.
    auto result = run_both_ways(kernel, query);
    assert(result.groups.empty());

    assert(f.alice != 0);
    cleanup(root);
}

void aggregate_survives_a_restart() {
    auto root = test_root("aggregate_survives_a_restart");

    double before = 0.0;
    {
        KnowledgeKernel kernel(StorageConfig{root});
        auto f = seed(kernel);

        AggregateQuery query;
        query.selection = active_salaries(f);
        query.aggregations = {{AggregateFunction::Avg, AggregateTarget::ObjectValue}};
        before = *run_both_ways(kernel, query).groups[0].values[0].number;
    }

    KnowledgeKernel recovered(StorageConfig{root});

    AggregateQuery query;
    query.selection.predicate = recovered.find_predicate("salary").value();
    query.selection.statuses = {AssertionStatus::Active};
    query.aggregations = {{AggregateFunction::Avg, AggregateTarget::ObjectValue}};

    assert(near(*run_both_ways(recovered, query).groups[0].values[0].number, before));

    cleanup(root);
}

} // namespace

int main() {
    aggregate_without_grouping_is_one_global_group();
    aggregate_groups_by_subject_status_and_object();
    aggregate_buckets_time_including_before_the_epoch();
    aggregate_skips_rows_with_no_number_rather_than_counting_zero();
    aggregate_honors_the_full_selection_surface();
    aggregate_rejects_caller_mistakes();
    aggregate_is_empty_when_nothing_matches();
    aggregate_survives_a_restart();

    std::cout << "All query_aggregate tests passed.\n";
    return 0;
}
