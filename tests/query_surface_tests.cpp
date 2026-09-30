// Phase 18: query surface hardening -- keyset cursors, resource budgets, and schema discovery.
//
// Two things need pinning here that no earlier suite can cover.
//
// The first is that a cursor walk sees every row exactly once. That is easy to get subtly wrong at the
// tie-break: rows sharing an ordering key are separated only by AssertionId, so a cursor comparing keys
// with >= instead of > repeats a row, and one comparing with > on the wrong field drops one. Both bugs
// are invisible on a corpus where every key is distinct, so the corpus below deliberately gives many
// rows the same valid_from and observed_at.
//
// The second is that paging stays honest while someone commits. A cursor names a position in the
// ordering, so rows committed mid-walk cannot shift the pages already handed out -- which offset paging
// cannot promise, and the test below shows offset actually breaking where the cursor does not.

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
#include "kernel/query_cursor.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

constexpr size_t ROW_COUNT = 120;
constexpr PredicateId WORKS_AT = 1;
constexpr PredicateId LIVES_IN = 2;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("query_surface_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

// Ordering keys collide on purpose: valid_from takes one of six values and observed_at one of eight, so
// every ordering has long runs of ties for the AssertionId tie-break to separate.
void seed(KnowledgeKernel &kernel) {
    kernel.intern_predicate("works_at");
    kernel.intern_predicate("lives_in");

    std::vector<PendingAssertion> rows;
    for (size_t i = 0; i < ROW_COUNT; ++i) {
        rows.push_back({static_cast<EntityId>(i % 40 + 1), i % 3 == 0 ? LIVES_IN : WORKS_AT,
                        static_cast<EntityId>(90'001 + i % 7), static_cast<Timestamp>(i % 6 * 1000), OPEN_ENDED,
                        static_cast<Timestamp>(i % 8 * 100), 0.5 + static_cast<double>(i % 5) / 10.0});
    }
    kernel.commit_batch(rows);
}

// Walks the whole result one page at a time through next_cursor, returning the ids in the order seen.
std::vector<AssertionId> cursor_walk(const KnowledgeKernel &kernel, Query query, size_t page_size) {
    std::vector<AssertionId> seen;
    std::string cursor;

    for (size_t page = 0; page <= ROW_COUNT / page_size + 2; ++page) {
        query.limit = page_size;
        query.cursor = cursor;

        QueryResult result = kernel.query(query);
        for (const auto &assertion : result.assertions) {
            seen.push_back(assertion.id);
        }

        if (result.next_cursor.empty()) {
            return seen;
        }
        cursor = result.next_cursor;
    }

    assert(false && "cursor walk did not terminate");
    return seen;
}

std::vector<AssertionId> ids_of(const QueryResult &result) {
    std::vector<AssertionId> ids;
    for (const auto &assertion : result.assertions) {
        ids.push_back(assertion.id);
    }
    return ids;
}

void a_cursor_walk_returns_every_row_once_in_order() {
    auto root = test_root("walk");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        // Every ordering, both directions, and all three execution paths: a cursor is a comparison the
        // index path, the columnar path and the row path each apply separately, so each is walked.
        for (QueryOrder order : {QueryOrder::AssertionId, QueryOrder::ValidFrom, QueryOrder::ObservedAt}) {
            for (bool newest_first : {false, true}) {
                for (int path = 0; path < 3; ++path) {
                    Query query;
                    query.order = order;
                    query.newest_first = newest_first;
                    query.predicate = WORKS_AT; // an index is applicable, so path 0 exercises it
                    query.force_scan = path != 0;
                    query.force_row_scan = path == 2;

                    Query whole = query;
                    whole.limit = MAX_QUERY_RESULT;
                    std::vector<AssertionId> expected = ids_of(kernel.query(whole));
                    assert(!expected.empty());

                    for (size_t page_size : {1, 7, 13}) {
                        assert(cursor_walk(kernel, query, page_size) == expected);
                    }
                }
            }
        }
    }
    cleanup(root);
    std::cout << "a_cursor_walk_returns_every_row_once_in_order ok\n";
}

void the_last_page_carries_no_cursor() {
    auto root = test_root("last_page");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        Query query;
        query.limit = ROW_COUNT;
        QueryResult whole = kernel.query(query);
        assert(whole.assertions.size() == ROW_COUNT);
        assert(!whole.truncated);

        // Exactly-the-last-row is the boundary worth pinning: a next_cursor here would send a caller
        // round again for an empty page.
        assert(whole.next_cursor.empty());

        query.limit = ROW_COUNT - 1;
        QueryResult partial = kernel.query(query);
        assert(partial.truncated);
        assert(!partial.next_cursor.empty());

        query.limit = ROW_COUNT;
        query.cursor = partial.next_cursor;
        QueryResult tail = kernel.query(query);
        assert(tail.assertions.size() == 1);
        assert(tail.assertions.front().id == ROW_COUNT);
        assert(tail.next_cursor.empty());

        // An empty result has nothing to resume from either.
        Query nothing;
        nothing.subject = 99'999;
        QueryResult none = kernel.query(nothing);
        assert(none.assertions.empty());
        assert(none.next_cursor.empty());
    }
    cleanup(root);
    std::cout << "the_last_page_carries_no_cursor ok\n";
}

