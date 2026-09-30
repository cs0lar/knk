#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kernel/knowledge_kernel.hpp"
#include "kernel/mcp_tools.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;
using namespace knk::mcp;

namespace {

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("mcp_tools_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

// Every KernelCommand variant (see kernel_command.hpp) must have exactly one corresponding tool,
// with a non-empty description and an object-typed input schema -- this is the completeness check
// that would catch a forgotten tool the next time a command is added.
void tool_specs_cover_every_kernel_command_with_a_well_formed_schema() {
    std::set<std::string> expected_names{"commit",
                                         "commit_by_name",
                                         "commit_batch",
                                         "commit_batch_by_name",
                                         "commit_retraction",
                                         "commit_superseding",
                                         "write_snapshot",
                                         "intern_entity",
                                         "intern_value",
                                         "intern_predicate",
                                         "intern_document",
                                         "record_provenance",
                                         "record_provenance_batch",
                                         "commit_hypothesis",
                                         "merge_entities",
                                         "archive_segments_before",
                                         "get",
                                         "assertions_for_subject",
                                         "current",
                                         "current_by_name",
                                         "current_by_object",
                                         "current_by_predicate",
                                         "valid_at",
                                         "known_at",
                                         "valid_at_known_at",
                                         "valid_time_timeline",
                                         "observed_time_timeline",
                                         "commit_history",
                                         "changes_since",
                                         "explain",
                                         "find_conflicts",
                                         "find_entity",
                                         "find_value",
                                         "find_predicate",
                                         "entity_name",
                                         "entity_value",
                                         "predicate_name",
                                         "document_content",
                                         "provenance_for",
                                         "entity_name_batch",
                                         "entity_value_batch",
                                         "predicate_name_batch",
                                         "provenance_for_batch",
                                         "hypotheses_for",
                                         "neighbors",
                                         "co_occurring_predicates",
                                         "resolve_entity",
                                         "query",
                                         "aggregate",
                                         "explain_query",
                                         "query_spill",
                                         "describe_predicates",
                                         "describe_corpus"};

    const auto &specs = tool_specs();
    assert(specs.size() == expected_names.size());

    std::set<std::string> actual_names;
    for (const auto &spec : specs) {
        assert(!spec.description.empty());
        assert(spec.input_schema.at("type") == "object");
        actual_names.insert(spec.name);
    }

    assert(actual_names == expected_names);
}

// Guards the wire-format invariant AGENTS.md's "MCP parameter ordering" rule documents: a
// treelang-style caller binds tool arguments positionally against the *emitted* "properties"
// order and takes a prefix for "required", so "required" must equal properties[0:len(required)]
// once the schema is actually serialized -- not just as declared in mcp_tools.cpp. This dumps each
// schema to a JSON string (as it would go over the wire) and re-parses with ordered_json, since
// re-parsing with plain nlohmann::json would re-sort keys and hide exactly the bug this test
// exists to catch (see #47: ToolSpec::input_schema used to be nlohmann::json, which silently
// resorted "properties" alphabetically on dump(), breaking assertions_for_subject, commit_history,
// and changes_since as soon as #43 gave them optional trailing parameters).
void every_tool_schema_emits_required_as_a_prefix_of_properties_in_declared_order() {
    for (const auto &spec : tool_specs()) {
        nlohmann::ordered_json wire = nlohmann::ordered_json::parse(spec.input_schema.dump());

        std::vector<std::string> required = wire.at("required").get<std::vector<std::string>>();
        std::vector<std::string> property_order;
        for (const auto &[name, prop] : wire.at("properties").items()) {
            property_order.push_back(name);
        }

        assert(required.size() <= property_order.size());
        std::vector<std::string> leading_properties(property_order.begin(), property_order.begin() + required.size());
        assert(leading_properties == required);
    }
}

void commit_tool_round_trips_ids_timestamps_and_confidence() {
    auto root = test_root("commit_tool_round_trips_ids_timestamps_and_confidence");
    KnowledgeKernel kernel(StorageConfig{root});

    nlohmann::json args{{"subject", 1},  {"predicate", 10},           {"object", 100},    {"valid_from", 1704067200},
                        {"valid_to", 0}, {"observed_at", 1719792000}, {"confidence", 0.9}};

    auto result = handle_tool_call(kernel, "commit", args);
    assert(!result.is_error);

    auto id_json = nlohmann::json::parse(result.content_text);
    AssertionId id = id_json.get<AssertionId>();

    auto committed = kernel.get(id);
    assert(committed.has_value());
    assert(committed->subject == 1);
    assert(committed->object == 100);
    assert(committed->confidence == 0.9);

    cleanup(root);
}

void commit_by_name_tool_interns_names_and_commits() {
    auto root = test_root("commit_by_name_tool_interns_names_and_commits");
    KnowledgeKernel kernel(StorageConfig{root});

    nlohmann::json args{{"subject_name", "Alice"},
                        {"predicate_name", "works_at"},
                        {"object", {{"kind", "text"}, {"value", "Acme"}}},
                        {"valid_from", 1704067200},
                        {"valid_to", 0},
                        {"observed_at", 1719792000},
                        {"confidence", 0.9}};

    auto result = handle_tool_call(kernel, "commit_by_name", args);
    assert(!result.is_error);

    AssertionId id = nlohmann::json::parse(result.content_text).get<AssertionId>();
    auto committed = kernel.get(id);
    assert(committed.has_value());
    assert(kernel.entity_name(committed->subject) == "Alice");
    assert(kernel.entity_name(committed->object) == "Acme");
    assert(kernel.predicate_name(committed->predicate) == "works_at");

    cleanup(root);
}

void commit_batch_tool_commits_every_entry_and_returns_ids_in_input_order() {
    auto root = test_root("commit_batch_tool_commits_every_entry_and_returns_ids_in_input_order");
    KnowledgeKernel kernel(StorageConfig{root});

    nlohmann::json args{{"entries",
                         {{{"subject", 1},
                           {"predicate", 10},
                           {"object", 100},
                           {"valid_from", 1704067200},
                           {"valid_to", 0},
                           {"observed_at", 1719792000},
                           {"confidence", 0.9}},
                          {{"subject", 2},
                           {"predicate", 10},
                           {"object", 100},
                           {"valid_from", 1672531200},
                           {"valid_to", 0},
                           {"observed_at", 1719792000},
                           {"confidence", 0.8}}}}};

    auto result = handle_tool_call(kernel, "commit_batch", args);
    assert(!result.is_error);

    auto ids = nlohmann::json::parse(result.content_text).get<std::vector<AssertionId>>();
    assert(ids.size() == 2);

    // Input order, and each entry's own valid_from -- the two properties a caller restating a field
    // across a population depends on.
    assert(kernel.get(ids[0])->subject == 1);
    assert(kernel.get(ids[0])->valid_from == 1704067200);
    assert(kernel.get(ids[1])->subject == 2);
    assert(kernel.get(ids[1])->valid_from == 1672531200);

    cleanup(root);
}

void commit_batch_tool_reports_a_malformed_entry_as_a_tool_error() {
    auto root = test_root("commit_batch_tool_reports_a_malformed_entry_as_a_tool_error");
    KnowledgeKernel kernel(StorageConfig{root});

    // Second entry is missing "confidence": the batch must be rejected whole, before any of it is
    // committed, and surface as an is_error result rather than throwing out of handle_tool_call.
    nlohmann::json args{{"entries",
                         {{{"subject", 1},
                           {"predicate", 10},
                           {"object", 100},
                           {"valid_from", 1704067200},
                           {"valid_to", 0},
                           {"observed_at", 1719792000},
                           {"confidence", 0.9}},
                          {{"subject", 2},
                           {"predicate", 10},
                           {"object", 100},
                           {"valid_from", 1672531200},
                           {"valid_to", 0},
                           {"observed_at", 1719792000}}}}};

    auto result = handle_tool_call(kernel, "commit_batch", args);
    assert(result.is_error);
    assert(!kernel.get(1).has_value());

    // A non-array "entries" is likewise a tool error, not a crash.
    auto not_an_array = handle_tool_call(kernel, "commit_batch", nlohmann::json{{"entries", 7}});
    assert(not_an_array.is_error);

    cleanup(root);
}

void commit_batch_by_name_tool_interns_names_and_commits_every_entry() {
    auto root = test_root("commit_batch_by_name_tool_interns_names_and_commits_every_entry");
    KnowledgeKernel kernel(StorageConfig{root});

    nlohmann::json args{{"entries",
                         {{{"subject_name", "Alice"},
                           {"predicate_name", "works_at"},
                           {"object", {{"kind", "text"}, {"value", "Acme"}}},
                           {"valid_from", 1704067200},
                           {"valid_to", 0},
                           {"observed_at", 1719792000},
                           {"confidence", 0.9}},
                          {{"subject_name", "Bob"},
                           {"predicate_name", "works_at"},
                           {"object", {{"kind", "int64"}, {"value", 42}}},
                           {"valid_from", 1672531200},
                           {"valid_to", 0},
                           {"observed_at", 1719792000},
                           {"confidence", 0.8}}}}};

    auto result = handle_tool_call(kernel, "commit_batch_by_name", args);
    assert(!result.is_error);

    auto ids = nlohmann::json::parse(result.content_text).get<std::vector<AssertionId>>();
    assert(ids.size() == 2);

    assert(kernel.entity_name(kernel.get(ids[0])->subject) == "Alice");
    assert(kernel.entity_name(kernel.get(ids[0])->object) == "Acme");
    assert(kernel.predicate_name(kernel.get(ids[0])->predicate) == "works_at");
    assert(kernel.get(ids[0])->valid_from == 1704067200);

    // A tagged non-text object survives the wire as a literal, same as commit_by_name's object.
    assert(kernel.entity_value(kernel.get(ids[1])->object) == Value::of_int64(42));
    assert(kernel.get(ids[1])->valid_from == 1672531200);

    // A malformed entry is a tool error, and nothing is committed.
    auto bad =
        handle_tool_call(kernel, "commit_batch_by_name", nlohmann::json{{"entries", {{{"subject_name", "Dana"}}}}});
    assert(bad.is_error);
    assert(!kernel.find_entity("Dana").has_value());

    cleanup(root);
}

void record_provenance_batch_tool_records_every_entry_and_rejects_an_unknown_target() {
    auto root = test_root("record_provenance_batch_tool_records_every_entry_and_rejects_an_unknown_target");
    KnowledgeKernel kernel(StorageConfig{root});

    EntityId source = kernel.intern_entity("migration_job");
    auto ids = kernel.commit_batch(
        {{1, 10, 100, 0, OPEN_ENDED, 1719792000, 0.9}, {2, 10, 100, 0, OPEN_ENDED, 1719792000, 0.9}});

    nlohmann::json args{{"records",
                         {{{"assertion_id", ids[0]},
                           {"source", source},
                           {"recorded_at", 1719792000},
                           {"method", "restated_from_legacy_field"}},
                          {{"assertion_id", ids[1]},
                           {"source", source},
                           {"recorded_at", 1719792000},
                           {"method", "restated_from_legacy_field"}}}}};

    auto result = handle_tool_call(kernel, "record_provenance_batch", args);
    assert(!result.is_error);
    assert(kernel.provenance_for(ids[0])->source == source);
    assert(kernel.provenance_for(ids[1])->method == "restated_from_legacy_field");

    // An unknown target comes back as a tool error, not a throw, and writes nothing -- including the
    // valid record listed alongside it.
    auto bad = handle_tool_call(
        kernel, "record_provenance_batch",
        nlohmann::json{
            {"records",
             {{{"assertion_id", ids[0]}, {"source", source}, {"recorded_at", 1720450412}, {"method", "later"}},
              {{"assertion_id", 999}, {"source", source}, {"recorded_at", 1720450412}, {"method", "later"}}}}});
    assert(bad.is_error);
    assert(kernel.provenance_for(ids[0])->method == "restated_from_legacy_field");

    cleanup(root);
}

void batch_read_tools_answer_in_input_order_with_null_slots() {
    auto root = test_root("batch_read_tools_answer_in_input_order_with_null_slots");
    KnowledgeKernel kernel(StorageConfig{root});

    EntityId alice = kernel.intern_entity("Alice");
    EntityId number = kernel.intern_value(Value::of_int64(42));
    PredicateId works_at = kernel.intern_predicate("works_at");
    AssertionId id = kernel.commit(alice, works_at, number, 0, OPEN_ENDED, 1719792000, 0.9);
    kernel.record_provenance(id, alice, 1719792000, "manual_entry");

    auto call = [&](const std::string &tool, const nlohmann::json &args) {
        auto result = handle_tool_call(kernel, tool, args);
        assert(!result.is_error);
        return nlohmann::json::parse(result.content_text);
    };

    // A literal has no name, and 999 was never interned: both come back as null in their own slot,
    // not dropped, so the answer lines up with the ids it was asked for.
    auto names = call("entity_name_batch", nlohmann::json{{"ids", {alice, number, 999}}});
    assert(names.size() == 3);
    assert(names[0] == "Alice");
    assert(names[1].is_null());
    assert(names[2].is_null());

    auto values = call("entity_value_batch", nlohmann::json{{"ids", {number, 999}}});
    assert(values.size() == 2);
    assert(values[0].at("kind") == "int64");
    assert(values[0].at("value") == 42);
    assert(values[1].is_null());

    auto predicate_names = call("predicate_name_batch", nlohmann::json{{"ids", {works_at, 999}}});
    assert(predicate_names.size() == 2);
    assert(predicate_names[0] == "works_at");
    assert(predicate_names[1].is_null());

    auto provenance = call("provenance_for_batch", nlohmann::json{{"assertion_ids", {id, 999}}});
    assert(provenance.size() == 2);
    assert(provenance[0].at("method") == "manual_entry");
    assert(provenance[1].is_null());

    // A non-array list, or a non-integer element in one, is a tool error rather than a partial answer.
    assert(handle_tool_call(kernel, "entity_name_batch", nlohmann::json{{"ids", 7}}).is_error);
    assert(handle_tool_call(kernel, "provenance_for_batch", nlohmann::json{{"assertion_ids", {id, "x"}}}).is_error);

    cleanup(root);
}

void query_tool_runs_a_shaped_read_and_reports_truncation() {
    auto root = test_root("query_tool_runs_a_shaped_read_and_reports_truncation");
    KnowledgeKernel kernel(StorageConfig{root});

    EntityId alice = kernel.intern_entity("Alice");
    PredicateId works_at = kernel.intern_predicate("works_at");
    EntityId acme = kernel.intern_entity("Acme");
    EntityId beta = kernel.intern_entity("Beta");

    AssertionId closed = kernel.commit(alice, works_at, acme, 0, 1672531200, 1719792000, 0.9);
    AssertionId open = kernel.commit(alice, works_at, beta, 1672531200, OPEN_ENDED, 1719878400, 0.95);

    // The current-shaped query over the wire: Active plus open-ended excludes the closed interval.
    auto result = handle_tool_call(
        kernel, "query", nlohmann::json{{"subject", alice}, {"open_ended_only", true}, {"statuses", {"Active"}}});
    assert(!result.is_error);

    auto answer = nlohmann::json::parse(result.content_text);
    assert(answer.at("truncated") == false);
    assert(answer.at("assertions").size() == 1);
    assert(answer.at("assertions")[0].at("id") == open);

    // An unfiltered query returns both rows, and limit surfaces truncation.
    auto all = nlohmann::json::parse(handle_tool_call(kernel, "query", nlohmann::json::object()).content_text);
    assert(all.at("assertions").size() == 2);

    auto capped = nlohmann::json::parse(handle_tool_call(kernel, "query", nlohmann::json{{"limit", 1}}).content_text);
    assert(capped.at("assertions").size() == 1);
    assert(capped.at("truncated") == true);
    assert(capped.at("assertions")[0].at("id") == closed); // default order is id-ascending

    // Malformed arguments are tool errors, not throws: an unknown status name, an unknown order, a
    // non-array statuses, and an ir_version this build does not know.
    assert(handle_tool_call(kernel, "query", nlohmann::json{{"statuses", {"Nonsense"}}}).is_error);
    assert(handle_tool_call(kernel, "query", nlohmann::json{{"order", "sideways"}}).is_error);
    assert(handle_tool_call(kernel, "query", nlohmann::json{{"statuses", 7}}).is_error);
    assert(handle_tool_call(kernel, "query", nlohmann::json{{"ir_version", 99}}).is_error);

    cleanup(root);
}

void query_tool_accepts_filters_and_name_resolution() {
    auto root = test_root("query_tool_accepts_filters_and_name_resolution");
    KnowledgeKernel kernel(StorageConfig{root});

    EntityId alice = kernel.intern_entity("Alice");
    PredicateId works_at = kernel.intern_predicate("works_at");
    EntityId acme = kernel.intern_entity("Acme");
    EntityId forty_two = kernel.intern_value(Value::of_int64(42));

    AssertionId high = kernel.commit(alice, works_at, acme, 0, OPEN_ENDED, 1719792000, 0.95);
    kernel.commit(alice, works_at, forty_two, 0, OPEN_ENDED, 1719878400, 0.20);

    // A boolean tree over the wire: confidence >= 0.9 AND status == Active.
    nlohmann::json filter{{"kind", "and"},
                          {"children",
                           {{{"kind", "comparison"},
                             {"field", "confidence"},
                             {"op", "gte"},
                             {"value", {{"kind", "double"}, {"value", 0.9}}}},
                            {{"kind", "comparison"},
                             {"field", "status"},
                             {"op", "eq"},
                             {"value", {{"kind", "text"}, {"value", "Active"}}}}}}};

    auto result = handle_tool_call(kernel, "query", nlohmann::json{{"filter", filter}});
    assert(!result.is_error);

    auto answer = nlohmann::json::parse(result.content_text);
    assert(answer.at("assertions").size() == 1);
    assert(answer.at("assertions")[0].at("id") == high);
    assert(!answer.contains("names")); // absent unless asked for

    // Name resolution rides alongside the rows, one slot per returned row.
    auto resolved =
        nlohmann::json::parse(handle_tool_call(kernel, "query", nlohmann::json{{"resolve_names", true}}).content_text);
    assert(resolved.at("names").size() == resolved.at("assertions").size());
    assert(resolved.at("names")[0].at("subject_name") == "Alice");
    assert(resolved.at("names")[0].at("predicate_name") == "works_at");
    assert(resolved.at("names")[0].at("object_value").at("kind") == "text");
    assert(resolved.at("names")[1].at("object_value").at("kind") == "int64");
    assert(resolved.at("names")[1].at("object_value").at("value") == 42);

    // A comparison against the object's value, not its id.
    nlohmann::json by_value{
        {"kind", "comparison"}, {"field", "object_value"}, {"op", "gt"}, {"value", {{"kind", "int64"}, {"value", 10}}}};
    auto numeric =
        nlohmann::json::parse(handle_tool_call(kernel, "query", nlohmann::json{{"filter", by_value}}).content_text);
    assert(numeric.at("assertions").size() == 1);
    assert(numeric.at("assertions")[0].at("object") == forty_two);

    // Malformed trees are tool errors, not throws: unknown field, unknown op, unknown kind, a
    // non-object filter, and an operand whose kind does not match its field.
    auto bad = [&kernel](const nlohmann::json &filter) {
        return handle_tool_call(kernel, "query", nlohmann::json{{"filter", filter}}).is_error;
    };

    assert(bad(nlohmann::json{
        {"kind", "comparison"}, {"field", "nonsense"}, {"op", "eq"}, {"value", {{"kind", "int64"}, {"value", 1}}}}));
    assert(bad(nlohmann::json{{"kind", "comparison"},
                              {"field", "confidence"},
                              {"op", "sideways"},
                              {"value", {{"kind", "double"}, {"value", 1.0}}}}));
    assert(bad(nlohmann::json{{"kind", "nonsense"}, {"children", nlohmann::json::array()}}));
    assert(bad(nlohmann::json(7)));
    assert(bad(nlohmann::json{
        {"kind", "comparison"}, {"field", "confidence"}, {"op", "eq"}, {"value", {{"kind", "int64"}, {"value", 1}}}}));
    assert(bad(nlohmann::json{{"kind", "and"}, {"children", nlohmann::json::array()}}));

    cleanup(root);
}

void aggregate_tool_groups_rows_and_reports_absent_numbers_as_null() {
    auto root = test_root("aggregate_tool_groups_rows_and_reports_absent_numbers_as_null");
    KnowledgeKernel kernel(StorageConfig{root});

    EntityId alice = kernel.intern_entity("Alice");
    EntityId bob = kernel.intern_entity("Bob");
    PredicateId salary = kernel.intern_predicate("salary");
    PredicateId dept = kernel.intern_predicate("dept");
    EntityId eng = kernel.intern_entity("eng");

    kernel.commit(alice, salary, kernel.intern_value(Value::of_int64(120000)), 0, OPEN_ENDED, 100, 0.9);
    kernel.commit(bob, salary, kernel.intern_value(Value::of_int64(90000)), 0, OPEN_ENDED, 200, 0.6);
    kernel.commit(alice, dept, eng, 0, OPEN_ENDED, 100, 1.0);

    // Average salary, grouped by predicate: the salary group averages two numbers, the dept group has
    // a text object and so has nothing to average.
    nlohmann::json args{{"aggregations",
                         {{{"function", "avg"}, {"target", "object_value"}},
                          {{"function", "count"}},
                          {{"function", "count_distinct"}, {"target", "subject"}}}},
                        {"group_by", {{{"field", "predicate"}}}}};

    auto result = handle_tool_call(kernel, "aggregate", args);
    assert(!result.is_error);

    auto answer = nlohmann::json::parse(result.content_text);
    assert(answer.at("groups").size() == 2);

    const auto &salary_group = answer.at("groups")[0];
    assert(salary_group.at("key")[0].at("value") == salary);
    assert(salary_group.at("row_count") == 2);
    assert(salary_group.at("values")[0] == 105000.0); // (120000 + 90000) / 2
    assert(salary_group.at("values")[1] == 2);
    assert(salary_group.at("values")[2] == 2); // two distinct subjects

    const auto &dept_group = answer.at("groups")[1];
    assert(dept_group.at("key")[0].at("value") == dept);
    assert(dept_group.at("row_count") == 1);
    assert(dept_group.at("values")[0].is_null()); // nothing numeric to average
    assert(dept_group.at("values")[1] == 1);      // but the row is still counted

    // A global aggregate has an empty key.
    auto global = nlohmann::json::parse(
        handle_tool_call(kernel, "aggregate", nlohmann::json{{"aggregations", {{{"function", "count"}}}}})
            .content_text);
    assert(global.at("groups").size() == 1);
    assert(global.at("groups")[0].at("key").empty());
    assert(global.at("groups")[0].at("values")[0] == 3);

    // Caller mistakes come back as tool errors rather than throwing.
    auto bad = [&kernel](const nlohmann::json &args) { return handle_tool_call(kernel, "aggregate", args).is_error; };

    assert(bad(nlohmann::json::object())); // aggregations is required
    assert(bad(nlohmann::json{{"aggregations", {{{"function", "nonsense"}}}}}));
    assert(bad(nlohmann::json{{"aggregations", {{{"function", "sum"}, {"target", "nonsense"}}}}}));
    assert(bad(nlohmann::json{{"aggregations", {{{"function", "count"}}}},
                              {"group_by", {{{"field", "observed_at_bucket"}}}}}));         // no bucket_width
    assert(bad(nlohmann::json{{"aggregations", {{{"function", "count"}}}}, {"limit", 5}})); // row-shaping

    cleanup(root);
}

void explain_query_tool_reports_the_plan_and_its_alternatives() {
    auto root = test_root("explain_query_tool_reports_the_plan");
    KnowledgeKernel kernel(StorageConfig{root});

    PredicateId works_at = kernel.intern_predicate("works_at");
    EntityId acme = kernel.intern_entity("Acme");

    std::vector<PendingAssertion> rows;
    for (size_t i = 0; i < 300; ++i) {
        rows.push_back({static_cast<EntityId>(i + 10), works_at, acme, 0, OPEN_ENDED, static_cast<Timestamp>(i), 0.9});
    }
    kernel.commit_batch(rows);

    // A selective subject: the index wins, and the plan says so with an exact row count.
    auto selective = nlohmann::json::parse(
        handle_tool_call(kernel, "explain_query",
                         nlohmann::json{{"subject", 10}, {"statuses", {"Active"}}, {"open_ended_only", true}})
            .content_text);
    assert(selective.at("chosen") == "subject_index");
    assert(selective.at("estimated_rows") == 1);
    assert(selective.at("total_rows") == 300);

    // A predicate covering the whole corpus: scanned, with the index still listed and costed so a caller
    // can see it was considered rather than forgotten.
    auto common = nlohmann::json::parse(
        handle_tool_call(kernel, "explain_query",
                         nlohmann::json{{"predicate", works_at}, {"statuses", {"Active"}}, {"open_ended_only", true}})
            .content_text);
    assert(common.at("chosen") == "columnar_scan");

    bool saw_predicate_index = false;
    for (const auto &option : common.at("considered")) {
        if (option.at("source") == "predicate_current_index") {
            saw_predicate_index = true;
            assert(option.at("rows") == 300);
            assert(option.at("cost") > common.at("estimated_cost").get<double>());
        }
    }
    assert(saw_predicate_index);

    // Rejections carry a reason instead of numbers.
    auto audit = nlohmann::json::parse(
        handle_tool_call(kernel, "explain_query", nlohmann::json{{"predicate", works_at}}).content_text);
    for (const auto &option : audit.at("considered")) {
        if (option.at("source") == "predicate_current_index") {
            assert(option.contains("rejected_because"));
            assert(!option.contains("rows"));
        }
    }

    // Explaining validates exactly as running does.
    assert(handle_tool_call(kernel, "explain_query", nlohmann::json{{"ir_version", 99}}).is_error);

    cleanup(root);
}

void batch_tool_schemas_state_their_bound() {
    // The issue these tools answer asks for the batch bound to be discoverable from the tool itself,
    // not just enforced at call time -- so both the description and the schema must carry it, for
    // every batch tool.
    std::set<std::string> checked;

    // Each batch tool's list parameter, and the type of one element: the write batches take objects,
    // the batch reads take bare ids. Every other property of the bound is identical across all of them.
    const std::map<std::string, std::pair<std::string, std::string>> batch_tools{
        {"commit_batch", {"entries", "object"}},
        {"commit_batch_by_name", {"entries", "object"}},
        {"record_provenance_batch", {"records", "object"}},
        {"entity_name_batch", {"ids", "integer"}},
        {"entity_value_batch", {"ids", "integer"}},
        {"predicate_name_batch", {"ids", "integer"}},
        {"provenance_for_batch", {"assertion_ids", "integer"}},
    };

    for (const auto &spec : tool_specs()) {
        auto batch = batch_tools.find(spec.name);
        if (batch == batch_tools.end()) {
            continue;
        }

        const auto &[list_property, item_type] = batch->second;

        assert(spec.description.find(std::to_string(KnowledgeKernel::MAX_BATCH_SIZE)) != std::string::npos);

        const auto &list = spec.input_schema.at("properties").at(list_property);
        assert(list.at("type") == "array");
        assert(list.at("maxItems") == KnowledgeKernel::MAX_BATCH_SIZE);
        assert(list.at("items").at("type") == item_type);

        checked.insert(spec.name);
    }

    assert(checked.size() == batch_tools.size());
}

void intern_entity_tool_round_trips_a_string_argument() {
    auto root = test_root("intern_entity_tool_round_trips_a_string_argument");
    KnowledgeKernel kernel(StorageConfig{root});

    auto result = handle_tool_call(kernel, "intern_entity", nlohmann::json{{"name", "Alice"}});
    assert(!result.is_error);

    EntityId id = nlohmann::json::parse(result.content_text).get<EntityId>();
    assert(kernel.entity_name(id) == "Alice");

    cleanup(root);
}

void intern_value_tool_round_trips_a_tagged_value_argument() {
    auto root = test_root("intern_value_tool_round_trips_a_tagged_value_argument");
    KnowledgeKernel kernel(StorageConfig{root});

    nlohmann::json value_arg{{"value", {{"kind", "int64"}, {"value", 42}}}};
    auto result = handle_tool_call(kernel, "intern_value", value_arg);
    assert(!result.is_error);

    EntityId id = nlohmann::json::parse(result.content_text).get<EntityId>();
    assert(kernel.entity_value(id) == Value::of_int64(42));

    cleanup(root);
}

void intern_document_tool_round_trips_base64_bytes() {
    auto root = test_root("intern_document_tool_round_trips_base64_bytes");
    KnowledgeKernel kernel(StorageConfig{root});

    // "hi" base64-encoded, matching json_codec_tests' known vector.
    auto result = handle_tool_call(kernel, "intern_document", nlohmann::json{{"content", "aGk="}});
    assert(!result.is_error);

    EntityId id = nlohmann::json::parse(result.content_text).get<EntityId>();
    auto content = kernel.document_content(id);
    assert(content.has_value());
    assert(content->size() == 2);
    assert(static_cast<char>((*content)[0]) == 'h');
    assert(static_cast<char>((*content)[1]) == 'i');

    cleanup(root);
}

void current_by_name_tool_resolves_the_named_subject() {
    auto root = test_root("current_by_name_tool_resolves_the_named_subject");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit_by_name("Alice", "works_at", Value::of_text("Acme"), 1704067200, 0, 1719792000, 0.95);

    auto result = handle_tool_call(kernel, "current_by_name", nlohmann::json{{"subject_name", "Alice"}});
    assert(!result.is_error);

    auto current_json = nlohmann::json::parse(result.content_text);
    assert(current_json.is_array());
    assert(current_json.size() == 1);

    auto unknown = handle_tool_call(kernel, "current_by_name", nlohmann::json{{"subject_name", "Nobody"}});
    assert(!unknown.is_error);
    assert(nlohmann::json::parse(unknown.content_text).is_array());
    assert(nlohmann::json::parse(unknown.content_text).empty());

    cleanup(root);
}

void current_by_object_and_current_by_predicate_tools_round_trip() {
    auto root = test_root("current_by_object_and_current_by_predicate_tools_round_trip");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(1, 10, 100, 1704067200, 0, 1719792000, 0.9);

    auto by_object = handle_tool_call(kernel, "current_by_object", nlohmann::json{{"object", 100}});
    assert(!by_object.is_error);
    auto by_object_json = nlohmann::json::parse(by_object.content_text);
    assert(by_object_json.is_array());
    assert(by_object_json.size() == 1);

    auto by_predicate = handle_tool_call(kernel, "current_by_predicate", nlohmann::json{{"predicate", 10}});
    assert(!by_predicate.is_error);
    auto by_predicate_json = nlohmann::json::parse(by_predicate.content_text);
    assert(by_predicate_json.is_array());
    assert(by_predicate_json.size() == 1);

    cleanup(root);
}

void neighbors_tool_round_trips_a_size_t_argument() {
    auto root = test_root("neighbors_tool_round_trips_a_size_t_argument");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(1, 10, 100, 1704067200, 0, 1719792000, 0.9);

    auto result = handle_tool_call(kernel, "neighbors", nlohmann::json{{"subject", 1}, {"max_hops", 1}});
    assert(!result.is_error);

    auto neighbors_json = nlohmann::json::parse(result.content_text);
    assert(neighbors_json.is_array());
    assert(neighbors_json.size() == 1);
    assert(neighbors_json[0] == 100);

    cleanup(root);
}

void changes_since_tool_supports_optional_limit_and_newest_first() {
    auto root = test_root("changes_since_tool_supports_optional_limit_and_newest_first");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(1, 10, 100, 1704067200, 0, 1719792000, 0.9);
    AssertionId latest = kernel.commit(1, 10, 200, 1704067200, 0, 1719792001, 0.9);

    // limit/newest_first are optional -- omitting them preserves the pre-existing behavior.
    auto unbounded = handle_tool_call(kernel, "changes_since", nlohmann::json{{"observed_since", 0}});
    assert(!unbounded.is_error);
    assert(nlohmann::json::parse(unbounded.content_text).size() == 2);

    auto latest_only = handle_tool_call(kernel, "changes_since",
                                        nlohmann::json{{"observed_since", 0}, {"limit", 1}, {"newest_first", true}});
    assert(!latest_only.is_error);
    auto latest_only_json = nlohmann::json::parse(latest_only.content_text);
    assert(latest_only_json.size() == 1);
    assert(latest_only_json[0].at("id").get<AssertionId>() == latest);

    cleanup(root);
}

void assertions_for_subject_and_commit_history_tools_support_optional_limit() {
    auto root = test_root("assertions_for_subject_and_commit_history_tools_support_optional_limit");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(1, 10, 100, 1704067200, 0, 1719792000, 0.9);
    kernel.commit(1, 10, 200, 1704067200, 0, 1719792001, 0.9);

    auto subject_limited =
        handle_tool_call(kernel, "assertions_for_subject", nlohmann::json{{"subject", 1}, {"limit", 1}});
    assert(!subject_limited.is_error);
    assert(nlohmann::json::parse(subject_limited.content_text).size() == 1);

    auto history_limited =
        handle_tool_call(kernel, "commit_history", nlohmann::json{{"subject", 1}, {"predicate", 10}, {"limit", 1}});
    assert(!history_limited.is_error);
    assert(nlohmann::json::parse(history_limited.content_text).size() == 1);

    cleanup(root);
}

void get_tool_returns_null_for_an_unknown_assertion() {
    auto root = test_root("get_tool_returns_null_for_an_unknown_assertion");
    KnowledgeKernel kernel(StorageConfig{root});

    auto result = handle_tool_call(kernel, "get", nlohmann::json{{"id", 999}});
    assert(!result.is_error);
    assert(nlohmann::json::parse(result.content_text).is_null());

    cleanup(root);
}

void find_conflicts_tool_returns_paired_assertions() {
    auto root = test_root("find_conflicts_tool_returns_paired_assertions");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(1, 10, 100, 1704067200, 0, 1719792000, 0.5);
    kernel.commit(1, 10, 200, 1704067200, 0, 1719792000, 0.5);

    auto result = handle_tool_call(kernel, "find_conflicts", nlohmann::json{{"subject", 1}, {"predicate", 10}});
    assert(!result.is_error);

    auto conflicts_json = nlohmann::json::parse(result.content_text);
    assert(conflicts_json.is_array());
    assert(conflicts_json.size() == 1);
    assert(conflicts_json[0].contains("a"));
    assert(conflicts_json[0].contains("b"));

    cleanup(root);
}

void unknown_tool_name_is_reported_as_a_tool_error() {
    auto root = test_root("unknown_tool_name_is_reported_as_a_tool_error");
    KnowledgeKernel kernel(StorageConfig{root});

    auto result = handle_tool_call(kernel, "not_a_real_tool", nlohmann::json::object());
    assert(result.is_error);

    cleanup(root);
}

void missing_required_argument_is_reported_as_a_tool_error_not_a_crash() {
    auto root = test_root("missing_required_argument_is_reported_as_a_tool_error_not_a_crash");
    KnowledgeKernel kernel(StorageConfig{root});

    auto result = handle_tool_call(kernel, "commit", nlohmann::json{{"subject", 1}});
    assert(result.is_error);

    cleanup(root);
}

void kernel_exception_from_execute_is_reported_as_a_tool_error_not_a_crash() {
    auto root = test_root("kernel_exception_from_execute_is_reported_as_a_tool_error_not_a_crash");
    KnowledgeKernel kernel(StorageConfig{root});

    // retracting a nonexistent assertion id throws inside KnowledgeKernel::commit_retraction.
    nlohmann::json args{{"subject", 1},  {"predicate", 10},           {"object", 100},     {"valid_from", 1704067200},
                        {"valid_to", 0}, {"observed_at", 1719792000}, {"confidence", 0.9}, {"retracts_id", 999}};

    auto result = handle_tool_call(kernel, "commit_retraction", args);
    assert(result.is_error);

    cleanup(root);
}

} // namespace

int main() {
    tool_specs_cover_every_kernel_command_with_a_well_formed_schema();
    every_tool_schema_emits_required_as_a_prefix_of_properties_in_declared_order();
    commit_tool_round_trips_ids_timestamps_and_confidence();
    commit_by_name_tool_interns_names_and_commits();
    current_by_name_tool_resolves_the_named_subject();
    commit_batch_tool_commits_every_entry_and_returns_ids_in_input_order();
    commit_batch_tool_reports_a_malformed_entry_as_a_tool_error();
    commit_batch_by_name_tool_interns_names_and_commits_every_entry();
    record_provenance_batch_tool_records_every_entry_and_rejects_an_unknown_target();
    batch_read_tools_answer_in_input_order_with_null_slots();
    query_tool_runs_a_shaped_read_and_reports_truncation();
    query_tool_accepts_filters_and_name_resolution();
    aggregate_tool_groups_rows_and_reports_absent_numbers_as_null();
    explain_query_tool_reports_the_plan_and_its_alternatives();
    batch_tool_schemas_state_their_bound();
    intern_entity_tool_round_trips_a_string_argument();
    intern_value_tool_round_trips_a_tagged_value_argument();
    intern_document_tool_round_trips_base64_bytes();
    current_by_object_and_current_by_predicate_tools_round_trip();
    neighbors_tool_round_trips_a_size_t_argument();
    changes_since_tool_supports_optional_limit_and_newest_first();
    assertions_for_subject_and_commit_history_tools_support_optional_limit();
    get_tool_returns_null_for_an_unknown_assertion();
    find_conflicts_tool_returns_paired_assertions();
    unknown_tool_name_is_reported_as_a_tool_error();
    missing_required_argument_is_reported_as_a_tool_error_not_a_crash();
    kernel_exception_from_execute_is_reported_as_a_tool_error_not_a_crash();

    std::cout << "All mcp_tools tests passed.\n";
    return 0;
}
