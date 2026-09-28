// Phase 10's correctness gate: every existing KnowledgeKernel query method, re-expressed as a Query,
// must return exactly the same assertions. That is what makes the IR an additional reach over the same
// semantics rather than a second, subtly different interpretation of them -- and it is asserted here
// rather than argued in review.
//
// Two comparison styles, deliberately different:
//
//  * As sets (sorted by id) for current/valid_at/known_at/hypotheses_for/current_by_*, because those
//    methods iterate unordered containers and their order is genuinely unspecified -- it can differ
//    between runs. The IR, by contrast, always has one deterministic order.
//  * As exact sequences for assertions_for_subject and changes_since, whose orders *are* specified
//    (subject-index append order, which is id-ascending; and observed_at then id).
//
// Every parity case also runs with force_scan both ways, so index selection is pinned to changing cost
// and never results.

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/knowledge_kernel.hpp"
#include "kernel/query.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

constexpr Timestamp JAN_1_2020 = 1577836800;
constexpr Timestamp JAN_1_2023 = 1672531200;
constexpr Timestamp JAN_1_2024 = 1704067200;
constexpr Timestamp OBS_1 = 1719792000;
constexpr Timestamp OBS_2 = 1719878400;
constexpr Timestamp OBS_3 = 1719961200;
constexpr Timestamp OBS_4 = 1720047600;
constexpr Timestamp OBS_5 = 1720134000;
constexpr Timestamp OBS_6 = 1720220400;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("query_engine_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

// A kernel holding all five statuses, closed and open-ended intervals, two subjects sharing an object,
// and a spread of observed times -- so a parity failure in any one filter has something to bite on.
struct Fixture {
    EntityId alice;
    EntityId bob;
    EntityId acme;
    EntityId beta;
    EntityId source;
    PredicateId works_at;
    PredicateId lives_in;

    AssertionId closed;      // alice works_at acme, closed interval, Active
    AssertionId current;     // alice works_at beta, open-ended, Active
    AssertionId lives;       // alice lives_in acme, open-ended, Active
    AssertionId superseded;  // bob works_at acme -> Superseded
    AssertionId superseding; // bob works_at beta, open-ended, Active
    AssertionId hypothesis;  // alice lives_in beta, Hypothesis
    AssertionId retracted;   // bob lives_in acme -> Retracted
    AssertionId retraction;  // the Retraction audit record for it
};

Fixture seed(KnowledgeKernel &kernel) {
    Fixture f;
    f.alice = kernel.intern_entity("Alice");
    f.bob = kernel.intern_entity("Bob");
    f.acme = kernel.intern_entity("Acme");
    f.beta = kernel.intern_entity("Beta");
    f.source = kernel.intern_entity("pipeline");
    f.works_at = kernel.intern_predicate("works_at");
    f.lives_in = kernel.intern_predicate("lives_in");

    f.closed = kernel.commit(f.alice, f.works_at, f.acme, JAN_1_2020, JAN_1_2023, OBS_1, 0.90);
    f.current = kernel.commit(f.alice, f.works_at, f.beta, JAN_1_2023, OPEN_ENDED, OBS_2, 0.95);
    f.lives = kernel.commit(f.alice, f.lives_in, f.acme, JAN_1_2020, OPEN_ENDED, OBS_1, 0.80);
    f.superseded = kernel.commit(f.bob, f.works_at, f.acme, JAN_1_2023, OPEN_ENDED, OBS_3, 0.70);
    f.superseding =
        kernel.commit_superseding(f.bob, f.works_at, f.beta, JAN_1_2023, OPEN_ENDED, OBS_4, 0.90, f.superseded);
    f.hypothesis = kernel.commit_hypothesis(f.alice, f.lives_in, f.beta, JAN_1_2024, OPEN_ENDED, OBS_5, 0.50, f.source,
                                            OBS_5, "guess");
    f.retracted = kernel.commit(f.bob, f.lives_in, f.acme, JAN_1_2020, OPEN_ENDED, OBS_2, 0.60);
    f.retraction = kernel.commit_retraction(f.bob, f.lives_in, f.acme, JAN_1_2020, OPEN_ENDED, OBS_6, 1.0, f.retracted);

    return f;
}

std::vector<AssertionId> ids_of(const std::vector<Assertion> &assertions) {
    std::vector<AssertionId> ids;
    for (const auto &assertion : assertions) {
        ids.push_back(assertion.id);
    }
    return ids;
}

std::vector<AssertionId> sorted_ids(const std::vector<Assertion> &assertions) {
    auto ids = ids_of(assertions);
    std::sort(ids.begin(), ids.end());
    return ids;
}

// Runs the query both with and without index selection and asserts the two agree, returning the rows.
// Every parity assertion below goes through this, so "the index path and the scan path answer the same
// thing" is checked on every case for free.
std::vector<Assertion> run_both_ways(const KnowledgeKernel &kernel, Query query) {
    query.force_scan = false;
    auto indexed = kernel.query(query);

    query.force_scan = true;
    auto scanned = kernel.query(query);

    assert(ids_of(indexed.assertions) == ids_of(scanned.assertions));
    assert(indexed.truncated == scanned.truncated);

    return indexed.assertions;
}

// --- Parity with the existing query methods --------------------------------------

void query_matches_current() {
    auto root = test_root("query_matches_current");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    Query query;
    query.subject = f.alice;
    query.statuses = {AssertionStatus::Active};
    query.open_ended_only = true;

    assert(sorted_ids(run_both_ways(kernel, query)) == sorted_ids(kernel.current(f.alice)));

    // Not vacuous: Alice has an Active-but-closed assertion and a Hypothesis that must both be absent.
    auto ids = sorted_ids(run_both_ways(kernel, query));
    assert(ids.size() == 2);
    assert(std::find(ids.begin(), ids.end(), f.closed) == ids.end());
    assert(std::find(ids.begin(), ids.end(), f.hypothesis) == ids.end());

    cleanup(root);
}

void query_matches_current_by_object_and_predicate() {
    auto root = test_root("query_matches_current_by_object_and_predicate");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    Query by_object;
    by_object.object = f.acme;
    by_object.statuses = {AssertionStatus::Active};
    by_object.open_ended_only = true;
    assert(sorted_ids(run_both_ways(kernel, by_object)) == sorted_ids(kernel.current_by_object(f.acme)));

    Query by_predicate;
    by_predicate.predicate = f.works_at;
    by_predicate.statuses = {AssertionStatus::Active};
    by_predicate.open_ended_only = true;
    assert(sorted_ids(run_both_ways(kernel, by_predicate)) == sorted_ids(kernel.current_by_predicate(f.works_at)));

    // The superseded assertion had works_at/acme, so both queries would return it if status were ignored.
    auto predicate_ids = sorted_ids(run_both_ways(kernel, by_predicate));
    assert(std::find(predicate_ids.begin(), predicate_ids.end(), f.superseded) == predicate_ids.end());

    cleanup(root);
}

void query_matches_valid_at_and_known_at() {
    auto root = test_root("query_matches_valid_at_and_known_at");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    for (Timestamp t : {JAN_1_2020, JAN_1_2023, JAN_1_2024}) {
        Query valid;
        valid.subject = f.alice;
        valid.statuses = {AssertionStatus::Active};
        valid.valid_at = t;
        assert(sorted_ids(run_both_ways(kernel, valid)) == sorted_ids(kernel.valid_at(f.alice, t)));
    }

    for (Timestamp t : {OBS_1, OBS_2, OBS_5, OBS_6}) {
        Query known;
        known.subject = f.alice;
        known.statuses = {AssertionStatus::Active};
        known.observed_to = t;
        assert(sorted_ids(run_both_ways(kernel, known)) == sorted_ids(kernel.known_at(f.alice, t)));
    }

    Query both;
    both.subject = f.alice;
    both.statuses = {AssertionStatus::Active};
    both.valid_at = JAN_1_2023;
    both.observed_to = OBS_2;
    assert(sorted_ids(run_both_ways(kernel, both)) == sorted_ids(kernel.valid_at_known_at(f.alice, JAN_1_2023, OBS_2)));

    // valid_to is exclusive: the closed assertion covers JAN_1_2020 but not the instant it ends.
    Query at_end;
    at_end.subject = f.alice;
    at_end.statuses = {AssertionStatus::Active};
    at_end.valid_at = JAN_1_2023;
    auto at_end_ids = sorted_ids(run_both_ways(kernel, at_end));
    assert(std::find(at_end_ids.begin(), at_end_ids.end(), f.closed) == at_end_ids.end());

    cleanup(root);
}

void query_matches_hypotheses_for() {
    auto root = test_root("query_matches_hypotheses_for");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    Query query;
    query.subject = f.alice;
    query.statuses = {AssertionStatus::Hypothesis};

    auto ids = sorted_ids(run_both_ways(kernel, query));
    assert(ids == sorted_ids(kernel.hypotheses_for(f.alice)));
    assert(ids.size() == 1 && ids[0] == f.hypothesis);

    cleanup(root);
}

void query_matches_assertions_for_subject_in_order() {
    auto root = test_root("query_matches_assertions_for_subject_in_order");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    // Sequence parity, not set parity: the subject index is append-ordered, which is id-ascending, and
    // the IR's default order is id-ascending too.
    Query query;
    query.subject = f.alice;
    assert(ids_of(run_both_ways(kernel, query)) == ids_of(kernel.assertions_for_subject(f.alice)));

    // An explicit limit is a prefix of that same order in both.
    Query limited = query;
    limited.limit = 2;
    assert(ids_of(run_both_ways(kernel, limited)) == ids_of(kernel.assertions_for_subject(f.alice, 2)));

    // No status filter means the audit rows come too: Alice's Hypothesis is included here.
    auto all_ids = ids_of(run_both_ways(kernel, query));
    assert(std::find(all_ids.begin(), all_ids.end(), f.hypothesis) != all_ids.end());

    cleanup(root);
}

void query_matches_changes_since_in_order() {
    auto root = test_root("query_matches_changes_since_in_order");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    for (bool newest_first : {false, true}) {
        for (size_t limit : {size_t{0}, size_t{3}}) {
            Query query;
            query.observed_from = OBS_2;
            query.order = QueryOrder::ObservedAt;
            query.newest_first = newest_first;
            query.limit = limit;

            assert(ids_of(run_both_ways(kernel, query)) == ids_of(kernel.changes_since(OBS_2, limit, newest_first)));
        }
    }

    cleanup(root);
}

void query_resolves_merged_entities_like_the_methods() {
    auto root = test_root("query_resolves_merged_entities_like_the_methods");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    constexpr EntityId ALICE_DUPLICATE = 987654;
    kernel.merge_entities(f.alice, ALICE_DUPLICATE, OBS_6);

    // Querying by the absorbed id must answer as the surviving id does, exactly as current() does.
    Query query;
    query.subject = ALICE_DUPLICATE;
    query.statuses = {AssertionStatus::Active};
    query.open_ended_only = true;

    assert(sorted_ids(run_both_ways(kernel, query)) == sorted_ids(kernel.current(ALICE_DUPLICATE)));
    assert(sorted_ids(run_both_ways(kernel, query)) == sorted_ids(kernel.current(f.alice)));

    cleanup(root);
}

// --- IR behavior the existing methods have no equivalent for ---------------------

void query_combines_filters_no_single_method_can() {
    auto root = test_root("query_combines_filters_no_single_method_can");
    KnowledgeKernel kernel(StorageConfig{root});
    auto f = seed(kernel);

    // The point of the phase: subject + predicate + object + status + open-endedness at once, which no
    // existing method expresses.
    Query query;
    query.subject = f.bob;
    query.predicate = f.works_at;
    query.object = f.beta;
    query.statuses = {AssertionStatus::Active};
    query.open_ended_only = true;

    auto ids = ids_of(run_both_ways(kernel, query));
    assert(ids.size() == 1 && ids[0] == f.superseding);

    // Audit-shaped in the other direction: every non-Active row for Bob, any predicate.
    Query audit;
    audit.subject = f.bob;
    audit.statuses = {AssertionStatus::Superseded, AssertionStatus::Retracted, AssertionStatus::Retraction};
    auto audit_ids = sorted_ids(run_both_ways(kernel, audit));
    assert(audit_ids == (std::vector<AssertionId>{f.superseded, f.retracted, f.retraction}));

    cleanup(root);
}

void query_orders_deterministically() {
    auto root = test_root("query_orders_deterministically");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    Query by_valid_from;
    by_valid_from.order = QueryOrder::ValidFrom;
    auto ascending = ids_of(run_both_ways(kernel, by_valid_from));

    for (size_t i = 1; i < ascending.size(); ++i) {
        auto previous = kernel.get(ascending[i - 1]);
        auto current = kernel.get(ascending[i]);
        assert(previous->valid_from < current->valid_from ||
               (previous->valid_from == current->valid_from && previous->id < current->id));
    }

    // newest_first reverses the tie-break too, so the reversal is exact.
    Query descending = by_valid_from;
    descending.newest_first = true;
    auto reversed = ids_of(run_both_ways(kernel, descending));
    std::reverse(reversed.begin(), reversed.end());
    assert(reversed == ascending);

    cleanup(root);
}

void query_limit_offset_and_truncation() {
    auto root = test_root("query_limit_offset_and_truncation");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    Query all;
    auto total = kernel.query(all);
    assert(!total.truncated);
    assert(total.assertions.size() == 8);

    Query page;
    page.limit = 3;
    auto first = kernel.query(page);
    assert(first.assertions.size() == 3);
    assert(first.truncated); // more matched than were returned

    page.offset = 3;
    auto second = kernel.query(page);
    assert(second.assertions.size() == 3);
    assert(second.truncated);

    page.offset = 6;
    auto last = kernel.query(page);
    assert(last.assertions.size() == 2);
    assert(!last.truncated); // the tail exactly fits, so nothing was cut

    // Paging covers the whole answer with no gaps or repeats.
    std::vector<AssertionId> paged;
    for (size_t offset = 0; offset < 8; offset += 3) {
        Query p;
        p.limit = 3;
        p.offset = offset;
        for (AssertionId id : ids_of(kernel.query(p).assertions)) {
            paged.push_back(id);
        }
    }
    assert(paged == ids_of(total.assertions));

    // An offset past the end is an empty page, not an error.
    Query beyond;
    beyond.offset = 100;
    assert(kernel.query(beyond).assertions.empty());

    cleanup(root);
}

void query_caps_limit_at_the_result_ceiling() {
    auto root = test_root("query_caps_limit_at_the_result_ceiling");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    // A limit above the ceiling is capped rather than refused; with only 8 rows the cap is invisible in
    // the row count, so assert the ceiling is what a 0 limit resolves to by checking both agree.
    Query huge;
    huge.limit = MAX_QUERY_RESULT * 10;
    Query defaulted;
    defaulted.limit = 0;

    assert(ids_of(kernel.query(huge).assertions) == ids_of(kernel.query(defaulted).assertions));

    cleanup(root);
}

void query_rejects_an_unknown_ir_version() {
    auto root = test_root("query_rejects_an_unknown_ir_version");
    KnowledgeKernel kernel(StorageConfig{root});
    seed(kernel);

    Query query;
    query.ir_version = QUERY_IR_VERSION + 1;

    bool threw = false;
    try {
        kernel.query(query);
    } catch (const std::runtime_error &) {
        threw = true;
    }

    // Rejected, not guessed at: a stored or forwarded query from a newer build must never be
    // reinterpreted under this build's rules.
    assert(threw);

    cleanup(root);
}

void query_on_an_empty_kernel_answers_empty() {
    auto root = test_root("query_on_an_empty_kernel_answers_empty");
    KnowledgeKernel kernel(StorageConfig{root});

    Query query;
    auto result = kernel.query(query);
    assert(result.assertions.empty());
    assert(!result.truncated);

    // A query naming ids that were never interned is empty too, not an error.
    Query unknown;
    unknown.subject = 4242;
    assert(kernel.query(unknown).assertions.empty());

    cleanup(root);
}

void query_survives_a_restart() {
    auto root = test_root("query_survives_a_restart");

    Fixture f;
    std::vector<AssertionId> before;
    {
        KnowledgeKernel kernel(StorageConfig{root});
        f = seed(kernel);

        Query query;
        query.subject = f.alice;
        query.statuses = {AssertionStatus::Active};
        query.open_ended_only = true;
        before = sorted_ids(run_both_ways(kernel, query));
    }

    KnowledgeKernel recovered(StorageConfig{root});

    Query query;
    query.subject = f.alice;
    query.statuses = {AssertionStatus::Active};
    query.open_ended_only = true;

    // The engine reads whatever replay rebuilt, so this also guards the index paths it selects over.
    assert(sorted_ids(run_both_ways(recovered, query)) == before);
    assert(sorted_ids(run_both_ways(recovered, query)) == sorted_ids(recovered.current(f.alice)));

    cleanup(root);
}

} // namespace

int main() {
    query_matches_current();
    query_matches_current_by_object_and_predicate();
    query_matches_valid_at_and_known_at();
    query_matches_hypotheses_for();
    query_matches_assertions_for_subject_in_order();
    query_matches_changes_since_in_order();
    query_resolves_merged_entities_like_the_methods();
    query_combines_filters_no_single_method_can();
    query_orders_deterministically();
    query_limit_offset_and_truncation();
    query_caps_limit_at_the_result_ceiling();
    query_rejects_an_unknown_ir_version();
    query_on_an_empty_kernel_answers_empty();
    query_survives_a_restart();

    std::cout << "All query_engine tests passed.\n";
    return 0;
}
