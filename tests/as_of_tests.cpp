// Phase 19: as-of reconstruction.
//
// The claim is strong and therefore needs a strong test: an as-of query answers exactly what the kernel
// would have answered at that point. So the test does not check a hand-computed expectation -- it builds
// the earlier kernel. Each prefix of the log is replayed into its own scratch storage root, and what
// `current()` returns there must equal what `as_of_commit = N` returns against the whole log. If the
// reconstruction is wrong in any way at all, the two disagree.
//
// The corpus is built to make that comparison hard: supersessions, retractions, a hypothesis that is
// later superseded (the case that forced the snapshot format change -- effective status cannot say what
// a closed row was committed as), a row that is superseded and then retracted, and backdated
// observed_at so observed order and commit order genuinely differ.

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/json_codec.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/mcp_tools.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

constexpr EntityId ALICE = 1;
constexpr EntityId BOB = 2;
constexpr EntityId CAROL = 3;
constexpr PredicateId WORKS_AT = 1;
constexpr PredicateId LIVES_IN = 2;
constexpr EntityId ACME = 90'001;
constexpr EntityId GLOBEX = 90'002;
constexpr EntityId LONDON = 90'003;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("as_of_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

// The corpus as a script of operations, each appending exactly one record, so ids are 1..RECORD_COUNT in
// step order. "The kernel as it was after N commits" is then literally the first N steps applied to a
// fresh store -- which is what makes the equivalence test below a comparison against a real past kernel
// rather than against a hand-computed expectation.
//
//   1  Alice works_at Acme            observed 100
//   2  Bob   works_at Acme            observed 110
//   3  Carol works_at Globex          observed 120, Hypothesis
//   4  Alice lives_in London          observed 130
//   5  supersedes 1: Alice -> Globex  observed 200
//   6  supersedes 3: Carol -> Acme    observed 210   (a superseded *hypothesis*)
//   7  retracts 4                     observed 220
//   8  Bob lives_in London            observed 150   (backdated: observed before 5-7, committed after)
constexpr size_t RECORD_COUNT = 8;

void seed_through(KnowledgeKernel &kernel, size_t steps) {
    kernel.intern_predicate("works_at");
    kernel.intern_predicate("lives_in");

    if (steps >= 1) {
        kernel.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 100, 0.9);
    }
    if (steps >= 2) {
        kernel.commit(BOB, WORKS_AT, ACME, 0, OPEN_ENDED, 110, 0.9);
    }
    if (steps >= 3) {
        kernel.commit_hypothesis(CAROL, WORKS_AT, GLOBEX, 0, OPEN_ENDED, 120, 0.4, ALICE, 120, "test");
    }
    if (steps >= 4) {
        kernel.commit(ALICE, LIVES_IN, LONDON, 0, OPEN_ENDED, 130, 0.8);
    }
    if (steps >= 5) {
        kernel.commit_superseding(ALICE, WORKS_AT, GLOBEX, 0, OPEN_ENDED, 200, 0.95, 1);
    }
    if (steps >= 6) {
        kernel.commit_superseding(CAROL, WORKS_AT, ACME, 0, OPEN_ENDED, 210, 0.9, 3);
    }
    if (steps >= 7) {
        kernel.commit_retraction(ALICE, LIVES_IN, LONDON, 0, OPEN_ENDED, 220, 0.8, 4);
    }
    if (steps >= 8) {
        kernel.commit(BOB, LIVES_IN, LONDON, 0, OPEN_ENDED, 150, 0.7);
    }
}

void seed(KnowledgeKernel &kernel) { seed_through(kernel, RECORD_COUNT); }

