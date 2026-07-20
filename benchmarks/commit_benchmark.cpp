#include <cstddef>
#include <filesystem>
#include <string>

#include "kernel/knowledge_kernel.hpp"
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
        kernel.commit(static_cast<EntityId>(i + 1), WORKS_AT, static_cast<EntityId>(i + 1'000'000), 0, OPEN_ENDED,
                      0, 1.0);
    }
    double elapsed = timer.elapsed_seconds();

    report("commit_throughput(" + std::to_string(count) + ")", static_cast<double>(count) / elapsed,
           "commits/sec");
    report("commit_throughput(" + std::to_string(count) + ") avg latency", (elapsed / static_cast<double>(count)) *
                                                                                1000.0,
           "ms/commit");

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

} // namespace

int main() {
    section("commit() -- distinct subjects");
    commit_throughput(1'000);
    commit_throughput(10'000);

    section("commit_superseding() -- same subject/predicate");
    commit_superseding_throughput(1'000);
    commit_superseding_throughput(10'000);

    return 0;
}