void a_cursor_survives_a_writer_where_an_offset_does_not() {
    auto root = test_root("concurrent");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        // Newest-first is where the difference shows: a committed row lands at the *front* of this
        // ordering, so counted paging shifts by one and offset re-reads a row it already returned.
        Query query;
        query.order = QueryOrder::AssertionId;
        query.newest_first = true;
        query.limit = 5;

        QueryResult first = kernel.query(query);
        std::vector<AssertionId> first_ids = ids_of(first);
        assert(first_ids.size() == 5);

        kernel.commit(1, WORKS_AT, 90'001, 0, OPEN_ENDED, 500, 0.9);
        kernel.commit(2, WORKS_AT, 90'002, 0, OPEN_ENDED, 500, 0.9);

        Query resumed = query;
        resumed.cursor = first.next_cursor;
        std::vector<AssertionId> cursor_ids = ids_of(kernel.query(resumed));

        Query offset_paged = query;
        offset_paged.offset = 5;
        std::vector<AssertionId> offset_ids = ids_of(kernel.query(offset_paged));

        // The cursor page continues where the first left off: no id from page one comes back.
        for (AssertionId id : cursor_ids) {
            assert(std::find(first_ids.begin(), first_ids.end(), id) == first_ids.end());
        }
        assert(cursor_ids.front() == first_ids.back() - 1);

        // Offset, on the same walk, hands back two rows the caller has already seen -- the behaviour the
        // cursor exists to avoid, asserted rather than described so it cannot quietly become untrue.
        size_t repeats = 0;
        for (AssertionId id : offset_ids) {
            if (std::find(first_ids.begin(), first_ids.end(), id) != first_ids.end()) {
                ++repeats;
            }
        }
        assert(repeats == 2);
    }
    cleanup(root);
    std::cout << "a_cursor_survives_a_writer_where_an_offset_does_not ok\n";
}

void a_read_only_walk_is_a_snapshot() {
    auto root = test_root("read_only_walk");
    {
        KnowledgeKernel writer(StorageConfig{root.string()});
        seed(writer);

        // Phase 13's read-only open takes no lock, so writer and reader coexist. The reader's view is
        // fixed at open, which is what makes a walk through it repeatable to the row.
        KnowledgeKernel reader(StorageConfig{root.string()}, OpenMode::ReadOnly);

        Query query;
        query.order = QueryOrder::ObservedAt;
        std::vector<AssertionId> before = cursor_walk(reader, query, 9);

        for (size_t i = 0; i < 20; ++i) {
            writer.commit(1, WORKS_AT, 90'003, 0, OPEN_ENDED, 700, 0.7);
        }

        assert(cursor_walk(reader, query, 9) == before);
        assert(before.size() == ROW_COUNT);
    }
    cleanup(root);
    std::cout << "a_read_only_walk_is_a_snapshot ok\n";
}

void a_malformed_or_mismatched_cursor_is_rejected() {
    auto root = test_root("bad_cursor");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        Query query;
        query.limit = 3;
        query.order = QueryOrder::ValidFrom;
        QueryResult page = kernel.query(query);
        assert(!page.next_cursor.empty());

        auto rejected = [&kernel](const Query &bad) {
            bool threw = false;
            try {
                kernel.query(bad);
            } catch (const std::runtime_error &) {
                threw = true;
            }
            assert(threw);
        };

        // A cursor from a different ordering describes a position that does not exist in this one. Taking
        // it would silently return the wrong window, so it is refused.
        Query wrong_order = query;
        wrong_order.order = QueryOrder::ObservedAt;
        wrong_order.cursor = page.next_cursor;
        rejected(wrong_order);

        Query wrong_direction = query;
        wrong_direction.newest_first = true;
        wrong_direction.cursor = page.next_cursor;
        rejected(wrong_direction);

        // Two answers to "where does this page start"; guessing is worse than refusing.
        Query with_offset = query;
        with_offset.cursor = page.next_cursor;
        with_offset.offset = 10;
        rejected(with_offset);

        for (const std::string &token :
             {"", "garbage", "knkc1:id:asc:1", "knkc1:id:sideways:1:1", "knkc1:nonesuch:asc:1:1", "knkc9:id:asc:1:1",
              "knkc1:id:asc:1:0", "knkc1:id:asc:x:1", "knkc1:id:asc:1:1:1"}) {
            if (token.empty()) {
                continue; // empty means "no cursor", not a bad one -- covered by the walk tests
            }
            Query malformed = query;
            malformed.cursor = token;
            rejected(malformed);
        }
    }
    cleanup(root);
    std::cout << "a_malformed_or_mismatched_cursor_is_rejected ok\n";
}

void cursor_tokens_round_trip() {
    QueryCursor cursor;
    cursor.order = QueryOrder::ObservedAt;
    cursor.newest_first = true;
    cursor.key = -4'000'000'000; // a negative timestamp: before the epoch is a legal observed_at
    cursor.id = 987'654'321;

    QueryCursor decoded = decode_query_cursor(encode_query_cursor(cursor));
    assert(decoded.order == cursor.order);
    assert(decoded.newest_first == cursor.newest_first);
    assert(decoded.key == cursor.key);
    assert(decoded.id == cursor.id);

    // The tie-break direction, checked directly: same key, larger id is "after" ascending and "before"
    // descending. This is the comparison a cursor walk's correctness rests on.
    Assertion row;
    row.id = 10;
    row.observed_at = 500;

    QueryCursor ascending;
    ascending.order = QueryOrder::ObservedAt;
    ascending.key = 500;
    ascending.id = 9;
    assert(after_query_cursor(ascending, row));
    ascending.id = 10;
    assert(!after_query_cursor(ascending, row)); // strictly after: the cursor row is never re-returned
    ascending.id = 11;
    assert(!after_query_cursor(ascending, row));

    QueryCursor descending = ascending;
    descending.newest_first = true;
    descending.id = 11;
    assert(after_query_cursor(descending, row));
    descending.id = 10;
    assert(!after_query_cursor(descending, row));

    std::cout << "cursor_tokens_round_trip ok\n";
}

void a_row_budget_is_enforced_on_every_path() {
    auto root = test_root("budget");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        auto over_budget = [&kernel](const Query &query) {
            try {
                kernel.query(query);
            } catch (const QueryBudgetExceeded &exceeded) {
                assert(exceeded.budget == "max_rows_examined");
                assert(exceeded.limit == query.max_rows_examined);
                assert(exceeded.reached > exceeded.limit);
                return true;
            }
            return false;
        };

        for (int path = 0; path < 3; ++path) {
            Query query;
            query.predicate = WORKS_AT;
            query.force_scan = path != 0;
            query.force_row_scan = path == 2;
            query.max_rows_examined = 5;
            assert(over_budget(query));

            // Generous enough for the whole corpus on any path, so the budget is not simply always fatal.
            query.max_rows_examined = ROW_COUNT * 10;
            assert(!kernel.query(query).assertions.empty());

            // 0 means no budget, which is what every query written before this phase carries.
            query.max_rows_examined = 0;
            assert(!kernel.query(query).assertions.empty());
        }

        // A budget a QueryBudgetExceeded caller does not catch specifically still behaves like the storage
        // errors around it: the type derives from std::runtime_error.
        Query query;
        query.max_rows_examined = 1;
        bool caught_as_runtime_error = false;
        try {
            kernel.query(query);
        } catch (const std::runtime_error &) {
            caught_as_runtime_error = true;
        }
        assert(caught_as_runtime_error);
    }
    cleanup(root);
    std::cout << "a_row_budget_is_enforced_on_every_path ok\n";
}