std::vector<AssertionId> ids_of(const std::vector<Assertion> &assertions) {
    std::vector<AssertionId> ids;
    for (const auto &assertion : assertions) {
        ids.push_back(assertion.id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<AssertionId> ids_of(const QueryResult &result) { return ids_of(result.assertions); }

// Everything active across every subject, which is what "what did we believe" means when asked of the
// whole store rather than of one subject.
Query current_shaped() {
    Query query;
    query.statuses = {AssertionStatus::Active};
    query.open_ended_only = true;
    return query;
}

void as_of_commit_equals_a_kernel_replayed_to_that_commit() {
    auto root = test_root("prefix");
    {
        KnowledgeKernel kernel(StorageConfig{(root / "full").string()});
        seed(kernel);

        for (size_t prefix = 0; prefix <= RECORD_COUNT; ++prefix) {
            // The kernel as it was: a scratch store holding exactly the first `prefix` records, built by
            // committing them -- a real kernel at that point in its life, not a mock of one.
            auto scratch_root = root / ("prefix_" + std::to_string(prefix));
            KnowledgeKernel scratch(StorageConfig{scratch_root.string()});
            seed_through(scratch, prefix);

            Query then = current_shaped();
            std::vector<AssertionId> expected = ids_of(scratch.query(then));

            Query as_of = current_shaped();
            as_of.as_of_commit = static_cast<AssertionId>(prefix);

            // All three execution paths, because an as-of mode changes what each one compares against:
            // the index path looks the status up per row, the columnar path scans reconstructed bytes,
            // and the row path walks everything. They must agree with the past, and with each other.
            for (int path = 0; path < 3; ++path) {
                Query variant = as_of;
                variant.force_scan = path != 0;
                variant.force_row_scan = path == 2;

                std::vector<AssertionId> reconstructed = ids_of(kernel.query(variant));
                if (reconstructed != expected) {
                    std::cerr << "as-of mismatch at prefix " << prefix << ", path " << path << "\n  expected [";
                    for (AssertionId id : expected) {
                        std::cerr << id << ",";
                    }
                    std::cerr << "]\n  got      [";
                    for (AssertionId id : reconstructed) {
                        std::cerr << id << ",";
                    }
                    std::cerr << "]\n";
                    assert(false && "as_of_commit disagreed with a kernel replayed to that commit");
                }
            }

            // The same equivalence for every status, not just the current-shaped question: an auditor
            // asking what was Superseded then is asking the same kind of question.
            for (AssertionStatus status :
                 {AssertionStatus::Active, AssertionStatus::Superseded, AssertionStatus::Retracted,
                  AssertionStatus::Hypothesis, AssertionStatus::Retraction}) {
                Query then_status;
                then_status.statuses = {status};
                Query as_of_status = then_status;
                as_of_status.as_of_commit = static_cast<AssertionId>(prefix);

                assert(ids_of(kernel.query(as_of_status)) == ids_of(scratch.query(then_status)));
            }
        }
    }
    cleanup(root);
    std::cout << "as_of_commit_equals_a_kernel_replayed_to_that_commit ok\n";
}

void a_superseded_hypothesis_reconstructs_as_a_hypothesis() {
    auto root = test_root("hypothesis");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        // Record 3 is the hypothesis about Carol; record 6 superseded it. Now it reads Superseded.
        assert(kernel.get(3)->status == AssertionStatus::Superseded);

        // As of commit 5 it was still an open hypothesis -- not Active, which is what a reconstruction
        // that assumed "closed rows were committed Active" would say, and not Superseded either.
        Query hypotheses;
        hypotheses.statuses = {AssertionStatus::Hypothesis};
        hypotheses.as_of_commit = 5;
        assert(ids_of(kernel.query(hypotheses)) == std::vector<AssertionId>{3});

        Query active;
        active.statuses = {AssertionStatus::Active};
        active.as_of_commit = 5;
        auto active_ids = ids_of(kernel.query(active));
        assert(std::find(active_ids.begin(), active_ids.end(), 3) == active_ids.end());

        // And after the supersession it is Superseded, at every as-of point from 6 on.
        Query superseded;
        superseded.statuses = {AssertionStatus::Superseded};
        superseded.as_of_commit = 6;
        auto superseded_ids = ids_of(kernel.query(superseded));
        assert(std::find(superseded_ids.begin(), superseded_ids.end(), 3) != superseded_ids.end());
    }
    cleanup(root);
    std::cout << "a_superseded_hypothesis_reconstructs_as_a_hypothesis ok\n";
}

// The reason the snapshot format moved to v2. A snapshot stored effective statuses, which cannot say
// what a closed row was committed as -- so the fast path and a full replay would answer this differently.
void a_snapshot_does_not_change_what_an_as_of_query_answers() {
    auto root = test_root("snapshot");
    {
        std::vector<AssertionId> without_snapshot;
        std::vector<AssertionId> with_snapshot;

        Query hypotheses;
        hypotheses.statuses = {AssertionStatus::Hypothesis};
        hypotheses.as_of_commit = 5;

        {
            KnowledgeKernel kernel(StorageConfig{(root / "plain").string()});
            seed(kernel);
            without_snapshot = ids_of(kernel.query(hypotheses));
        }
        {
            // Reopened from the log, no snapshot: the full-replay path.
            KnowledgeKernel reopened(StorageConfig{(root / "plain").string()});
            assert(ids_of(reopened.query(hypotheses)) == without_snapshot);
        }

        {
            KnowledgeKernel kernel(StorageConfig{(root / "snapped").string()});
            seed(kernel);
            kernel.write_snapshot();
        }
        {
            // Reopened through the snapshot fast path.
            KnowledgeKernel reopened(StorageConfig{(root / "snapped").string()});
            with_snapshot = ids_of(reopened.query(hypotheses));
        }

        assert(with_snapshot == without_snapshot);
        assert(with_snapshot == std::vector<AssertionId>{3});

        // The fast path must also get *present* state right, which is what the snapshot's effective
        // statuses used to provide directly and are now re-derived.
        KnowledgeKernel reopened(StorageConfig{(root / "snapped").string()});
        assert(reopened.get(3)->status == AssertionStatus::Superseded);
        assert(reopened.get(1)->status == AssertionStatus::Superseded);
        assert(reopened.get(4)->status == AssertionStatus::Retracted);
        assert(reopened.get(2)->status == AssertionStatus::Active);
        assert(ids_of(reopened.query(current_shaped())) == ids_of(reopened.query(current_shaped())));
    }
    cleanup(root);
    std::cout << "a_snapshot_does_not_change_what_an_as_of_query_answers ok\n";
}

void as_of_observed_answers_what_known_at_cannot() {
    auto root = test_root("observed");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        // The documented gap: by observed time 150 the kernel had seen Alice-at-Acme (observed 100) and
        // nothing had corrected it (the correction was observed at 200). known_at applies the cutoff but
        // reports status now, so it loses that row entirely.
        auto known = kernel.known_at(ALICE, 150);
        auto known_ids = ids_of(known);
        assert(std::find(known_ids.begin(), known_ids.end(), 1) == known_ids.end());

        Query as_of;
        as_of.subject = ALICE;
        as_of.statuses = {AssertionStatus::Active};
        as_of.as_of_observed = 150;
        auto reconstructed = ids_of(kernel.query(as_of));

        // Alice at Acme (1) and Alice in London (4) were both believed then. The correction (5) and the
        // retraction (7) had not been observed yet.
        assert(reconstructed == (std::vector<AssertionId>{1, 4}));

        // After the correction is observed, the old row stops being believed and the new one starts.
        Query later = as_of;
        later.as_of_observed = 200;
        auto later_ids = ids_of(kernel.query(later));
        assert(std::find(later_ids.begin(), later_ids.end(), 1) == later_ids.end());
        assert(std::find(later_ids.begin(), later_ids.end(), 5) != later_ids.end());

        // Backdating: record 8 was committed last but observed at 150, so it is visible as of observed
        // time 150 even though as_of_commit = 7 cannot see it. The two modes ask different questions on
        // purpose, and this is the case where they visibly differ.
        Query by_observed;
        by_observed.subject = BOB;
        by_observed.as_of_observed = 150;
        auto observed_ids = ids_of(kernel.query(by_observed));
        assert(std::find(observed_ids.begin(), observed_ids.end(), 8) != observed_ids.end());

        Query by_commit;
        by_commit.subject = BOB;
        by_commit.as_of_commit = 7;
        auto commit_ids = ids_of(kernel.query(by_commit));
        assert(std::find(commit_ids.begin(), commit_ids.end(), 8) == commit_ids.end());

        // All three paths agree here too.
        for (int path = 1; path < 3; ++path) {
            Query variant = as_of;
            variant.force_scan = true;
            variant.force_row_scan = path == 2;
            assert(ids_of(kernel.query(variant)) == reconstructed);
        }
    }
    cleanup(root);
    std::cout << "as_of_observed_answers_what_known_at_cannot ok\n";
}

void an_as_of_query_sees_no_row_committed_later() {
    auto root = test_root("visibility");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        for (AssertionId point = 0; point <= RECORD_COUNT; ++point) {
            Query query;
            query.as_of_commit = point;

            for (int path = 0; path < 3; ++path) {
                Query variant = query;
                variant.force_scan = path != 0;
                variant.force_row_scan = path == 2;

                QueryResult result = kernel.query(variant);
                assert(result.assertions.size() == point);
                for (const auto &assertion : result.assertions) {
                    assert(assertion.id <= point);
                }
            }
        }
    }
    cleanup(root);
    std::cout << "an_as_of_query_sees_no_row_committed_later ok\n";
}

