#include <cstddef>
#include <filesystem>
#include <string>

#include "kernel/aggregate.hpp"
#include "kernel/column_store.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/query.hpp"
#include "kernel/query_plan.hpp"
#include "kernel/storage_config.hpp"
#include "kernel/storage_engine.hpp"
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

// The same queries with index selection disabled, measured both ways: over the columnar store (Phase
// 15's vectorized passes) and over the row layout. Both are measured in the same run on purpose --
// comparing against a number recorded on another day measures the machine as much as the code.
void ir_forced_scan_latency(const KnowledgeKernel &kernel, size_t queries) {
    Timer columnar_timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query = current_shaped(static_cast<EntityId>(i % SUBJECT_COUNT + 1));
        query.force_scan = true;
        kernel.query(query);
    }
    report("query{...} scan, columnar", columnar_timer.elapsed_seconds() / static_cast<double>(queries) * 1000.0,
           "ms/query");

    Timer row_timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query = current_shaped(static_cast<EntityId>(i % SUBJECT_COUNT + 1));
        query.force_scan = true;
        query.force_row_scan = true;
        kernel.query(query);
    }
    report("query{...} scan, rows", row_timer.elapsed_seconds() / static_cast<double>(queries) * 1000.0, "ms/query");
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

    // A filter tree is evaluated per surviving row either way, so this pair isolates what the columnar
    // passes do *not* help with -- the filter here is the whole predicate, and nothing narrows it first.
    Timer selective_rows_timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query;
        query.force_row_scan = true;
        query.filter = Filter::compare(FilterField::Confidence, CompareOp::Lt, Value::of_double(0.5));
        kernel.query(query);
    }
    report("query{filter: confidence < 0.5} rows",
           selective_rows_timer.elapsed_seconds() / static_cast<double>(queries) * 1000.0, "ms/query");

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

// --- the columnar substrate (Phase 14) -------------------------------------------
//
// Nothing queries columns yet -- Phase 15 is what changes execution -- so these numbers exist to be
// beaten: they establish how fast the same predicate is over one mapped column versus over the
// row-of-structs layout the engine scans today, and what the manifest-checksum design costs at open.
//
// The engine is opened read-only (Phase 13), which is also the only reason it can coexist with the
// KnowledgeKernel holding the writer lock a few lines up.
void columnar_substrate(const std::filesystem::path &root, size_t rounds) {
    StorageEngine engine(StorageConfig{root}, OpenMode::ReadOnly);

    auto columns = engine.map_columns();
    if (columns.empty()) {
        report("columns unavailable -- skipped", 0.0, "ms");
        return;
    }

    // One column, one contiguous byte range: 8 bytes touched per row instead of striding over an
    // 88-byte record for the one field the predicate reads.
    Timer column_timer;
    size_t column_matches = 0;
    for (size_t round = 0; round < rounds; ++round) {
        for (size_t i = 0; i < columns.confidence.size(); ++i) {
            if (columns.confidence[i] >= 0.5) {
                ++column_matches;
            }
        }
    }
    double column_elapsed = column_timer.elapsed_seconds();
    report("columns: count(confidence >= 0.5)", column_elapsed / static_cast<double>(rounds) * 1000.0, "ms/scan");

    // The same predicate over the row layout, which is what the query engine scans today.
    auto rows = engine.load_assertions();
    Timer row_timer;
    size_t row_matches = 0;
    for (size_t round = 0; round < rounds; ++round) {
        for (const auto &assertion : rows) {
            if (assertion.confidence >= 0.5) {
                ++row_matches;
            }
        }
    }
    double row_elapsed = row_timer.elapsed_seconds();
    report("rows:    count(confidence >= 0.5)", row_elapsed / static_cast<double>(rounds) * 1000.0, "ms/scan");

    if (column_matches != row_matches) {
        report("MISMATCH between column and row scan", static_cast<double>(column_matches), "matches");
    }

    // What verifying the manifest costs, since that is the price paid for columns having no per-row
    // checksum: an O(rows) read every time a root is opened read-write.
    Timer verify_timer;
    for (size_t round = 0; round < rounds; ++round) {
        engine.verify_columns();
    }
    report("columns: verify (manifest checksums)",
           verify_timer.elapsed_seconds() / static_cast<double>(rounds) * 1000.0, "ms/open");
}