void an_aggregate_honours_the_row_and_group_budgets() {
    auto root = test_root("aggregate_budget");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        AggregateQuery aggregate;
        aggregate.aggregations = {{AggregateFunction::Count, AggregateTarget::Confidence}};
        aggregate.selection.max_rows_examined = 3;

        bool rows_exceeded = false;
        try {
            kernel.aggregate(aggregate);
        } catch (const QueryBudgetExceeded &exceeded) {
            rows_exceeded = exceeded.budget == "max_rows_examined";
        }
        assert(rows_exceeded);

        // The group cap was already enforced; Phase 18 makes it a budget a caller can recognize rather
        // than a message they have to match on.
        AggregateQuery grouped;
        grouped.aggregations = {{AggregateFunction::Count, AggregateTarget::Confidence}};
        grouped.group_by = {{GroupField::Subject, 0}};
        grouped.max_groups = 4;

        bool groups_exceeded = false;
        try {
            kernel.aggregate(grouped);
        } catch (const QueryBudgetExceeded &exceeded) {
            groups_exceeded = exceeded.budget == "max_groups" && exceeded.limit == 4 && exceeded.reached == 5;
        }
        assert(groups_exceeded);

        grouped.max_groups = 0;
        assert(kernel.aggregate(grouped).groups.size() == 40);

        // An aggregate returns groups, not rows, so a cursor is meaningless to it and is refused rather
        // than ignored -- the same rule limit/offset/order already follow.
        AggregateQuery with_cursor = grouped;
        with_cursor.selection.cursor = "knkc1:id:asc:1:1";
        bool refused = false;
        try {
            kernel.aggregate(with_cursor);
        } catch (const std::runtime_error &) {
            refused = true;
        }
        assert(refused);
    }
    cleanup(root);
    std::cout << "an_aggregate_honours_the_row_and_group_budgets ok\n";
}