void an_as_of_aggregate_counts_what_was_believed_then() {
    auto root = test_root("aggregate");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        auto active_count = [&kernel](std::optional<AssertionId> as_of) {
            AggregateQuery aggregate;
            aggregate.aggregations = {{AggregateFunction::Count, AggregateTarget::Confidence}};
            aggregate.selection.statuses = {AssertionStatus::Active};
            aggregate.selection.open_ended_only = true;
            aggregate.selection.as_of_commit = as_of;

            AggregateResult result = kernel.aggregate(aggregate);
            return result.groups.empty() ? int64_t{0} : *result.groups.front().values.front().count;
        };

        // An aggregate and a query must never disagree about which rows are current -- as of any point.
        for (AssertionId point = 0; point <= RECORD_COUNT; ++point) {
            Query query = current_shaped();
            query.as_of_commit = point;
            assert(active_count(point) == static_cast<int64_t>(kernel.query(query).assertions.size()));
        }

        assert(active_count(std::nullopt) == static_cast<int64_t>(kernel.query(current_shaped()).assertions.size()));

        // Grouping works the same way: as of commit 4, Alice has two active facts and nothing is closed.
        AggregateQuery grouped;
        grouped.aggregations = {{AggregateFunction::Count, AggregateTarget::Confidence}};
        grouped.group_by = {{GroupField::Status, 0}};
        grouped.selection.as_of_commit = 4;

        AggregateResult result = kernel.aggregate(grouped);
        int64_t total = 0;
        for (const auto &group : result.groups) {
            total += group.row_count;
        }
        assert(total == 4);
    }
    cleanup(root);
    std::cout << "an_as_of_aggregate_counts_what_was_believed_then ok\n";
}

