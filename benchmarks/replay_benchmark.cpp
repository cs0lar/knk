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

void populate(const std::filesystem::path &root, size_t count) {
    KnowledgeKernel kernel(StorageConfig{root});
    for (size_t i = 0; i < count; ++i) {
        kernel.commit(static_cast<EntityId>(i + 1), WORKS_AT, static_cast<EntityId>(i + 1'000'000), 0, OPEN_ENDED,
                      0, 1.0);
    }
}

// Startup cost with everything trusted: commit() already wrote the checkpoint and the three index
// logs, so this constructor call takes the fast path (restore_assertion over the log tail plus one
// in-memory object-index pass), never touching apply()/IndexManager::add.
double time_trusted_index_reopen(const std::filesystem::path &root) {
    Timer timer;
    KnowledgeKernel kernel(StorageConfig{root});
    return timer.elapsed_seconds();
}

// Same log, but with the checkpoint deleted so checkpoint_matches is false regardless of the index
// files' own contents -- this is the same fallback trigger corrupt_*_falls_back_to_replay_and_self_
// heals exercises, applied deliberately here to force the full apply()-every-record path and measure
// its cost against the trusted-index reopen above.
double time_forced_full_replay(const std::filesystem::path &root) {
    std::filesystem::remove(StorageConfig{root}.checkpoint_path());

    Timer timer;
    KnowledgeKernel kernel(StorageConfig{root});
    return timer.elapsed_seconds();
}

// Same log again, but a snapshot was written first -- the fast path then skips re-parsing every
// pre-snapshot record entirely (tail_records is just whatever committed after the snapshot, here
// nothing) and loads assertions_ from the single snapshot file instead.
double time_snapshot_reopen(const std::filesystem::path &root) {
    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.write_snapshot();
    }

    Timer timer;
    KnowledgeKernel kernel(StorageConfig{root});
    return timer.elapsed_seconds();
}

void replay_comparison(size_t count) {
    auto root = fresh_root("replay_" + std::to_string(count));
    populate(root, count);

    double trusted = time_trusted_index_reopen(root);
    double full = time_forced_full_replay(root);
    double snapshot = time_snapshot_reopen(root);

    report("reopen, trusted indexes (" + std::to_string(count) + " records)", trusted * 1000.0, "ms");
    report("reopen, forced full replay (" + std::to_string(count) + " records)", full * 1000.0, "ms");
    report("reopen, after write_snapshot (" + std::to_string(count) + " records)", snapshot * 1000.0, "ms");

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    // Kept modest deliberately: replay cost is what's under test here, not commit throughput
    // (covered by commit_benchmark.cpp), and every scenario below first has to *populate* the log
    // with `count` fsync'd commits before a single reopen is timed.
    section("startup/replay cost");
    replay_comparison(2'000);
    replay_comparison(5'000);

    return 0;
}
