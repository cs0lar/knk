#include <cstddef>
#include <filesystem>
#include <string>

#include "kernel/aggregate.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/query.hpp"
#include "kernel/storage_config.hpp"
#include "kernel/time.hpp"

#include "benchmark_harness.hpp"

using namespace knk;
using namespace knk::benchmark;

namespace {

constexpr PredicateId WORKS_AT = 1;
constexpr PredicateId LIVES_IN = 2;
constexpr size_t SUBJECT_COUNT = 5'000;

std::filesystem::path fresh_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("knk_benchmark_" + name);
    std::filesystem::remove_all(path);
    return path;
}

// Populates one WORKS_AT and one LIVES_IN assertion per subject, chaining subjects into each
// other's object slot (subject i's LIVES_IN object is subject i+1) so neighbors() has real edges to
// traverse instead of dead ends. Not timed -- this is fixture setup, not the measured hot path.
KnowledgeKernel populate(const std::filesystem::path &root) {
    KnowledgeKernel kernel(StorageConfig{root});
    for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
        EntityId subject = static_cast<EntityId>(i + 1);
        EntityId employer = static_cast<EntityId>(1'000'000 + (i % 100));
        EntityId next_subject = static_cast<EntityId>((i + 1) % SUBJECT_COUNT + 1);
        kernel.commit(subject, WORKS_AT, employer, 0, OPEN_ENDED, 0, 1.0);
        kernel.commit(subject, LIVES_IN, next_subject, 0, OPEN_ENDED, 0, 1.0);
    }
    return kernel;
}

void current_throughput(const KnowledgeKernel &kernel, size_t rounds) {
    Timer timer;
    size_t total_queries = 0;
    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
            auto results = kernel.current(static_cast<EntityId>(i + 1));
            total_queries += results.size();
        }
    }
    double elapsed = timer.elapsed_seconds();
    double query_count = static_cast<double>(rounds * SUBJECT_COUNT);
    report("current(subject)", query_count / elapsed, "queries/sec");
}

void valid_at_throughput(const KnowledgeKernel &kernel, size_t rounds) {
    Timer timer;
    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
            kernel.valid_at(static_cast<EntityId>(i + 1), 0);
        }
    }
    double elapsed = timer.elapsed_seconds();
    double query_count = static_cast<double>(rounds * SUBJECT_COUNT);
    report("valid_at(subject, t)", query_count / elapsed, "queries/sec");
}

void known_at_throughput(const KnowledgeKernel &kernel, size_t rounds) {
    Timer timer;
    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
            kernel.known_at(static_cast<EntityId>(i + 1), 0);
        }
    }
    double elapsed = timer.elapsed_seconds();
    double query_count = static_cast<double>(rounds * SUBJECT_COUNT);
    report("known_at(subject, t)", query_count / elapsed, "queries/sec");
}

void neighbors_throughput(const KnowledgeKernel &kernel, size_t rounds) {
    Timer timer;
    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
            kernel.neighbors(static_cast<EntityId>(i + 1), 2);
        }
    }
    double elapsed = timer.elapsed_seconds();
    double query_count = static_cast<double>(rounds * SUBJECT_COUNT);
    report("neighbors(subject, max_hops=2)", query_count / elapsed, "queries/sec");
}

// --- IR query paths (Phase 11) ---------------------------------------------------
//
// The same current-shaped question the methods above answer, asked through the query IR, plus the
// paths that only the IR has: a filter tree, an observed-time window, and name resolution. Reported as
// ms/query where a path is inherently a scan, since throughput numbers there say more about the corpus
// size than the engine.

Query current_shaped(EntityId subject) {
    Query query;
    query.subject = subject;
    query.statuses = {AssertionStatus::Active};
    query.open_ended_only = true;
    return query;
}

void ir_current_equivalent_throughput(const KnowledgeKernel &kernel, size_t rounds) {
    Timer timer;
    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
            kernel.query(current_shaped(static_cast<EntityId>(i + 1)));
        }
    }
    double elapsed = timer.elapsed_seconds();
    report("query{subject,Active,open_ended}", static_cast<double>(rounds * SUBJECT_COUNT) / elapsed, "queries/sec");
}

// The same queries with index selection disabled, which is what the differential tests compare against
// for correctness -- here it is the cost side of that same comparison.
void ir_forced_scan_latency(const KnowledgeKernel &kernel, size_t queries) {
    Timer timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query = current_shaped(static_cast<EntityId>(i % SUBJECT_COUNT + 1));
        query.force_scan = true;
        kernel.query(query);
    }
    double elapsed = timer.elapsed_seconds();
    report("query{...} force_scan", elapsed / static_cast<double>(queries) * 1000.0, "ms/query");
}