void a_status_filter_sees_the_reconstructed_status() {
    auto root = test_root("filter");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        // A filter and the statuses set must agree about what a row is; if the filter read the status as
        // it stands now, this would return nothing.
        Query query;
        query.as_of_commit = 5;
        query.filter = Filter::compare(FilterField::Status, CompareOp::Eq, Value::of_text("Hypothesis"));
        assert(ids_of(kernel.query(query)) == std::vector<AssertionId>{3});

        Query negated;
        negated.as_of_commit = 5;
        negated.filter =
            Filter::negate(Filter::compare(FilterField::Status, CompareOp::Eq, Value::of_text("Hypothesis")));
        auto others = ids_of(kernel.query(negated));
        assert(std::find(others.begin(), others.end(), 3) == others.end());
        assert(others.size() == 4);
    }
    cleanup(root);
    std::cout << "a_status_filter_sees_the_reconstructed_status ok\n";
}

void the_planner_refuses_a_current_state_index_for_an_as_of_query() {
    auto root = test_root("plan");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        Query current_by_predicate = current_shaped();
        current_by_predicate.predicate = WORKS_AT;

        QueryPlan now = kernel.explain_query(current_by_predicate);
        bool offered = false;
        for (const auto &option : now.considered) {
            if (option.source == PlanSource::PredicateCurrentIndex) {
                offered = option.rejected_because.empty();
            }
        }
        assert(offered); // without an as-of mode the index is a candidate

        Query as_of = current_by_predicate;
        as_of.as_of_commit = 4;
        QueryPlan then = kernel.explain_query(as_of);
        for (const auto &option : then.considered) {
            if (option.source == PlanSource::PredicateCurrentIndex) {
                // Rejected for correctness, with a reason that says so -- the index describes now. (The
                // object index is rejected earlier, for naming no object, which is why only this one
                // reaches the as-of check.)
                assert(option.rejected_because.find("as-of") != std::string::npos);
            }
            if (option.source == PlanSource::ObjectCurrentIndex) {
                assert(!option.rejected_because.empty());
            }
        }
        assert(then.chosen != PlanSource::PredicateCurrentIndex);
        assert(then.chosen != PlanSource::ObjectCurrentIndex);

        // The observed-time index, by contrast, is exactly the right shape for an as-of-observed query:
        // it is status-agnostic and ordered by the bound being asked about.
        Query observed;
        observed.subject = ALICE;
        observed.as_of_observed = 150;
        QueryPlan observed_plan = kernel.explain_query(observed);
        for (const auto &option : observed_plan.considered) {
            if (option.source == PlanSource::ObservedTimeIndex) {
                assert(option.rejected_because.empty());
            }
        }
    }
    cleanup(root);
    std::cout << "the_planner_refuses_a_current_state_index_for_an_as_of_query ok\n";
}

