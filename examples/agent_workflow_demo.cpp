// A single, chronological scenario -- not a feature-by-feature tour like knowledge_kernel_demo.cpp --
// following one autonomous agent as it ingests conflicting reports about a person from two sources,
// detects and resolves the conflict citing provenance, forms and later confirms a prediction, and
// explains its reasoning end to end. Each step is labeled with the "Current North Star" question
// from AGENTS.md it answers, so this file doubles as a literal check that those questions are all
// answerable through the public API working together, not just individually.
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "kernel/knowledge_kernel.hpp"

using namespace knk;

namespace {

std::filesystem::path example_root() {
    auto path = std::filesystem::temp_directory_path() / "knowledge_kernel_agent_workflow_demo";
    std::filesystem::remove_all(path);
    return path;
}

std::string entity_label(const KnowledgeKernel &kernel, EntityId id) {
    if (auto name = kernel.entity_name(id)) {
        return *name;
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
    case AssertionStatus::Hypothesis:
        return "hypothesis";
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
        std::cout << "  #" << fact.id << " [" << status_label(fact.status) << "] " << entity_label(kernel, fact.subject)
                  << " " << predicate_label(kernel, fact.predicate) << " " << entity_label(kernel, fact.object)
                  << " (confidence=" << fact.confidence << ", observed_at=" << fact.observed_at << ")\n";
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
        std::cout << "  #" << a.id << " " << entity_label(kernel, a.object) << "  <>  #" << b.id << " "
                  << entity_label(kernel, b.object) << "\n";
    }
}

// "Which source produced this claim?" -- a single provenance lookup, printed next to the fact it
// explains.
std::string provenance_label(const KnowledgeKernel &kernel, AssertionId id) {
    auto record = kernel.provenance_for(id);
    if (!record) {
        return "(no provenance recorded)";
    }
    return "source=" + entity_label(kernel, record->source) + ", method=\"" + record->method + "\"";
}

// "Why does the kernel believe this assertion?" -- explain() gives the supersession/retraction
// chain; resolving each hop's provenance is left to the caller (per AGENTS.md's Phase 6 design
// notes), so this zips the two together the way a real caller would.
void print_lineage_with_provenance(const std::string &header, const KnowledgeKernel &kernel,
                                   const std::vector<Assertion> &chain) {
    std::cout << header << "\n";

    for (const auto &hop : chain) {
        std::cout << "  #" << hop.id << " [" << status_label(hop.status) << "] " << entity_label(kernel, hop.subject)
                  << " " << predicate_label(kernel, hop.predicate) << " " << entity_label(kernel, hop.object)
                  << " -- " << provenance_label(kernel, hop.id) << "\n";
    }
}

} // namespace

