#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "kernel/knowledge_kernel.hpp"
#include "kernel/provenance_log.hpp"
#include "kernel/storage_config.hpp"
#include "kernel/time.hpp"

#include "benchmark_harness.hpp"

using namespace knk;
using namespace knk::benchmark;

namespace {

constexpr PredicateId WORKS_AT = 1;

std::filesystem::path fresh_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("knk_benchmark_" + name);
    std::filesystem::remove_all(path);
    return path;
}

// Sequential commit() throughput: every commit is a distinct subject/object pair, so this measures
// the append/index/checkpoint path itself (dominated by the fsync-before-return durability
// guarantee), not supersession or index-collision bookkeeping.
void commit_throughput(size_t count) {
    auto root = fresh_root("commit_throughput_" + std::to_string(count));
    KnowledgeKernel kernel(StorageConfig{root});

    Timer timer;
    for (size_t i = 0; i < count; ++i) {
        kernel.commit(static_cast<EntityId>(i + 1), WORKS_AT, static_cast<EntityId>(i + 1'000'000), 0, OPEN_ENDED, 0,
                      1.0);
    }
    double elapsed = timer.elapsed_seconds();

    report("commit_throughput(" + std::to_string(count) + ")", static_cast<double>(count) / elapsed, "commits/sec");
    report("commit_throughput(" + std::to_string(count) + ") avg latency",
           (elapsed / static_cast<double>(count)) * 1000.0, "ms/commit");

    std::filesystem::remove_all(root);
}

// Same shape, but every commit lands on the same subject/predicate, so commit_superseding also
// exercises mark_superseded's current-index removal on every call, not just an append.
void commit_superseding_throughput(size_t count) {
    auto root = fresh_root("commit_superseding_throughput_" + std::to_string(count));
    KnowledgeKernel kernel(StorageConfig{root});

    constexpr EntityId subject = 1;
    constexpr EntityId object = 2;
    AssertionId previous = kernel.commit(subject, WORKS_AT, object, 0, OPEN_ENDED, 0, 1.0);

    Timer timer;
    for (size_t i = 0; i < count; ++i) {
        previous = kernel.commit_superseding(subject, WORKS_AT, object, 0, OPEN_ENDED, 0, 1.0, previous);
    }
    double elapsed = timer.elapsed_seconds();

    report("commit_superseding_throughput(" + std::to_string(count) + ")", static_cast<double>(count) / elapsed,
           "commits/sec");

    std::filesystem::remove_all(root);
}

// The same work commit_throughput does -- count distinct subjects, one assertion each -- issued as a
// single commit_batch instead of count commit() calls. Directly comparable to commit_throughput at
// the same count, which is the point: the difference is entirely the collapsed durability boundary
// (one fsync per log for the batch, versus one per log per assertion), since the records written and
// the indexes updated are identical.
void commit_batch_throughput(size_t count) {
    auto root = fresh_root("commit_batch_throughput_" + std::to_string(count));
    KnowledgeKernel kernel(StorageConfig{root});

    std::vector<PendingAssertion> entries;
    entries.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        entries.push_back(PendingAssertion{static_cast<EntityId>(i + 1), WORKS_AT, static_cast<EntityId>(i + 1'000'000),
                                           0, OPEN_ENDED, 0, 1.0});
    }

    Timer timer;
    kernel.commit_batch(entries);
    double elapsed = timer.elapsed_seconds();

    report("commit_batch_throughput(" + std::to_string(count) + ")", static_cast<double>(count) / elapsed,
           "commits/sec");
    report("commit_batch_throughput(" + std::to_string(count) + ") whole batch", elapsed * 1000.0, "ms/batch");

    std::filesystem::remove_all(root);
}

// Attaching provenance to a batch, the two ways: one record_provenance call per assertion versus one
// record_provenance_batch for all of them. Measured against the same committed batch, so the only
// difference is how many fsyncs the provenance log takes. This is the one place per-assertion fsync
// cost survived on the batch path, which is why it is worth measuring next to the batch itself.
void provenance_for_a_batch(size_t count) {
    auto root = fresh_root("provenance_for_a_batch_" + std::to_string(count));
    KnowledgeKernel kernel(StorageConfig{root});

    std::vector<PendingAssertion> entries;
    entries.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        entries.push_back(PendingAssertion{static_cast<EntityId>(i + 1), WORKS_AT, static_cast<EntityId>(i + 1'000'000),
                                           0, OPEN_ENDED, 0, 1.0});
    }

    auto ids = kernel.commit_batch(entries);
    EntityId source = kernel.intern_entity("migration_job");

    Timer per_record_timer;
    for (AssertionId id : ids) {
        kernel.record_provenance(id, source, 0, "restated_from_legacy_field");
    }
    double per_record_elapsed = per_record_timer.elapsed_seconds();

    // Re-recording the same assertions: provenance is last-writer-wins per assertion, so this writes
    // the same number of records and leaves the same final state, making the two directly comparable.
    std::vector<ProvenanceRecord> records;
    records.reserve(count);
    for (AssertionId id : ids) {
        records.push_back(ProvenanceRecord{id, source, 0, "restated_from_legacy_field"});
    }

    Timer batch_timer;
    kernel.record_provenance_batch(records);
    double batch_elapsed = batch_timer.elapsed_seconds();

    report("record_provenance x" + std::to_string(count), per_record_elapsed * 1000.0, "ms");
    report("record_provenance_batch(" + std::to_string(count) + ")", batch_elapsed * 1000.0, "ms");

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    section("commit() -- distinct subjects");
    commit_throughput(1'000);
    commit_throughput(10'000);

    section("commit_superseding() -- same subject/predicate");
    commit_superseding_throughput(1'000);
    commit_superseding_throughput(10'000);

    // Same workload as the first section, one call instead of N -- MAX_BATCH_SIZE caps a single
    // batch, so 10'000 is the largest directly comparable pair.
    section("commit_batch() -- distinct subjects, one durability boundary");
    commit_batch_throughput(1'000);
    commit_batch_throughput(10'000);

    section("provenance for a committed batch -- per record vs. batched");
    provenance_for_a_batch(1'000);
    provenance_for_a_batch(10'000);

    return 0;
}