void discovery_describes_the_predicates_a_caller_would_have_guessed() {
    auto root = test_root("predicates");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        std::vector<PredicateSummary> predicates = kernel.describe_predicates();
        assert(predicates.size() == 2);

        // Ascending by id, always: an agent shows this list to a user, and a list that reshuffles between
        // identical calls reads as data changing.
        assert(predicates[0].id == WORKS_AT);
        assert(predicates[0].name == "works_at");
        assert(predicates[1].id == LIVES_IN);
        assert(predicates[1].name == "lives_in");

        Query works_at;
        works_at.predicate = WORKS_AT;
        works_at.open_ended_only = true;
        works_at.statuses = {AssertionStatus::Active};
        assert(predicates[0].current_rows == kernel.query(works_at).assertions.size());

        // A retraction moves a row out of "current", so the count has to move with it rather than being
        // a total that includes withdrawn history.
        size_t before = kernel.describe_predicates()[0].current_rows;
        Assertion target = kernel.query(works_at).assertions.front();
        kernel.commit_retraction(target.subject, target.predicate, target.object, target.valid_from, target.valid_to,
                                 900, target.confidence, target.id);
        assert(kernel.describe_predicates()[0].current_rows == before - 1);

        // A predicate interned but never asserted still appears: that is exactly the name a caller needs
        // to be told about before they query for it and conclude the fact is absent.
        kernel.intern_predicate("reports_to");
        std::vector<PredicateSummary> after = kernel.describe_predicates();
        assert(after.size() == 3);
        assert(after[2].name == "reports_to");
        assert(after[2].current_rows == 0);
    }
    cleanup(root);
    std::cout << "discovery_describes_the_predicates_a_caller_would_have_guessed ok\n";
}

