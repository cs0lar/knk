// A guided tour of the Knowledge Kernel's public API: bitemporal commit/query, supersession and
// retraction, audit/timeline history, the Phase 6 explain()/find_conflicts() read-side queries, the
// Phase 5 Catalog (name/value interning), the Phase 5 PayloadStore (documents), and recovery across a
// restart. See examples/catalog_usage.cpp for a narrower, more focused look at just the Catalog.
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kernel/knowledge_kernel.hpp"

using namespace knk;

namespace {

std::filesystem::path example_root() {
    auto path = std::filesystem::temp_directory_path() / "knowledge_kernel_demo";
    std::filesystem::remove_all(path);
    return path;
}

std::string entity_label(const KnowledgeKernel &kernel, EntityId id) {
    if (auto name = kernel.entity_name(id)) {
        return *name;
    }

    if (auto value = kernel.entity_value(id)) {
        switch (value->kind) {
        case ValueKind::Text:
            return value->text;
        case ValueKind::Int64:
            return std::to_string(value->int64_value);
        case ValueKind::Double:
            return std::to_string(value->double_value);
        case ValueKind::Bool:
            return value->bool_value ? "true" : "false";
        case ValueKind::Timestamp:
            return std::to_string(value->timestamp_value);
        }
    }

    return "entity#" + std::to_string(id);
}

std::string predicate_label(const KnowledgeKernel &kernel, PredicateId id) {
    if (auto name = kernel.predicate_name(id)) {
        return *name;
    }
    return "predicate#" + std::to_string(id);
}

std::string status_label(AssertionStatus status) {
    switch (status) {
    case AssertionStatus::Active:
        return "active";
    case AssertionStatus::Superseded:
        return "superseded";
    case AssertionStatus::Retracted:
        return "retracted";
    case AssertionStatus::Retraction:
        return "retraction-record";
    }
    return "?";
}

void print_facts(const std::string &header, const KnowledgeKernel &kernel, const std::vector<Assertion> &facts) {
    std::cout << header << "\n";

    if (facts.empty()) {
        std::cout << "  (none)\n";
        return;
    }

    for (const auto &fact : facts) {
        std::cout << "  [" << status_label(fact.status) << "] " << entity_label(kernel, fact.subject) << " "
                  << predicate_label(kernel, fact.predicate) << " " << entity_label(kernel, fact.object)
                  << " (confidence=" << fact.confidence << ", valid_to="
                  << (fact.valid_to == OPEN_ENDED ? std::string("open") : std::to_string(fact.valid_to)) << ")\n";
    }
}

void print_conflicts(const std::string &header, const KnowledgeKernel &kernel,
                     const std::vector<std::pair<Assertion, Assertion>> &conflicts) {
    std::cout << header << "\n";

    if (conflicts.empty()) {
        std::cout << "  (none)\n";
        return;
    }

    for (const auto &[a, b] : conflicts) {
        std::cout << "  #" << a.id << " " << entity_label(kernel, a.subject) << " "
                  << predicate_label(kernel, a.predicate) << " " << entity_label(kernel, a.object) << "  <>  #" << b.id
                  << " " << entity_label(kernel, b.object) << " (overlapping active claims with different objects)\n";
    }
}

std::vector<std::byte> to_bytes(const std::string &text) {
    std::vector<std::byte> bytes(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(text[i]);
    }
    return bytes;
}

} // namespace