// --- inputs to the cost model (Phase 16) -----------------------------------------
//
// The planner compares candidate sources on modelled cost, and the constants in that model have to come
// from somewhere. These four measurements are that somewhere: two index lookups differing only in how
// many candidate rows they yield (which separates fixed lookup overhead from per-candidate cost), and
// the two scans.
void planner_cost_inputs(const KnowledgeKernel &kernel, size_t queries) {
    // ~2 candidate rows: one subject, both of its assertions.
    Timer small_timer;
    for (size_t i = 0; i < queries; ++i) {
        kernel.query(current_shaped(static_cast<EntityId>(i % SUBJECT_COUNT + 1)));
    }
    double small = small_timer.elapsed_seconds() / static_cast<double>(queries);
    report("index path, ~2 candidates", small * 1e6, "us/query");

    // ~SUBJECT_COUNT candidate rows: every WORKS_AT assertion, through the predicate current index.
    Timer large_timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query;
        query.predicate = WORKS_AT;
        query.statuses = {AssertionStatus::Active};
        query.open_ended_only = true;
        query.limit = 1; // page one row, so materialization is not what is being measured
        kernel.query(query);
    }
    double large = large_timer.elapsed_seconds() / static_cast<double>(queries);
    report("index path, ~5000 candidates", large * 1e6, "us/query");

    double per_candidate = (large - small) / static_cast<double>(SUBJECT_COUNT - 2);
    report("  => per candidate row", per_candidate * 1e9, "ns");

    Timer columnar_timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query = current_shaped(static_cast<EntityId>(i % SUBJECT_COUNT + 1));
        query.force_scan = true;
        kernel.query(query);
    }
    double columnar = columnar_timer.elapsed_seconds() / static_cast<double>(queries);
    report("  => per row, columnar scan", columnar / (2.0 * static_cast<double>(SUBJECT_COUNT)) * 1e9, "ns");

    Timer row_timer;
    for (size_t i = 0; i < queries; ++i) {
        Query query = current_shaped(static_cast<EntityId>(i % SUBJECT_COUNT + 1));
        query.force_scan = true;
        query.force_row_scan = true;
        kernel.query(query);
    }
    double rows = row_timer.elapsed_seconds() / static_cast<double>(queries);
    report("  => per row, row scan", rows / (2.0 * static_cast<double>(SUBJECT_COUNT)) * 1e9, "ns");
}

// --- a corpus too large to cache (Phase 16) --------------------------------------
//
// Every number above this point is measured on 10,000 assertions, which is ~880 KB of records and ~80 KB
// of one column -- both cache-resident, so they measure instruction count rather than memory traffic.
// Phase 15 flagged that as the reason its 1.7x scan figure understated the layout, and as a risk for
// calibrating a planner. This section exists to answer that: 250,000 assertions is ~22 MB of records
// against ~2 MB of one column, which no longer fits.
constexpr size_t LARGE_CORPUS = 250'000;

void large_corpus_scans(size_t queries) {
    auto root = fresh_root("large_corpus");
    KnowledgeKernel kernel(StorageConfig{root});

    for (size_t written = 0; written < LARGE_CORPUS; written += 10'000) {
        std::vector<PendingAssertion> batch;
        batch.reserve(10'000);
        for (size_t i = 0; i < 10'000; ++i) {
            size_t row = written + i;
            batch.push_back({static_cast<EntityId>(row % 50'000 + 1), static_cast<PredicateId>(row % 4 + 1),
                             static_cast<EntityId>(1'000'000 + row % 100), 0, OPEN_ENDED, static_cast<Timestamp>(row),
                             1.0});
        }
        kernel.commit_batch(batch);
    }

    Query columnar;
    columnar.statuses = {AssertionStatus::Active};
    columnar.open_ended_only = true;
    columnar.predicate = 1;
    columnar.force_scan = true;
    columnar.limit = 1;

    Timer columnar_timer;
    for (size_t i = 0; i < queries; ++i) {
        kernel.query(columnar);
    }
    double columnar_elapsed = columnar_timer.elapsed_seconds() / static_cast<double>(queries);
    report("large: scan, columnar", columnar_elapsed * 1000.0, "ms/query");
    report("  => per row", columnar_elapsed / static_cast<double>(LARGE_CORPUS) * 1e9, "ns");

    Query rows = columnar;
    rows.force_row_scan = true;

    Timer row_timer;
    for (size_t i = 0; i < queries; ++i) {
        kernel.query(rows);
    }
    double row_elapsed = row_timer.elapsed_seconds() / static_cast<double>(queries);
    report("large: scan, rows", row_elapsed * 1000.0, "ms/query");
    report("  => per row", row_elapsed / static_cast<double>(LARGE_CORPUS) * 1e9, "ns");

    // What the planner decides at this size -- and then the index it declined, actually timed, so the
    // decision is validated rather than asserted. current_by_predicate always uses that index, which
    // makes it a direct measurement of the path the planner rejected.
    Query planned = columnar;
    planned.force_scan = false;
    auto plan = kernel.explain_query(planned);
    report(std::string("large: planner chose ") + plan_source_name(plan.chosen), plan.estimated_cost, "cost units");

    Timer index_timer;
    for (size_t i = 0; i < queries; ++i) {
        kernel.current_by_predicate(1);
    }
    double index_elapsed = index_timer.elapsed_seconds() / static_cast<double>(queries);
    report("large: the declined index path", index_elapsed * 1000.0, "ms/query");

    std::filesystem::remove_all(root);
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

    section("columnar substrate over the same corpus");
    columnar_substrate(root, 20);

    section("cost model inputs (Phase 16)");
    planner_cost_inputs(kernel, 500);

    section("a corpus too large to cache (" + std::to_string(LARGE_CORPUS) + " assertions)");
    large_corpus_scans(20);

    std::filesystem::remove_all(root);
    return 0;
}