void discovery_describes_the_corpus_shape() {
    auto root = test_root("corpus");
    {
        KnowledgeKernel empty_kernel(StorageConfig{(root / "empty").string()});
        CorpusSummary empty = empty_kernel.describe_corpus();
        assert(empty.assertion_count == 0);
        assert(empty.predicate_count == 0);
        assert(empty.status_counts.size() == 5); // every status, zeros included
        for (const auto &[status, count] : empty.status_counts) {
            (void)status;
            assert(count == 0);
        }
        // Absent, not zero: a store with no rows has no observed-time span, and reporting 0 would name a
        // moment in 1970 as if a fact had been seen there.
        assert(!empty.min_observed_at.has_value());
        assert(!empty.max_valid_from.has_value());

        KnowledgeKernel kernel(StorageConfig{(root / "seeded").string()});
        seed(kernel);
        Assertion first = *kernel.get(1);
        kernel.commit_retraction(first.subject, first.predicate, first.object, first.valid_from, first.valid_to, 900,
                                 first.confidence, first.id);

        CorpusSummary summary = kernel.describe_corpus();
        assert(summary.assertion_count == ROW_COUNT + 1); // the retraction is itself a record
        assert(summary.predicate_count == 2);
        assert(summary.distinct_subjects == 40);
        assert(summary.min_valid_from == 0);
        assert(summary.max_valid_from == 5000);
        assert(summary.min_observed_at == 0);
        assert(summary.max_observed_at == 900); // the retraction's own observed_at

        auto count_for = [&summary](AssertionStatus status) {
            for (const auto &[candidate, count] : summary.status_counts) {
                if (candidate == status) {
                    return count;
                }
            }
            assert(false && "status missing from summary");
            return size_t{0};
        };

        assert(count_for(AssertionStatus::Retracted) == 1);
        assert(count_for(AssertionStatus::Retraction) == 1);
        assert(count_for(AssertionStatus::Active) == ROW_COUNT - 1);
        assert(count_for(AssertionStatus::Superseded) == 0);

        size_t total = 0;
        for (const auto &[status, count] : summary.status_counts) {
            (void)status;
            total += count;
        }
        assert(total == summary.assertion_count);
    }
    cleanup(root);
    std::cout << "discovery_describes_the_corpus_shape ok\n";
}

void discovery_works_on_a_read_only_open() {
    auto root = test_root("read_only_discovery");
    {
        {
            KnowledgeKernel writer(StorageConfig{root.string()});
            seed(writer);
        }

        KnowledgeKernel reader(StorageConfig{root.string()}, OpenMode::ReadOnly);
        assert(reader.describe_predicates().size() == 2);
        assert(reader.describe_corpus().assertion_count == ROW_COUNT);
    }
    cleanup(root);
    std::cout << "discovery_works_on_a_read_only_open ok\n";
}