void ir_observed_window_throughput(const KnowledgeKernel &kernel, size_t rounds) {
    Timer timer;
    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
            Query query;
            query.subject = static_cast<EntityId>(i + 1);
            query.observed_to = 0; // every row was observed at 0, so this selects via the observed index
            kernel.query(query);
        }
    }
    double elapsed = timer.elapsed_seconds();
    report("query{subject,observed_to}", static_cast<double>(rounds * SUBJECT_COUNT) / elapsed, "queries/sec");
}

void ir_resolve_names_throughput(const KnowledgeKernel &kernel, size_t rounds) {
    Timer timer;
    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
            Query query = current_shaped(static_cast<EntityId>(i + 1));
            query.resolve_names = true;
            kernel.query(query);
        }
    }
    double elapsed = timer.elapsed_seconds();
    report("query{...} resolve_names", static_cast<double>(rounds * SUBJECT_COUNT) / elapsed, "queries/sec");
}

// Two filter shapes with the same scan cost but opposite result sizes, which separates the cost of
// evaluating a filter from the cost of materializing what it matched.
void ir_filter_latency(const KnowledgeKernel &kernel, size_t queries) {
    Timer selective_timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query;
        query.filter = Filter::compare(FilterField::Confidence, CompareOp::Lt, Value::of_double(0.5));
        kernel.query(query);
    }
    double selective = selective_timer.elapsed_seconds();
    report("query{filter: confidence < 0.5} (matches none)", selective / static_cast<double>(queries) * 1000.0,
           "ms/query");

    Timer broad_timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query;
        query.filter = Filter::compare(FilterField::Confidence, CompareOp::Gte, Value::of_double(0.5));
        kernel.query(query);
    }
    double broad = broad_timer.elapsed_seconds();
    report("query{filter: confidence >= 0.5} (matches all)", broad / static_cast<double>(queries) * 1000.0, "ms/query");
}

// --- aggregation (Phase 12) ------------------------------------------------------
//
// Aggregates fold rows as they are visited rather than collecting them, so the interesting comparison
// is against the row query that would have had to materialize the same rows.

void aggregate_latency(const KnowledgeKernel &kernel, size_t queries) {
    Timer global_timer;
    for (size_t i = 0; i < queries; ++i) {
        AggregateQuery query;
        query.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};
        kernel.aggregate(query);
    }
    report("aggregate{count, no grouping}", global_timer.elapsed_seconds() / static_cast<double>(queries) * 1000.0,
           "ms/query");

    Timer grouped_timer;
    for (size_t i = 0; i < queries; ++i) {
        AggregateQuery query;
        query.group_by = {{GroupField::Predicate, 0}};
        query.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue},
                              {AggregateFunction::Avg, AggregateTarget::Confidence}};
        kernel.aggregate(query);
    }
    report("aggregate{count+avg, by predicate}",
           grouped_timer.elapsed_seconds() / static_cast<double>(queries) * 1000.0, "ms/query");

    // One group per subject: 5,000 groups from 10,000 rows, which is where the ordered group map and
    // the key construction per row start to show up.
    Timer wide_timer;
    for (size_t i = 0; i < queries; ++i) {
        AggregateQuery query;
        query.group_by = {{GroupField::Subject, 0}};
        query.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue}};
        kernel.aggregate(query);
    }
    report("aggregate{count, by subject (5000 groups)}",
           wide_timer.elapsed_seconds() / static_cast<double>(queries) * 1000.0, "ms/query");
}

} // namespace

int main() {
    auto root = fresh_root("query_benchmark");
    KnowledgeKernel kernel = populate(root);

    section("query throughput over " + std::to_string(SUBJECT_COUNT) + " subjects");
    constexpr size_t rounds = 5;
    current_throughput(kernel, rounds);
    valid_at_throughput(kernel, rounds);
    known_at_throughput(kernel, rounds);
    neighbors_throughput(kernel, rounds);

    section("query IR over the same " + std::to_string(SUBJECT_COUNT) + " subjects");
    ir_current_equivalent_throughput(kernel, rounds);
    ir_observed_window_throughput(kernel, rounds);
    ir_resolve_names_throughput(kernel, rounds);
    ir_forced_scan_latency(kernel, 200);
    ir_filter_latency(kernel, 200);

    section("aggregation over the same corpus");
    aggregate_latency(kernel, 200);

    std::filesystem::remove_all(root);
    return 0;
}