int main() {
    auto root = example_root();
    KnowledgeKernel kernel(StorageConfig{root});

    // --- Setup -----------------------------------------------------------------------
    EntityId alice = kernel.intern_entity("Alice");
    EntityId acme = kernel.intern_entity("Acme Corp");
    EntityId beta = kernel.intern_entity("Beta Inc");
    EntityId gamma = kernel.intern_entity("Gamma Startup");
    PredicateId works_at = kernel.intern_predicate("works_at");

    EntityId crm_import = kernel.intern_entity("crm_import_pipeline");
    EntityId linkedin_scraper = kernel.intern_entity("linkedin_scraper");
    EntityId resolution_agent = kernel.intern_entity("resolution_agent");
    EntityId churn_model = kernel.intern_entity("churn_prediction_model_v2");

    constexpr Timestamp JAN_1_2024 = 1704067200;
    constexpr Timestamp JAN_15_2024 = 1705276800;
    constexpr Timestamp FEB_1_2024 = 1706745600;
    constexpr Timestamp MAR_1_2024 = 1709251200;
    constexpr Timestamp JUL_1_2024 = 1719792000;
    constexpr Timestamp JUL_2_2024 = 1719878400;
    constexpr Timestamp SEP_1_2024 = 1725148800;
    constexpr Timestamp JAN_1_2026 = 1767225600;
    constexpr Timestamp JUN_1_2025 = 1748736000;

    // --- Two sources disagree -----------------------------------------------------------
    std::cout << "== A CRM sync reports Alice's employer ==\n";
    AssertionId crm_acme = kernel.commit(alice, works_at, acme, JAN_1_2024, OPEN_ENDED, JAN_15_2024, 0.75);
    kernel.record_provenance(crm_acme, crm_import, JAN_15_2024, "daily_crm_sync");
    print_facts("North Star: \"What do we currently know about Alice?\" -- current(Alice):", kernel,
                kernel.current(alice));
    print_facts("North Star: \"What did we know about Alice as of 2024-02-01?\" -- known_at(Alice, 2024-02-01), "
                "captured now while the CRM claim is still the only, unresolved record:",
                kernel, kernel.known_at(alice, FEB_1_2024));

    std::cout << "\n== A LinkedIn scrape reports a different employer ==\n";
    AssertionId linkedin_beta = kernel.commit(alice, works_at, beta, JAN_1_2024, OPEN_ENDED, MAR_1_2024, 0.65);
    kernel.record_provenance(linkedin_beta, linkedin_scraper, MAR_1_2024, "profile_scrape");
    print_facts("current(Alice) now holds two disagreeing active facts:", kernel, kernel.current(alice));

    // --- Detecting and explaining the conflict --------------------------------------------
    std::cout << "\nNorth Star: \"Which assertions conflict?\" -- find_conflicts(Alice, works_at):\n";
    auto conflicts = kernel.find_conflicts(alice, works_at);
    print_conflicts("", kernel, conflicts);

    std::cout << "\nNorth Star: \"Which source produced this claim?\" -- resolving provenance for each side:\n";
    for (const auto &[a, b] : conflicts) {
        std::cout << "  #" << a.id << " (" << entity_label(kernel, a.object) << "): " << provenance_label(kernel, a.id)
                  << "\n";
        std::cout << "  #" << b.id << " (" << entity_label(kernel, b.object) << "): " << provenance_label(kernel, b.id)
                  << "\n";
    }

    // --- Resolving the conflict ----------------------------------------------------------
    // The agent trusts the fresher LinkedIn report over the stale CRM one, so it corrects the
    // record via ordinary commit_superseding -- the same mechanism a source correcting its own
    // earlier claim would use, just invoked by a resolving agent instead. The now-redundant
    // LinkedIn claim is retracted rather than left to coexist as a second active fact with the
    // same object (which find_conflicts would not flag as a conflict, but would still be
    // redundant clutter in current()).
    std::cout << "\n== Resolving the conflict ==\n";
    AssertionId corrected_beta =
        kernel.commit_superseding(alice, works_at, beta, JAN_1_2024, OPEN_ENDED, JUL_2_2024, 0.92, crm_acme);
    kernel.record_provenance(corrected_beta, resolution_agent, JUL_2_2024, "conflict_resolution:linkedin_more_recent");

    AssertionId retract_linkedin_duplicate = kernel.commit_retraction(alice, works_at, beta, JAN_1_2024, OPEN_ENDED,
                                                                       JUL_2_2024, 0.65, linkedin_beta);
    kernel.record_provenance(retract_linkedin_duplicate, resolution_agent, JUL_2_2024, "merged_into_resolved_claim");

    print_facts("current(Alice) after resolution -- exactly one active fact:", kernel, kernel.current(alice));

    // "Which assertion superseded this one?" and "Why does the kernel believe this assertion?"
    // are the same walk: explain() gives the chain, provenance_for() gives the "who/how" per hop.
    print_lineage_with_provenance(
        "\nNorth Star: \"Why does the kernel believe this / which assertion superseded this one?\" -- "
        "explain(corrected Beta claim), newest first:",
        kernel, kernel.explain(corrected_beta));

    // --- Revisiting belief over both time axes --------------------------------------------
    std::cout << "\n== What we believed, and when we believed it ==\n";
    print_facts("North Star: \"What was true on 2024-01-01 according to what we know now?\" -- "
                "valid_at_known_at(Alice, 2024-01-01, 2024-07-02):",
                kernel, kernel.valid_at_known_at(alice, JAN_1_2024, JUL_2_2024));

    // A real subtlety, worth surfacing rather than glossing over: known_at/valid_at_known_at only
    // ever return assertions that are *currently* Active -- they are not a true point-in-time
    // reconstruction of what current()/valid_at() would have returned back then. Re-running the
    // exact same known_at(Alice, 2024-02-01) call from right after the CRM sync above, now that the
    // CRM claim has been superseded, demonstrates this: the historical observed-time cutoff hasn't
    // changed, but the result has, because the underlying record's status has.
    std::cout << "\nRe-running known_at(Alice, 2024-02-01) from earlier, now that the CRM claim it returned has "
                 "been superseded:\n";
    print_facts("known_at(Alice, 2024-02-01) is now empty -- a later correction removes a record from these "
                "results even for a historical cutoff from before the correction happened; this method answers "
                "\"of what's been observed by t, what do we still currently believe,\" not \"what did we believe "
                "back then\":",
                kernel, kernel.known_at(alice, FEB_1_2024));

    print_facts("\nNorth Star: \"What changed since 2024-07-01?\" -- changes_since(2024-07-01), kernel-wide and "
                "status-agnostic (unlike commit_history, this doesn't require already knowing which subject/"
                "predicate to ask about):",
                kernel, kernel.changes_since(JUL_1_2024));

    // --- Forming a prediction --------------------------------------------------------------
    std::cout << "\n== An external model predicts a future change ==\n";
    AssertionId hypothesis = kernel.commit_hypothesis(alice, works_at, gamma, JAN_1_2026, OPEN_ENDED, SEP_1_2024,
                                                       0.55, churn_model, SEP_1_2024, "churn_prediction_model_v2");
    print_facts("North Star: \"What does the kernel currently predict about Alice?\" -- hypotheses_for(Alice):",
                kernel, kernel.hypotheses_for(alice));

    std::cout << "\nNorth Star: \"What existing evidence supports or contradicts this hypothesis?\" -- "
              << "immediately after forming it, nothing yet does: hypotheses_for(Alice) has no confirming or "
                 "contradicting record beside the prediction itself.\n";

    // --- Confirming the prediction with real evidence --------------------------------------
    // A later, independent CRM sync confirms the predicted move actually happened. Promoting a
    // hypothesis is not a separate primitive -- it's ordinary commit_superseding against the
    // hypothesis's id, same as correcting any other assertion.
    std::cout << "\n== Independent evidence confirms the prediction ==\n";
    AssertionId gamma_confirmed =
        kernel.commit_superseding(alice, works_at, gamma, JUN_1_2025, OPEN_ENDED, JUN_1_2025, 0.97, hypothesis);
    kernel.record_provenance(gamma_confirmed, crm_import, JUN_1_2025, "daily_crm_sync_confirms_prediction");
    print_facts("hypotheses_for(Alice) after confirmation -- the promoted hypothesis is no longer an open "
                "prediction:",
                kernel, kernel.hypotheses_for(alice));

    // Promoting the hypothesis creates a fresh problem: the still-active Beta claim from the
    // earlier resolution and the newly-promoted Gamma claim are now both active for the same
    // predicate -- find_conflicts catches this exactly like the original CRM/LinkedIn disagreement,
    // demonstrating the kernel needs no hypothesis-specific conflict logic.
    print_conflicts("\nfind_conflicts(Alice, works_at) after promotion -- the confirmed prediction now disagrees "
                    "with the still-active Beta claim:",
                    kernel, kernel.find_conflicts(alice, works_at));

    AssertionId retract_stale_beta =
        kernel.commit_retraction(alice, works_at, beta, JAN_1_2024, OPEN_ENDED, JUN_1_2025, 0.92, corrected_beta);
    kernel.record_provenance(retract_stale_beta, resolution_agent, JUN_1_2025, "superseded_by_confirmed_prediction");
    print_facts("\ncurrent(Alice) after retracting the stale claim -- exactly one active fact again:", kernel,
                kernel.current(alice));

    // current(subject) only ever answers in the subject -> object direction. An application built on
    // top of the kernel -- a CRM answering "who works at Gamma Startup," or a retriever pulling every
    // WORKS_AT edge to build a subgraph for an agent -- needs the reverse and kernel-wide directions
    // too, without already knowing which subjects to ask about.
    print_facts("\ncurrent_by_object(Gamma Startup) -- reverse-direction lookup, \"who currently works at Gamma "
                "Startup\":",
                kernel, kernel.current_by_object(gamma));
    print_facts("current_by_predicate(works_at) -- kernel-wide lookup, every currently-active WORKS_AT "
                "relationship, any subject:",
                kernel, kernel.current_by_predicate(works_at));

    print_lineage_with_provenance(
        "\nNorth Star: \"What evidence supports this hypothesis?\" -- explain(confirmed Gamma claim) traces "
        "straight back to the original prediction it confirms:",
        kernel, kernel.explain(gamma_confirmed));

    // --- The full story, preserved forever ---------------------------------------------------
    print_facts("\nFull audit trail -- commit_history(Alice, works_at), every step of this scenario preserved:",
                kernel, kernel.commit_history(alice, works_at));

    std::filesystem::remove_all(root);
}
