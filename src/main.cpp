#include <iostream>

#include "kernel/index_manager.hpp"
#include "kernel/knowledge_kernel.hpp"

using namespace knk;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("knowledge_kernel_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

int main(int argc, char const *argv[]) {
    auto root = test_root("main_example");
    KnowledgeKernel kernel(StorageConfig{root});

    EntityId alice = 1;
    EntityId acme = 100;
    EntityId beta = 200;

    PredicateId works_at = 10;

    kernel.commit(alice, works_at, acme,
                  1672531200, // 2023-01-01
                  1719792000, // 2024-07-01
                  1719878400, // observed 2024-07-02
                  0.95);

    kernel.commit(alice, works_at, beta,
                  1719792000, // 2024-07-01
                  OPEN_ENDED, 1719878400, 0.90);

    auto current = kernel.current(alice);

    std::cout << "Current facts for Alice\n";
    for (const auto &fact : current) {
        std::cout << "subject=" << fact.subject << " predicate=" << fact.predicate << " object=" << fact.object
                  << " confidence=" << fact.confidence << "\n";
    }

    auto historical = kernel.valid_at(alice, 1704067200);

    std::cout << "\nFacts valid on 2024-01-01:\n";
    for (const auto &fact : historical) {
        std::cout << "subject=" << fact.subject << " predicate=" << fact.predicate << " object=" << fact.object << "\n";
    }

    cleanup(root);
}