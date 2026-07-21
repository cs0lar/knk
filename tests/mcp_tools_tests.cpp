#include <cassert>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>

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
                                         "commit_retraction",
                                         "commit_superseding",
                                         "write_snapshot",
                                         "intern_entity",
                                         "intern_value",
                                         "intern_predicate",
                                         "intern_document",
                                         "record_provenance",
                                         "commit_hypothesis",
                                         "merge_entities",
                                         "archive_segments_before",
                                         "get",
                                         "assertions_for_subject",
                                         "current",
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
                                         "hypotheses_for",
                                         "neighbors",
                                         "co_occurring_predicates",
                                         "resolve_entity"};

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
    commit_tool_round_trips_ids_timestamps_and_confidence();
    intern_entity_tool_round_trips_a_string_argument();
    intern_value_tool_round_trips_a_tagged_value_argument();
    intern_document_tool_round_trips_base64_bytes();
    current_by_object_and_current_by_predicate_tools_round_trip();
    neighbors_tool_round_trips_a_size_t_argument();
    get_tool_returns_null_for_an_unknown_assertion();
    find_conflicts_tool_returns_paired_assertions();
    unknown_tool_name_is_reported_as_a_tool_error();
    missing_required_argument_is_reported_as_a_tool_error_not_a_crash();
    kernel_exception_from_execute_is_reported_as_a_tool_error_not_a_crash();

    std::cout << "All mcp_tools tests passed.\n";
    return 0;
}