// The MCP layer: the new tools reach the kernel, the new query arguments are parsed, and the results
// serialize into the shapes docs/mcp_server.md documents.
void the_mcp_surface_exposes_cursors_budgets_and_discovery() {
    auto root = test_root("mcp");
    {
        KnowledgeKernel kernel(StorageConfig{root.string()});
        seed(kernel);

        auto call = [&kernel](const std::string &name, const nlohmann::json &args) {
            mcp::ToolCallResult result = mcp::handle_tool_call(kernel, name, args);
            assert(!result.is_error);
            return nlohmann::json::parse(result.content_text);
        };

        nlohmann::json page = call("query", {{"limit", 4}});
        assert(page.at("assertions").size() == 4);
        assert(page.at("truncated") == true);
        assert(page.contains("next_cursor"));

        nlohmann::json second = call("query", {{"limit", 4}, {"cursor", page.at("next_cursor")}});
        assert(second.at("assertions").at(0).at("id") == 5);

        // Absent rather than null on the last page: a client tests for the key.
        nlohmann::json whole = call("query", {{"limit", ROW_COUNT}});
        assert(!whole.contains("next_cursor"));

        // A budget failure is a tool-level error carrying the budget's name -- MCP's convention, and the
        // reason the exception is typed: the client is told which limit it hit, not just that it failed.
        mcp::ToolCallResult refused = mcp::handle_tool_call(kernel, "query", {{"max_rows_examined", 2}});
        assert(refused.is_error);
        assert(refused.content_text.find("max_rows_examined") != std::string::npos);

        nlohmann::json predicates = call("describe_predicates", nlohmann::json::object());
        assert(predicates.is_array());
        assert(predicates.size() == 2);
        assert(predicates.at(0).at("name") == "works_at");
        assert(predicates.at(0).at("current_rows").get<size_t>() > 0);

        nlohmann::json corpus = call("describe_corpus", nlohmann::json::object());
        assert(corpus.at("assertion_count") == ROW_COUNT);
        assert(corpus.at("distinct_subjects") == 40);
        // Keyed by status name, because those are the strings a caller puts into a query's `statuses`.
        assert(corpus.at("status_counts").at("Active") == ROW_COUNT);
        assert(corpus.at("status_counts").at("Retracted") == 0);
        assert(corpus.at("min_observed_at") == 0);

        // The schemas have to advertise what the handlers accept, or a client cannot discover paging.
        for (const auto &spec : mcp::tool_specs()) {
            if (spec.name == "query") {
                const auto &properties = spec.input_schema.at("properties");
                assert(properties.contains("cursor"));
                assert(properties.contains("max_rows_examined"));

                // The wire-format rule from AGENTS.md: the new properties go at the end, so no existing
                // argument's position moves. Checked here because nothing else would notice.
                std::vector<std::string> names;
                for (auto it = properties.begin(); it != properties.end(); ++it) {
                    names.push_back(it.key());
                }
                assert(names[0] == "subject");
                assert(names[names.size() - 2] == "cursor");
                assert(names[names.size() - 1] == "max_rows_examined");
            }
        }
    }
    cleanup(root);
    std::cout << "the_mcp_surface_exposes_cursors_budgets_and_discovery ok\n";
}

} // namespace

int main() {
    cursor_tokens_round_trip();
    a_cursor_walk_returns_every_row_once_in_order();
    the_last_page_carries_no_cursor();
    a_cursor_survives_a_writer_where_an_offset_does_not();
    a_read_only_walk_is_a_snapshot();
    a_malformed_or_mismatched_cursor_is_rejected();
    a_row_budget_is_enforced_on_every_path();
    an_aggregate_honours_the_row_and_group_budgets();
    discovery_describes_the_predicates_a_caller_would_have_guessed();
    discovery_describes_the_corpus_shape();
    discovery_works_on_a_read_only_open();
    the_mcp_surface_exposes_cursors_budgets_and_discovery();

    std::cout << "all query surface tests passed\n";
    return 0;
}