int main() {
    auto root = example_root();

    EntityId alice = 0;
    EntityId acme = 0;
    EntityId beta = 0;
    EntityId bio_document = 0;
    AssertionId alice_at_beta = 0;
    AssertionId acme_founded_2010 = 0;
    PredicateId works_at = 0;

    {
        KnowledgeKernel kernel(StorageConfig{root});

        // --- Identities --------------------------------------------------------------
        std::cout << "== Interning identities ==\n";

        alice = kernel.intern_entity("Alice");
        acme = kernel.intern_entity("Acme Corp");
        beta = kernel.intern_entity("Beta Inc");
        works_at = kernel.intern_predicate("works_at");
        PredicateId founded_in = kernel.intern_predicate("founded_in");
        EntityId founded_2010 = kernel.intern_value(Value::of_int64(2010));

        std::cout << "  Alice -> entity#" << alice << ", works_at -> predicate#" << works_at << "\n";

        // Callers can still assign EntityIds directly instead of interning them -- Catalog only
        // manages the subset of ids a caller chooses to name/value-intern. Nothing prevents mixing
        // the two id sources, though avoiding collisions between them is the caller's
        // responsibility (see docs/storage_format.md's "Entity/predicate catalog" section).
        EntityId external_feed = 9000;
        std::cout << "  external_feed (a manually chosen id, never interned) -> entity#" << external_feed << "\n";

        // --- Commit + bitemporal queries -----------------------------------------------
        std::cout << "\n== Committing assertions ==\n";

        kernel.commit(alice, works_at, acme,
                      1672531200, // valid_from:  2023-01-01
                      1719792000, // valid_to:    2024-07-01
                      1719878400, // observed_at: 2024-07-02
                      0.95);

        alice_at_beta = kernel.commit(alice, works_at, beta,
                                      1719792000, // valid_from: 2024-07-01
                                      OPEN_ENDED, 1719878400, 0.80);

        acme_founded_2010 = kernel.commit(acme, founded_in, founded_2010, OPEN_ENDED, OPEN_ENDED, 1719878400, 1.0);

        print_facts("\ncurrent(Alice) -- only Active, open-ended facts:", kernel, kernel.current(alice));
        print_facts("\nvalid_at(Alice, 2024-01-01) -- what was true then:", kernel, kernel.valid_at(alice, 1704067200));
        print_facts("\nknown_at(Alice, 2024-07-01) -- nothing observed by then yet:", kernel,
                    kernel.known_at(alice, 1719792000));
        print_facts("\nvalid_at_known_at(Alice, 2024-01-01, 2024-07-08) -- what we now know was true then:", kernel,
                    kernel.valid_at_known_at(alice, 1704067200, 1720450412));

        // --- Supersession ------------------------------------------------------------
        std::cout << "\n== Correcting a fact via supersession ==\n";
        // The confidence recorded for "Alice works_at Beta" was optimistic; a later assertion
        // corrects it without ever mutating the original append-only record.
        AssertionId corrected_alice_at_beta =
            kernel.commit_superseding(alice, works_at, beta, 1719792000, OPEN_ENDED, 1719961200, 0.98, alice_at_beta);
        print_facts("current(Alice) after supersession -- the corrected fact replaces the old one:", kernel,
                    kernel.current(alice));

        // --- Retraction ----------------------------------------------------------------
        std::cout << "\n== Retracting an incorrect fact ==\n";
        kernel.commit_retraction(acme, founded_in, founded_2010, OPEN_ENDED, OPEN_ENDED, 1719961200, 1.0,
                                 acme_founded_2010);
        print_facts("current(Acme Corp) after retraction -- the retracted fact is gone:", kernel, kernel.current(acme));

        // --- Audit history -------------------------------------------------------------
        // Unlike current()/valid_at()/known_at(), these surface the full history, including
        // superseded, retracted, and retraction-audit records.
        std::cout << "\n== Full audit history ==\n";
        print_facts("commit_history(Alice, works_at):", kernel, kernel.commit_history(alice, works_at));
        print_facts("valid_time_timeline(Alice, works_at) -- Active facts sorted by valid_from:", kernel,
                    kernel.valid_time_timeline(alice, works_at));

        // --- Explaining a fact's lineage (Phase 6) ------------------------------------
        // explain() walks supersedes_id/retracts_id back to the root, newest-first: the corrected
        // "Alice works_at Beta" (confidence 0.98) followed by the original it replaced (0.80). This
        // is the concrete answer to "why does the kernel believe this?".
        std::cout << "\n== Explaining a fact's lineage ==\n";
        print_facts("explain(corrected 'Alice works_at Beta') -- newest correction first, down to the root:", kernel,
                    kernel.explain(corrected_alice_at_beta));

        // --- Detecting conflicts (Phase 6) --------------------------------------------
        // Two sources disagree about where Alice currently lives. Both assertions are Active and
        // open-ended, so their valid-time intervals overlap -- find_conflicts surfaces the pair.
        std::cout << "\n== Detecting conflicting active assertions ==\n";
        PredicateId lives_in = kernel.intern_predicate("lives_in");
        EntityId paris = kernel.intern_entity("Paris");
        EntityId london = kernel.intern_entity("London");
        kernel.commit(alice, lives_in, paris, 1704067200, OPEN_ENDED, 1719878400, 0.60);
        kernel.commit(alice, lives_in, london, 1704067200, OPEN_ENDED, 1719878400, 0.70);
        print_conflicts("find_conflicts(Alice, lives_in):", kernel, kernel.find_conflicts(alice, lives_in));

        // --- Documents (PayloadStore) ----------------------------------------------------
        std::cout << "\n== Interning a document ==\n";
        std::string bio_text = "Alice joined Acme Corp in 2023 and moved to Beta Inc in 2024.";
        bio_document = kernel.intern_document(to_bytes(bio_text));

        // A document's EntityId is just as usable as the object of an assertion as any interned
        // name or value.
        PredicateId has_bio = kernel.intern_predicate("has_bio");
        kernel.commit(alice, has_bio, bio_document, OPEN_ENDED, OPEN_ENDED, 1719961200, 1.0);

        auto loaded_bio = kernel.document_content(bio_document);
        std::cout << "  document_content(bio_document) round-trips " << (loaded_bio ? loaded_bio->size() : 0)
                  << " bytes\n";

        // --- The reified command layer (Phase 6) --------------------------------------
        // Every public operation above can also be issued as data through execute(), which dispatches
        // 1:1 to the mirrored method and returns a KernelResult variant. This is what lets a future
        // boundary (an agent, an MCP/HTTP/gRPC server) serialize a call instead of linking the C++ API.
        std::cout << "\n== Calling the kernel through the command layer ==\n";
        KernelResult committed =
            kernel.execute(CommitCommand{alice, works_at, acme, 1735689600, OPEN_ENDED, 1735689600, 0.70});
        std::cout << "  execute(CommitCommand{...}) -> assertion #" << std::get<AssertionId>(committed) << "\n";

        KernelResult snapshot_of_alice = kernel.execute(CurrentCommand{alice});
        print_facts("execute(CurrentCommand{Alice}) returns the same vector current(Alice) would:", kernel,
                    std::get<std::vector<Assertion>>(snapshot_of_alice));

        // Explicit, application-triggered only -- there is no automatic snapshot cadence, so this
        // never affects commit-path latency unless the application calls it itself.
        kernel.write_snapshot();
        std::cout << "\nWrote an explicit snapshot; a future startup can skip re-parsing everything before it.\n";
    }

    // --- Recovery ------------------------------------------------------------------
    std::cout << "\n== Reopening the kernel over the same storage directory ==\n";
    {
        KnowledgeKernel kernel(StorageConfig{root});

        // The catalog is authoritative and persisted, not derived from assertions.log -- "Alice"
        // resolves back to the exact same EntityId as before, without replaying a single assertion.
        auto resolved_alice = kernel.find_entity("Alice");
        std::cout << "  find_entity(\"Alice\") -> " << (resolved_alice ? std::to_string(*resolved_alice) : "not found")
                  << " (was " << alice << ")\n";

        print_facts("\ncurrent(Alice) after restart:", kernel, kernel.current(alice));

        auto loaded_bio = kernel.document_content(bio_document);
        if (loaded_bio) {
            std::string text(reinterpret_cast<const char *>(loaded_bio->data()), loaded_bio->size());
            std::cout << "  document_content(bio_document) after restart: \"" << text << "\"\n";
        }
    }

    std::filesystem::remove_all(root);
}
