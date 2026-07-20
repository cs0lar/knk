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

    std::filesystem::remove_all(root);
    return 0;
}