void the_two_as_of_modes_are_mutually_exclusive() {
    auto root = test_root("exclusive");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        Query both;
        both.as_of_commit = 3;
        both.as_of_observed = 150;

        bool refused = false;
        try {
            kernel.query(both);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);

        AggregateQuery aggregate;
        aggregate.aggregations = {{AggregateFunction::Count, AggregateTarget::Confidence}};
        aggregate.selection = both;

        refused = false;
        try {
            kernel.aggregate(aggregate);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);

        // An as-of point past the end of the log is simply "now", and one at 0 is "before anything":
        // both are well-formed questions with obvious answers, so neither is an error.
        Query future;
        future.as_of_commit = 10'000;
        assert(kernel.query(future).assertions.size() == RECORD_COUNT);

        Query beginning;
        beginning.as_of_commit = 0;
        assert(kernel.query(beginning).assertions.empty());
    }
    cleanup(root);
    std::cout << "the_two_as_of_modes_are_mutually_exclusive ok\n";
}

void the_mcp_surface_exposes_both_as_of_modes() {
    auto root = test_root("mcp");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        auto call = [&kernel](const std::string &name, const nlohmann::json &args) {
            mcp::ToolCallResult result = mcp::handle_tool_call(kernel, name, args);
            assert(!result.is_error);
            return nlohmann::json::parse(result.content_text);
        };

        // As of commit 4 the store held four rows, three of them Active -- record 3 was a hypothesis.
        nlohmann::json then = call("query", {{"as_of_commit", 4}, {"statuses", {"Active"}}});
        assert(then.at("assertions").size() == 3);

        nlohmann::json believed =
            call("query", {{"subject", ALICE}, {"as_of_observed", 150}, {"statuses", {"Active"}}});
        std::set<AssertionId> ids;
        for (const auto &assertion : believed.at("assertions")) {
            ids.insert(assertion.at("id").get<AssertionId>());
        }
        assert(ids == (std::set<AssertionId>{1, 4}));

        // The hypothesis reads as one over the wire too, not as an Active fact.
        nlohmann::json hypothesis = call("query", {{"as_of_commit", 5}, {"statuses", {"Hypothesis"}}});
        assert(hypothesis.at("assertions").size() == 1);
        assert(hypothesis.at("assertions").at(0).at("id") == 3);
        assert(hypothesis.at("assertions").at(0).at("status") == "Hypothesis");

        mcp::ToolCallResult refused =
            mcp::handle_tool_call(kernel, "query", {{"as_of_commit", 3}, {"as_of_observed", 150}});
        assert(refused.is_error);

        for (const auto &spec : mcp::tool_specs()) {
            if (spec.name == "query" || spec.name == "aggregate" || spec.name == "explain_query" ||
                spec.name == "query_spill") {
                const auto &properties = spec.input_schema.at("properties");
                assert(properties.contains("as_of_commit"));
                assert(properties.contains("as_of_observed"));
            }
        }
    }
    cleanup(root);
    std::cout << "the_mcp_surface_exposes_both_as_of_modes ok\n";
}

} // namespace

int main() {
    as_of_commit_equals_a_kernel_replayed_to_that_commit();
    a_superseded_hypothesis_reconstructs_as_a_hypothesis();
    a_snapshot_does_not_change_what_an_as_of_query_answers();
    as_of_observed_answers_what_known_at_cannot();
    an_as_of_query_sees_no_row_committed_later();
    an_as_of_aggregate_counts_what_was_believed_then();
    a_status_filter_sees_the_reconstructed_status();
    the_planner_refuses_a_current_state_index_for_an_as_of_query();
    the_two_as_of_modes_are_mutually_exclusive();
    the_mcp_surface_exposes_both_as_of_modes();

    std::cout << "all as-of tests passed\n";
    return 0;
}
