// Demonstrates the Phase 5 Catalog: interning human-readable names and typed literal values into
// EntityId/PredicateId, so callers no longer have to invent and track their own name<->id mapping
// out of band (see AGENTS.md's Phase 5 "Entity/Predicate Catalog and Payload Store").
#include <filesystem>
#include <iostream>
#include <string>

#include "kernel/knowledge_kernel.hpp"

using namespace knk;

namespace {

std::filesystem::path example_root() {
    auto path = std::filesystem::temp_directory_path() / "knowledge_kernel_catalog_example";
    std::filesystem::remove_all(path);
    return path;
}

void print_current_facts(const KnowledgeKernel &kernel, EntityId subject, const std::string &subject_name) {
    std::cout << "Current facts for " << subject_name << ":\n";

    for (const auto &fact : kernel.current(subject)) {
        auto predicate_name = kernel.predicate_name(fact.predicate);
        auto object_name = kernel.entity_name(fact.object);
        auto object_value = kernel.entity_value(fact.object);

        std::cout << "  " << subject_name << " " << (predicate_name ? *predicate_name : "?") << " ";

        if (object_name) {
            std::cout << *object_name;
        } else if (object_value && object_value->kind == ValueKind::Int64) {
            std::cout << object_value->int64_value;
        } else {
            std::cout << "entity#" << fact.object;
        }

        std::cout << " (confidence " << fact.confidence << ")\n";
    }
}

} // namespace

int main() {
    auto root = example_root();

    {
        KnowledgeKernel kernel(StorageConfig{root});

        // Interning is idempotent: the same name or value always resolves to the same id, so
        // callers can freely re-intern the same subject/predicate/object across many commits
        // instead of hand-managing an id table themselves.
        EntityId alice = kernel.intern_entity("Alice");
        EntityId acme = kernel.intern_entity("Acme Corp");
        EntityId beta = kernel.intern_entity("Beta Inc");
        PredicateId works_at = kernel.intern_predicate("works_at");

        // Not every object is a named entity -- some are plain typed values. Interning
        // Value::of_int64(2010) gives "2010" a stable EntityId too, without inventing a fake name
        // for it, so it can be used as the object of an assertion exactly like Alice or Acme Corp.
        EntityId founded_2010 = kernel.intern_value(Value::of_int64(2010));
        PredicateId founded_in = kernel.intern_predicate("founded_in");

        kernel.commit(alice, works_at, acme,
                      1672531200, // valid_from: 2023-01-01
                      1719792000, // valid_to:   2024-07-01
                      1719878400, // observed_at: 2024-07-02
                      0.95);

        kernel.commit(alice, works_at, beta,
                      1719792000, // valid_from: 2024-07-01
                      OPEN_ENDED, 1719878400, 0.90);

        kernel.commit(acme, founded_in, founded_2010, OPEN_ENDED, OPEN_ENDED, 1719878400, 1.0);

        print_current_facts(kernel, alice, "Alice");
        print_current_facts(kernel, acme, "Acme Corp");

        // Re-interning "Alice" mid-session returns the same id every time -- no need to remember
        // it was already assigned earlier in this same process.
        std::cout << "\nintern_entity(\"Alice\") is idempotent: " << std::boolalpha
                  << (kernel.intern_entity("Alice") == alice) << "\n";
    }

    // The catalog is authoritative and persisted, not derived from assertions.log -- so a fresh
    // KnowledgeKernel over the same storage directory resolves "Alice" back to the exact same
    // EntityId it was given before, without replaying a single assertion.
    {
        KnowledgeKernel kernel(StorageConfig{root});

        auto alice = kernel.find_entity("Alice");
        auto works_at = kernel.find_predicate("works_at");

        std::cout << "\nAfter reopening the kernel:\n";
        std::cout << "  find_entity(\"Alice\") -> " << (alice ? std::to_string(*alice) : "not found") << "\n";
        std::cout << "  find_predicate(\"works_at\") -> " << (works_at ? std::to_string(*works_at) : "not found")
                  << "\n";

        if (alice) {
            print_current_facts(kernel, *alice, "Alice");
        }
    }

    std::filesystem::remove_all(root);
}
