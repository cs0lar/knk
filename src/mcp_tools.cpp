#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "kernel/json_codec.hpp"
#include "kernel/kernel_command.hpp"
#include "kernel/mcp_tools.hpp"

namespace knk::mcp {

namespace {

// These return nlohmann::ordered_json, not json: object_schema below assembles them into a
// "properties" object whose *emitted* key order is a positional calling convention for treelang-
// style callers (see AGENTS.md's "MCP parameter ordering" rule). json's default object is
// std::map-backed and re-sorts keys alphabetically on serialization, silently breaking that
// convention the moment a schema has both required and optional parameters; ordered_json preserves
// insertion order through dump().

nlohmann::ordered_json integer_property(const std::string &description) {
    return nlohmann::ordered_json{{"type", "integer"}, {"description", description}};
}

nlohmann::ordered_json number_property(const std::string &description) {
    return nlohmann::ordered_json{{"type", "number"}, {"description", description}};
}

nlohmann::ordered_json string_property(const std::string &description) {
    return nlohmann::ordered_json{{"type", "string"}, {"description", description}};
}

nlohmann::ordered_json boolean_property(const std::string &description) {
    return nlohmann::ordered_json{{"type", "boolean"}, {"description", description}};
}

nlohmann::ordered_json base64_string_property(const std::string &description) {
    return nlohmann::ordered_json{{"type", "string"}, {"description", description + " (base64-encoded bytes)"}};
}

nlohmann::ordered_json value_property(const std::string &description) {
    return nlohmann::ordered_json{
        {"type", "object"},
        {"description", description},
        {"properties",
         {{"kind", {{"type", "string"}, {"enum", {"text", "int64", "double", "bool", "timestamp"}}}},
          {"value", {{"description", "Interpreted according to kind."}}}}},
        {"required", nlohmann::ordered_json::array({"kind", "value"})}};
}

nlohmann::ordered_json object_property(const std::string &description) {
    // Deliberately shape-less beyond "an object": the filter tree is recursive, and a $ref-based
    // recursive JSON Schema is handled inconsistently across MCP clients, so the shape is specified in
    // the description instead and enforced by the parser.
    return nlohmann::ordered_json{{"type", "object"}, {"description", description}};
}

nlohmann::ordered_json array_property(const std::string &description, size_t max_items, nlohmann::ordered_json items) {
    nlohmann::ordered_json schema;
    schema["type"] = "array";
    schema["description"] = description;
    schema["maxItems"] = max_items;
    schema["items"] = std::move(items);
    return schema;
}

nlohmann::ordered_json object_schema(std::vector<std::pair<std::string, nlohmann::ordered_json>> properties,
                                     std::vector<std::string> required) {
    nlohmann::ordered_json properties_json = nlohmann::ordered_json::object();
    for (auto &[name, prop] : properties) {
        properties_json[name] = std::move(prop);
    }

    nlohmann::ordered_json schema;
    schema["type"] = "object";
    schema["properties"] = std::move(properties_json);
    schema["required"] = std::move(required);
    return schema;
}

uint64_t require_id(const nlohmann::json &args, const char *key) { return args.at(key).get<uint64_t>(); }

Timestamp require_timestamp(const nlohmann::json &args, const char *key) { return args.at(key).get<Timestamp>(); }

double require_double(const nlohmann::json &args, const char *key) { return args.at(key).get<double>(); }

std::string require_string(const nlohmann::json &args, const char *key) { return args.at(key).get<std::string>(); }

size_t require_size(const nlohmann::json &args, const char *key) { return args.at(key).get<size_t>(); }

// Unlike the require_* helpers, these tolerate a missing (or explicit null) key, returning
// default_value instead -- for optional tool arguments like limit/newest_first, where omitting the
// argument should mean "same as before this parameter existed," not a parse error.
size_t optional_size(const nlohmann::json &args, const char *key, size_t default_value) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return default_value;
    }
    return args.at(key).get<size_t>();
}

bool optional_bool(const nlohmann::json &args, const char *key, bool default_value) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return default_value;
    }
    return args.at(key).get<bool>();
}

Value require_value(const nlohmann::json &args, const char *key) { return value_from_json(args.at(key)); }

std::vector<PendingAssertion> require_pending_assertions(const nlohmann::json &args, const char *key) {
    const auto &entries = args.at(key);
    if (!entries.is_array()) {
        throw std::runtime_error("entries must be an array");
    }

    std::vector<PendingAssertion> result;
    result.reserve(entries.size());

    // Each entry is validated by the same require_* helpers a single-assertion tool uses, so a
    // malformed entry anywhere in the list throws before kernel.execute is reached and the batch never
    // starts -- handle_tool_call turns that into an ordinary is_error result.
    for (const auto &entry : entries) {
        result.push_back(PendingAssertion{require_id(entry, "subject"), require_id(entry, "predicate"),
                                          require_id(entry, "object"), require_timestamp(entry, "valid_from"),
                                          require_timestamp(entry, "valid_to"), require_timestamp(entry, "observed_at"),
                                          require_double(entry, "confidence")});
    }

    return result;
}

std::vector<PendingNamedAssertion> require_pending_named_assertions(const nlohmann::json &args, const char *key) {
    const auto &entries = args.at(key);
    if (!entries.is_array()) {
        throw std::runtime_error("entries must be an array");
    }

    std::vector<PendingNamedAssertion> result;
    result.reserve(entries.size());

    for (const auto &entry : entries) {
        result.push_back(
            PendingNamedAssertion{require_string(entry, "subject_name"), require_string(entry, "predicate_name"),
                                  require_value(entry, "object"), require_timestamp(entry, "valid_from"),
                                  require_timestamp(entry, "valid_to"), require_timestamp(entry, "observed_at"),
                                  require_double(entry, "confidence")});
    }

    return result;
}

std::vector<ProvenanceRecord> require_provenance_records(const nlohmann::json &args, const char *key) {
    const auto &records = args.at(key);
    if (!records.is_array()) {
        throw std::runtime_error("records must be an array");
    }

    std::vector<ProvenanceRecord> result;
    result.reserve(records.size());

    for (const auto &record : records) {
        result.push_back(ProvenanceRecord{require_id(record, "assertion_id"), require_id(record, "source"),
                                          require_timestamp(record, "recorded_at"), require_string(record, "method")});
    }

    return result;
}

// The id list of a batch read. Like the write batches' list parsers, a malformed element anywhere in
// the list throws before kernel.execute is reached, so handle_tool_call reports it as is_error rather
// than answering a partial list.
std::vector<uint64_t> require_ids(const nlohmann::json &args, const char *key) {
    const auto &ids = args.at(key);
    if (!ids.is_array()) {
        throw std::runtime_error(std::string(key) + " must be an array");
    }

    std::vector<uint64_t> result;
    result.reserve(ids.size());

    for (const auto &id : ids) {
        result.push_back(id.get<uint64_t>());
    }

    return result;
}

nlohmann::ordered_json enum_property(const std::string &description, std::vector<std::string> values) {
    return nlohmann::ordered_json{{"type", "string"}, {"description", description}, {"enum", std::move(values)}};
}

std::optional<uint64_t> optional_id(const nlohmann::json &args, const char *key) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return std::nullopt;
    }
    return args.at(key).get<uint64_t>();
}

std::optional<Timestamp> optional_timestamp(const nlohmann::json &args, const char *key) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return std::nullopt;
    }
    return args.at(key).get<Timestamp>();
}

// An omitted or null "statuses" means every status, which is Query's own default -- not "Active only".
std::vector<AssertionStatus> optional_statuses(const nlohmann::json &args, const char *key) {
    std::vector<AssertionStatus> statuses;

    if (!args.contains(key) || args.at(key).is_null()) {
        return statuses;
    }

    const auto &values = args.at(key);
    if (!values.is_array()) {
        throw std::runtime_error("statuses must be an array");
    }

    for (const auto &value : values) {
        statuses.push_back(status_from_json(value)); // throws on an unknown name rather than ignoring it
    }

    return statuses;
}

QueryOrder optional_order(const nlohmann::json &args, const char *key) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return QueryOrder::AssertionId;
    }

    auto name = args.at(key).get<std::string>();
    if (name == "assertion_id") {
        return QueryOrder::AssertionId;
    }
    if (name == "valid_from") {
        return QueryOrder::ValidFrom;
    }
    if (name == "observed_at") {
        return QueryOrder::ObservedAt;
    }

    throw std::runtime_error("unknown order: " + name);
}

FilterField filter_field_from_name(const std::string &name) {
    if (name == "subject") {
        return FilterField::Subject;
    }
    if (name == "predicate") {
        return FilterField::Predicate;
    }
    if (name == "object") {
        return FilterField::Object;
    }
    if (name == "object_value") {
        return FilterField::ObjectValue;
    }
    if (name == "confidence") {
        return FilterField::Confidence;
    }
    if (name == "valid_from") {
        return FilterField::ValidFrom;
    }
    if (name == "valid_to") {
        return FilterField::ValidTo;
    }
    if (name == "observed_at") {
        return FilterField::ObservedAt;
    }
    if (name == "status") {
        return FilterField::Status;
    }

    throw std::runtime_error("unknown filter field: " + name);
}

CompareOp compare_op_from_name(const std::string &name) {
    if (name == "eq") {
        return CompareOp::Eq;
    }
    if (name == "ne") {
        return CompareOp::Ne;
    }
    if (name == "lt") {
        return CompareOp::Lt;
    }
    if (name == "lte") {
        return CompareOp::Lte;
    }
    if (name == "gt") {
        return CompareOp::Gt;
    }
    if (name == "gte") {
        return CompareOp::Gte;
    }

    throw std::runtime_error("unknown filter op: " + name);
}

// Recursive, and depth-bounded here as well as in the engine: the engine's check protects evaluation,
// this one protects the parser itself from a maliciously deep argument.
Filter parse_filter(const nlohmann::json &node, size_t depth) {
    if (depth > MAX_FILTER_DEPTH) {
        throw std::runtime_error("filter nested deeper than MAX_FILTER_DEPTH");
    }

    if (!node.is_object()) {
        throw std::runtime_error("filter must be an object");
    }

    std::string kind = node.at("kind").get<std::string>();

    if (kind == "comparison") {
        return Filter::compare(filter_field_from_name(node.at("field").get<std::string>()),
                               compare_op_from_name(node.at("op").get<std::string>()),
                               value_from_json(node.at("value")));
    }

    if (kind == "and" || kind == "or" || kind == "not") {
        const auto &children_json = node.at("children");
        if (!children_json.is_array()) {
            throw std::runtime_error("filter children must be an array");
        }

        std::vector<Filter> children;
        children.reserve(children_json.size());
        for (const auto &child : children_json) {
            children.push_back(parse_filter(child, depth + 1));
        }

        if (kind == "and") {
            return Filter::all_of(std::move(children));
        }
        if (kind == "or") {
            return Filter::any_of(std::move(children));
        }

        if (children.size() != 1) {
            throw std::runtime_error("not filter needs exactly one child");
        }
        return Filter::negate(std::move(children.front()));
    }

    throw std::runtime_error("unknown filter kind: " + kind);
}

std::optional<Filter> optional_filter(const nlohmann::json &args, const char *key) {
    if (!args.contains(key) || args.at(key).is_null()) {
        return std::nullopt;
    }

    return parse_filter(args.at(key), 1);
}

AggregateFunction aggregate_function_from_name(const std::string &name) {
    if (name == "count") {
        return AggregateFunction::Count;
    }
    if (name == "count_distinct") {
        return AggregateFunction::CountDistinct;
    }
    if (name == "sum") {
        return AggregateFunction::Sum;
    }
    if (name == "min") {
        return AggregateFunction::Min;
    }
    if (name == "max") {
        return AggregateFunction::Max;
    }
    if (name == "avg") {
        return AggregateFunction::Avg;
    }

    throw std::runtime_error("unknown aggregate function: " + name);
}

AggregateTarget aggregate_target_from_name(const std::string &name) {
    if (name == "object_value") {
        return AggregateTarget::ObjectValue;
    }
    if (name == "confidence") {
        return AggregateTarget::Confidence;
    }
    if (name == "valid_from") {
        return AggregateTarget::ValidFrom;
    }
    if (name == "valid_to") {
        return AggregateTarget::ValidTo;
    }
    if (name == "observed_at") {
        return AggregateTarget::ObservedAt;
    }
    if (name == "subject") {
        return AggregateTarget::Subject;
    }
    if (name == "predicate") {
        return AggregateTarget::Predicate;
    }
    if (name == "object") {
        return AggregateTarget::Object;
    }

    throw std::runtime_error("unknown aggregate target: " + name);
}

GroupField group_field_from_name(const std::string &name) {
    if (name == "subject") {
        return GroupField::Subject;
    }
    if (name == "predicate") {
        return GroupField::Predicate;
    }
    if (name == "object") {
        return GroupField::Object;
    }
    if (name == "status") {
        return GroupField::Status;
    }
    if (name == "valid_from_bucket") {
        return GroupField::ValidFromBucket;
    }
    if (name == "observed_at_bucket") {
        return GroupField::ObservedAtBucket;
    }

    throw std::runtime_error("unknown group-by field: " + name);
}

std::vector<Aggregation> require_aggregations(const nlohmann::json &args, const char *key) {
    const auto &values = args.at(key);
    if (!values.is_array()) {
        throw std::runtime_error("aggregations must be an array");
    }

    std::vector<Aggregation> aggregations;
    aggregations.reserve(values.size());

    for (const auto &entry : values) {
        Aggregation aggregation;
        aggregation.function = aggregate_function_from_name(entry.at("function").get<std::string>());

        // count ignores its target, so it is the one aggregation that may omit it.
        if (aggregation.function != AggregateFunction::Count) {
            aggregation.target = aggregate_target_from_name(entry.at("target").get<std::string>());
        } else if (entry.contains("target") && !entry.at("target").is_null()) {
            aggregation.target = aggregate_target_from_name(entry.at("target").get<std::string>());
        }

        aggregations.push_back(aggregation);
    }

    return aggregations;
}

std::vector<GroupBy> optional_group_by(const nlohmann::json &args, const char *key) {
    std::vector<GroupBy> group_by;

    if (!args.contains(key) || args.at(key).is_null()) {
        return group_by;
    }

    const auto &values = args.at(key);
    if (!values.is_array()) {
        throw std::runtime_error("group_by must be an array");
    }

    for (const auto &entry : values) {
        GroupBy group;
        group.field = group_field_from_name(entry.at("field").get<std::string>());
        if (entry.contains("bucket_width") && !entry.at("bucket_width").is_null()) {
            group.bucket_width = entry.at("bucket_width").get<Timestamp>();
        }
        group_by.push_back(group);
    }

    return group_by;
}

// Builds the IR from tool arguments. Every field is optional: an empty query means "everything, up to
// the result ceiling", which is well defined and bounded. force_scan is deliberately absent -- it is a
// differential-testing knob, not part of the callable surface.
Query require_query(const nlohmann::json &args) {
    Query query;

    if (args.contains("ir_version") && !args.at("ir_version").is_null()) {
        query.ir_version = args.at("ir_version").get<uint32_t>();
    }

    query.subject = optional_id(args, "subject");
    query.predicate = optional_id(args, "predicate");
    query.object = optional_id(args, "object");
    query.valid_at = optional_timestamp(args, "valid_at");
    query.observed_from = optional_timestamp(args, "observed_from");
    query.observed_to = optional_timestamp(args, "observed_to");
    query.open_ended_only = optional_bool(args, "open_ended_only", false);
    query.statuses = optional_statuses(args, "statuses");
    query.filter = optional_filter(args, "filter");
    query.resolve_names = optional_bool(args, "resolve_names", false);
    query.order = optional_order(args, "order");
    query.newest_first = optional_bool(args, "newest_first", false);
    query.limit = optional_size(args, "limit", 0);
    query.offset = optional_size(args, "offset", 0);
    query.max_rows_examined = optional_size(args, "max_rows_examined", 0);
    query.as_of_commit = optional_id(args, "as_of_commit");
    query.as_of_observed = optional_timestamp(args, "as_of_observed");

    if (args.contains("cursor") && !args.at("cursor").is_null()) {
        query.cursor = args.at("cursor").get<std::string>();
    }

    return query;
}

std::vector<std::byte> require_bytes(const nlohmann::json &args, const char *key) {
    return base64_decode(args.at(key).get<std::string>());
}

struct ToolDefinition {
    ToolSpec spec;
    std::function<KernelResult(KnowledgeKernel &, const nlohmann::json &, const ToolContext &)> invoke;
};

const std::vector<ToolDefinition> &tool_definitions() {
    static const std::vector<ToolDefinition> definitions = [] {
        std::vector<ToolDefinition> defs;

        defs.push_back(
            {{"commit", "Commits a new active assertion.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")},
                             {"object", integer_property("Object EntityId.")},
                             {"valid_from", integer_property("Valid-from timestamp.")},
                             {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                             {"observed_at", integer_property("Observed-at timestamp.")},
                             {"confidence", number_property("Confidence in [0,1].")}},
                            {"subject", "predicate", "object", "valid_from", "valid_to", "observed_at", "confidence"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CommitCommand{
                     require_id(args, "subject"), require_id(args, "predicate"), require_id(args, "object"),
                     require_timestamp(args, "valid_from"), require_timestamp(args, "valid_to"),
                     require_timestamp(args, "observed_at"), require_double(args, "confidence")});
             }});

        defs.push_back(
            {{"commit_by_name",
              "Commits a new active assertion from names/literals instead of ids, interning subject, "
              "predicate, and object as needed (idempotent).",
              object_schema(
                  {{"subject_name", string_property("Subject entity name.")},
                   {"predicate_name", string_property("Predicate name.")},
                   {"object", value_property("Object: a text value names an entity, any other kind "
                                             "is a literal.")},
                   {"valid_from", integer_property("Valid-from timestamp.")},
                   {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                   {"observed_at", integer_property("Observed-at timestamp.")},
                   {"confidence", number_property("Confidence in [0,1].")}},
                  {"subject_name", "predicate_name", "object", "valid_from", "valid_to", "observed_at", "confidence"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(
                     CommitByNameCommand{require_string(args, "subject_name"), require_string(args, "predicate_name"),
                                         require_value(args, "object"), require_timestamp(args, "valid_from"),
                                         require_timestamp(args, "valid_to"), require_timestamp(args, "observed_at"),
                                         require_double(args, "confidence")});
             }});

        defs.push_back(
            {{"commit_batch",
              "Commits many new active assertions in one call, under a single durability boundary "
              "(one fsync per log for the whole batch, not per assertion). At most " +
                  std::to_string(KnowledgeKernel::MAX_BATCH_SIZE) +
                  " entries; a larger list is rejected without writing anything. Each entry carries its "
                  "own valid_from/valid_to/observed_at. Returns the new AssertionIds in input order. "
                  "Not atomic: a crash mid-batch leaves a prefix of the entries committed, never a gap, "
                  "so a caller resumes at the first uncommitted entry. Plain appends only -- use "
                  "commit_superseding or commit_retraction for corrections.",
              object_schema(
                  {{"entries",
                    array_property(
                        "Assertions to commit, in order.", KnowledgeKernel::MAX_BATCH_SIZE,
                        object_schema({{"subject", integer_property("Subject EntityId.")},
                                       {"predicate", integer_property("Predicate PredicateId.")},
                                       {"object", integer_property("Object EntityId.")},
                                       {"valid_from", integer_property("Valid-from timestamp.")},
                                       {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                                       {"observed_at", integer_property("Observed-at timestamp.")},
                                       {"confidence", number_property("Confidence in [0,1].")}},
                                      {"subject", "predicate", "object", "valid_from", "valid_to", "observed_at",
                                       "confidence"}))}},
                  {"entries"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CommitBatchCommand{require_pending_assertions(args, "entries")});
             }});

        defs.push_back(
            {{"commit_batch_by_name",
              "Commits many new active assertions in one call from names/literals instead of ids, "
              "interning each entry's subject, predicate, and object as needed (idempotent). Same "
              "batch semantics as commit_batch -- at most " +
                  std::to_string(KnowledgeKernel::MAX_BATCH_SIZE) +
                  " entries, each with its own valid_from/valid_to/observed_at, ids returned in input "
                  "order, and a crash mid-batch leaves a prefix committed. Interning happens before "
                  "the batch, so a genuinely new name costs its own durable write; names already known "
                  "cost nothing.",
              object_schema(
                  {{"entries",
                    array_property(
                        "Assertions to commit, in order.", KnowledgeKernel::MAX_BATCH_SIZE,
                        object_schema({{"subject_name", string_property("Subject entity name.")},
                                       {"predicate_name", string_property("Predicate name.")},
                                       {"object", value_property("Object: a text value names an entity, any "
                                                                 "other kind is a literal.")},
                                       {"valid_from", integer_property("Valid-from timestamp.")},
                                       {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                                       {"observed_at", integer_property("Observed-at timestamp.")},
                                       {"confidence", number_property("Confidence in [0,1].")}},
                                      {"subject_name", "predicate_name", "object", "valid_from", "valid_to",
                                       "observed_at", "confidence"}))}},
                  {"entries"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CommitBatchByNameCommand{require_pending_named_assertions(args, "entries")});
             }});

        defs.push_back(
            {{"commit_retraction", "Commits a retraction record for an existing assertion.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")},
                             {"object", integer_property("Object EntityId.")},
                             {"valid_from", integer_property("Valid-from timestamp.")},
                             {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                             {"observed_at", integer_property("Observed-at timestamp.")},
                             {"confidence", number_property("Confidence in [0,1].")},
                             {"retracts_id", integer_property("AssertionId being retracted.")}},
                            {"subject", "predicate", "object", "valid_from", "valid_to", "observed_at", "confidence",
                             "retracts_id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CommitRetractionCommand{
                     require_id(args, "subject"), require_id(args, "predicate"), require_id(args, "object"),
                     require_timestamp(args, "valid_from"), require_timestamp(args, "valid_to"),
                     require_timestamp(args, "observed_at"), require_double(args, "confidence"),
                     require_id(args, "retracts_id")});
             }});

        defs.push_back(
            {{"commit_superseding", "Commits a replacement assertion, marking the superseded one as such.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")},
                             {"object", integer_property("Object EntityId.")},
                             {"valid_from", integer_property("Valid-from timestamp.")},
                             {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                             {"observed_at", integer_property("Observed-at timestamp.")},
                             {"confidence", number_property("Confidence in [0,1].")},
                             {"supersedes_id", integer_property("AssertionId being superseded.")}},
                            {"subject", "predicate", "object", "valid_from", "valid_to", "observed_at", "confidence",
                             "supersedes_id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CommitSupersedingCommand{
                     require_id(args, "subject"), require_id(args, "predicate"), require_id(args, "object"),
                     require_timestamp(args, "valid_from"), require_timestamp(args, "valid_to"),
                     require_timestamp(args, "observed_at"), require_double(args, "confidence"),
                     require_id(args, "supersedes_id")});
             }});

        defs.push_back(
            {{"write_snapshot", "Persists a full snapshot of current in-memory assertions.", object_schema({}, {})},
             [](KnowledgeKernel &kernel, const nlohmann::json &, const ToolContext &context) -> KernelResult {
                 return kernel.execute(WriteSnapshotCommand{});
             }});

        defs.push_back(
            {{"intern_entity", "Interns a named entity, returning its EntityId (idempotent).",
              object_schema({{"name", string_property("Entity name.")}}, {"name"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(InternEntityCommand{require_string(args, "name")});
             }});

        defs.push_back(
            {{"intern_value", "Interns a typed literal value, returning its EntityId (idempotent).",
              object_schema({{"value", value_property("The literal to intern.")}}, {"value"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(InternValueCommand{require_value(args, "value")});
             }});

        defs.push_back(
            {{"intern_predicate", "Interns a named predicate, returning its PredicateId (idempotent).",
              object_schema({{"name", string_property("Predicate name.")}}, {"name"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(InternPredicateCommand{require_string(args, "name")});
             }});

        defs.push_back(
            {{"intern_document", "Interns raw document bytes, returning an EntityId.",
              object_schema({{"content", base64_string_property("Document content.")}}, {"content"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(InternDocumentCommand{require_bytes(args, "content")});
             }});

        defs.push_back(
            {{"record_provenance", "Records which source produced a given assertion, and by what method.",
              object_schema({{"assertion_id", integer_property("Target AssertionId.")},
                             {"source", integer_property("Source EntityId.")},
                             {"recorded_at", integer_property("Timestamp the provenance was recorded.")},
                             {"method", string_property("Free-text method description.")}},
                            {"assertion_id", "source", "recorded_at", "method"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(
                     RecordProvenanceCommand{require_id(args, "assertion_id"), require_id(args, "source"),
                                             require_timestamp(args, "recorded_at"), require_string(args, "method")});
             }});

        defs.push_back(
            {{"record_provenance_batch",
              "Records provenance for many assertions in one call, under a single durability boundary "
              "(one fsync for the whole list, not per record) -- the companion to commit_batch, whose "
              "returned ids come back in input order for exactly this. At most " +
                  std::to_string(KnowledgeKernel::MAX_BATCH_SIZE) +
                  " records. Every target assertion is validated first, so one unknown id rejects the "
                  "whole call without writing anything.",
              object_schema(
                  {{"records",
                    array_property(
                        "Provenance records to write, in order.", KnowledgeKernel::MAX_BATCH_SIZE,
                        object_schema({{"assertion_id", integer_property("Target AssertionId.")},
                                       {"source", integer_property("Source EntityId.")},
                                       {"recorded_at", integer_property("Timestamp the provenance was recorded.")},
                                       {"method", string_property("Free-text method description.")}},
                                      {"assertion_id", "source", "recorded_at", "method"}))}},
                  {"records"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(RecordProvenanceBatchCommand{require_provenance_records(args, "records")});
             }});

        defs.push_back(
            {{"commit_hypothesis", "Commits a labeled, machine-suggested (Hypothesis-status) assertion.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")},
                             {"object", integer_property("Object EntityId.")},
                             {"valid_from", integer_property("Valid-from timestamp.")},
                             {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                             {"observed_at", integer_property("Observed-at timestamp.")},
                             {"confidence", number_property("Confidence in [0,1].")},
                             {"source", integer_property("Source EntityId (required for hypotheses).")},
                             {"recorded_at", integer_property("Timestamp the provenance was recorded.")},
                             {"method", string_property("Free-text method description.")}},
                            {"subject", "predicate", "object", "valid_from", "valid_to", "observed_at", "confidence",
                             "source", "recorded_at", "method"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CommitHypothesisCommand{
                     require_id(args, "subject"), require_id(args, "predicate"), require_id(args, "object"),
                     require_timestamp(args, "valid_from"), require_timestamp(args, "valid_to"),
                     require_timestamp(args, "observed_at"), require_double(args, "confidence"),
                     require_id(args, "source"), require_timestamp(args, "recorded_at"),
                     require_string(args, "method")});
             }});

        defs.push_back(
            {{"merge_entities", "Merges absorb into keep: a one-way, append-only redirect.",
              object_schema({{"keep", integer_property("Surviving EntityId.")},
                             {"absorb", integer_property("Absorbed EntityId.")},
                             {"merged_at", integer_property("Timestamp of the merge decision.")}},
                            {"keep", "absorb", "merged_at"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(MergeEntitiesCommand{require_id(args, "keep"), require_id(args, "absorb"),
                                                            require_timestamp(args, "merged_at")});
             }});

        defs.push_back(
            {{"archive_segments_before",
              "Archives (compacts, does not delete) log segments entirely before an "
              "AssertionId.",
              object_schema({{"assertion_id", integer_property("Archive-before boundary AssertionId.")}},
                            {"assertion_id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 kernel.execute(ArchiveSegmentsBeforeCommand{require_id(args, "assertion_id")});
                 return std::monostate{};
             }});

        defs.push_back(
            {{"get", "Fetches a single assertion by id.",
              object_schema({{"id", integer_property("AssertionId.")}}, {"id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(GetCommand{require_id(args, "id")});
             }});

        defs.push_back(
            {{"assertions_for_subject", "Returns every recorded assertion (any status) for a subject.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"limit", integer_property("Maximum number of results (omit or 0 for no limit).")}},
                            {"subject"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(
                     AssertionsForSubjectCommand{require_id(args, "subject"), optional_size(args, "limit", 0)});
             }});

        defs.push_back(
            {{"current", "Returns every currently active, open-ended assertion for a subject.",
              object_schema({{"subject", integer_property("Subject EntityId.")}}, {"subject"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CurrentCommand{require_id(args, "subject")});
             }});

        defs.push_back(
            {{"current_by_name",
              "Returns every currently active, open-ended assertion for a subject looked up by name; "
              "empty (not an error) if the name was never interned.",
              object_schema({{"subject_name", string_property("Subject entity name.")}}, {"subject_name"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CurrentByNameCommand{require_string(args, "subject_name")});
             }});

        defs.push_back(
            {{"current_by_object", "Reverse-direction lookup: who currently has the given entity as object.",
              object_schema({{"object", integer_property("Object EntityId.")}}, {"object"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CurrentByObjectCommand{require_id(args, "object")});
             }});

        defs.push_back(
            {{"current_by_predicate",
              "Kernel-wide lookup: every currently active assertion for a predicate, any "
              "subject.",
              object_schema({{"predicate", integer_property("Predicate PredicateId.")}}, {"predicate"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CurrentByPredicateCommand{require_id(args, "predicate")});
             }});

        defs.push_back(
            {{"valid_at", "Returns assertions valid at a given point in valid time.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"valid_time", integer_property("Valid-time cutoff.")}},
                            {"subject", "valid_time"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(
                     ValidAtCommand{require_id(args, "subject"), require_timestamp(args, "valid_time")});
             }});

        defs.push_back(
            {{"known_at",
              "Returns assertions observed by a given point in observed time and still currently "
              "Active.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"observed_time", integer_property("Observed-time cutoff.")}},
                            {"subject", "observed_time"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(
                     KnownAtCommand{require_id(args, "subject"), require_timestamp(args, "observed_time")});
             }});

        defs.push_back(
            {{"valid_at_known_at", "Combines valid_at and known_at cutoffs.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"valid_time", integer_property("Valid-time cutoff.")},
                             {"observed_time", integer_property("Observed-time cutoff.")}},
                            {"subject", "valid_time", "observed_time"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(ValidAtKnownAtCommand{require_id(args, "subject"),
                                                             require_timestamp(args, "valid_time"),
                                                             require_timestamp(args, "observed_time")});
             }});

        defs.push_back(
            {{"valid_time_timeline", "Returns active assertions for a subject/predicate sorted by valid_from.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")}},
                            {"subject", "predicate"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(
                     ValidTimeTimelineCommand{require_id(args, "subject"), require_id(args, "predicate")});
             }});

        defs.push_back(
            {{"observed_time_timeline", "Returns active assertions for a subject/predicate sorted by observed_at.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")}},
                            {"subject", "predicate"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(
                     ObservedTimeTimelineCommand{require_id(args, "subject"), require_id(args, "predicate")});
             }});

        defs.push_back(
            {{"commit_history",
              "Returns every recorded assertion (any status) for a subject/predicate, in commit "
              "order.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")},
                             {"limit", integer_property("Maximum number of results (omit or 0 for no limit).")}},
                            {"subject", "predicate"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CommitHistoryCommand{require_id(args, "subject"), require_id(args, "predicate"),
                                                            optional_size(args, "limit", 0)});
             }});

        defs.push_back(
            {{"changes_since",
              "Kernel-wide, status-agnostic: every assertion observed at or after a cutoff, any "
              "subject or predicate.",
              object_schema(
                  {{"observed_since", integer_property("Observed-at cutoff.")},
                   {"limit", integer_property("Maximum number of results (omit or 0 for no limit).")},
                   {"newest_first", boolean_property("If true, most-recently-observed first instead of oldest first "
                                                     "(default false); combine with limit to fetch just the latest "
                                                     "change(s) without reading the whole log.")}},
                  {"observed_since"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(ChangesSinceCommand{require_timestamp(args, "observed_since"),
                                                           optional_size(args, "limit", 0),
                                                           optional_bool(args, "newest_first", false)});
             }});

        defs.push_back(
            {{"explain", "Walks the supersession/retraction chain from an assertion back to its root.",
              object_schema({{"id", integer_property("AssertionId to explain.")}}, {"id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(ExplainCommand{require_id(args, "id")});
             }});

        defs.push_back(
            {{"find_conflicts",
              "Finds overlapping active assertions for a subject/predicate with different "
              "objects.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")}},
                            {"subject", "predicate"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(
                     FindConflictsCommand{require_id(args, "subject"), require_id(args, "predicate")});
             }});

        defs.push_back(
            {{"find_entity", "Looks up a previously interned entity's id by name.",
              object_schema({{"name", string_property("Entity name.")}}, {"name"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(FindEntityCommand{require_string(args, "name")});
             }});

        defs.push_back(
            {{"find_value", "Looks up a previously interned literal value's id.",
              object_schema({{"value", value_property("The literal to look up.")}}, {"value"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(FindValueCommand{require_value(args, "value")});
             }});

        defs.push_back(
            {{"find_predicate", "Looks up a previously interned predicate's id by name.",
              object_schema({{"name", string_property("Predicate name.")}}, {"name"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(FindPredicateCommand{require_string(args, "name")});
             }});

        defs.push_back(
            {{"entity_name", "Resolves a previously interned entity's name.",
              object_schema({{"id", integer_property("EntityId.")}}, {"id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(EntityNameCommand{require_id(args, "id")});
             }});

        defs.push_back(
            {{"entity_value", "Resolves a previously interned entity's literal value.",
              object_schema({{"id", integer_property("EntityId.")}}, {"id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(EntityValueCommand{require_id(args, "id")});
             }});

        defs.push_back(
            {{"predicate_name", "Resolves a previously interned predicate's name.",
              object_schema({{"id", integer_property("PredicateId.")}}, {"id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(PredicateNameCommand{require_id(args, "id")});
             }});

        defs.push_back(
            {{"document_content", "Fetches previously interned document bytes.",
              object_schema({{"id", integer_property("Document EntityId.")}}, {"id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(DocumentContentCommand{require_id(args, "id")});
             }});

        defs.push_back(
            {{"provenance_for", "Resolves recorded provenance for an assertion.",
              object_schema({{"assertion_id", integer_property("AssertionId.")}}, {"assertion_id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(ProvenanceForCommand{require_id(args, "assertion_id")});
             }});

        // Batch reads (#55): one tool per single-id resolver above, same answer per slot. The shared
        // wording keeps the four descriptions from drifting apart on the contract a caller zips against.
        const std::string batch_read_contract =
            " Answers in input order, one slot per id, each slot exactly what the single call answers for "
            "that id -- null included, e.g. for an id that was never interned. At most " +
            std::to_string(KnowledgeKernel::MAX_BATCH_SIZE) +
            " ids; a larger list is rejected before anything is read.";

        defs.push_back(
            {{"entity_name_batch",
              "Resolves many previously interned entities' names in one call." + batch_read_contract,
              object_schema({{"ids", array_property("EntityIds to resolve, in order.", KnowledgeKernel::MAX_BATCH_SIZE,
                                                    integer_property("EntityId."))}},
                            {"ids"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(EntityNameBatchCommand{require_ids(args, "ids")});
             }});

        defs.push_back(
            {{"entity_value_batch",
              "Resolves many previously interned entities' literal values in one call." + batch_read_contract,
              object_schema({{"ids", array_property("EntityIds to resolve, in order.", KnowledgeKernel::MAX_BATCH_SIZE,
                                                    integer_property("EntityId."))}},
                            {"ids"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(EntityValueBatchCommand{require_ids(args, "ids")});
             }});

        defs.push_back(
            {{"predicate_name_batch",
              "Resolves many previously interned predicates' names in one call." + batch_read_contract,
              object_schema(
                  {{"ids", array_property("PredicateIds to resolve, in order.", KnowledgeKernel::MAX_BATCH_SIZE,
                                          integer_property("PredicateId."))}},
                  {"ids"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(PredicateNameBatchCommand{require_ids(args, "ids")});
             }});

        defs.push_back(
            {{"provenance_for_batch",
              "Resolves recorded provenance for many assertions in one call." + batch_read_contract,
              object_schema({{"assertion_ids",
                              array_property("AssertionIds to resolve, in order.", KnowledgeKernel::MAX_BATCH_SIZE,
                                             integer_property("AssertionId."))}},
                            {"assertion_ids"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(ProvenanceForBatchCommand{require_ids(args, "assertion_ids")});
             }});

        defs.push_back(
            {{"query",
              "Runs a shaped read against the query IR (Phase 10): any combination of subject, "
              "predicate, object, a valid-time point, an observed-time window, open-endedness and an "
              "explicit status set, with deterministic ordering and paging. Every single-purpose read "
              "above is expressible here and returns identical rows. All arguments are optional; an "
              "empty query means every assertion, up to the ceiling of " +
                  std::to_string(MAX_QUERY_RESULT) +
                  " rows that limit is capped to. Returns {assertions, truncated}, where truncated "
                  "says more rows matched than were returned, plus next_cursor when there is a further "
                  "page: pass it back as cursor to continue, and stop when it is absent. Prefer that to "
                  "offset for walking a large result -- it costs one pass per page instead of re-skipping, "
                  "and it cannot repeat or drop a row if someone commits while you page. The IR is "
                  "versioned (ir_version, currently " +
                  std::to_string(QUERY_IR_VERSION) +
                  ") and still evolving through the query-engine phases: an unknown version is "
                  "rejected rather than reinterpreted.",
              object_schema(
                  {{"subject", integer_property("Subject EntityId; omitted means any. Resolved through merge "
                                                "redirects.")},
                   {"predicate", integer_property("Predicate PredicateId; omitted means any.")},
                   {"object", integer_property("Object EntityId; omitted means any. Resolved through merge "
                                               "redirects.")},
                   {"valid_at", integer_property("Valid-time point: valid_from <= t < valid_to, with valid_to 0 "
                                                 "meaning open-ended.")},
                   {"observed_from", integer_property("Lower inclusive bound on observed_at.")},
                   {"observed_to", integer_property("Upper inclusive bound on observed_at.")},
                   {"open_ended_only", boolean_property("Only assertions whose valid_to is 0 (open-ended).")},
                   {"statuses", array_property("Statuses to include; omitted means every status.", 5,
                                               enum_property("Assertion status.", {"Active", "Superseded", "Retracted",
                                                                                   "Retraction", "Hypothesis"}))},
                   {"filter",
                    object_property(
                        "Filter tree, nesting up to " + std::to_string(MAX_FILTER_DEPTH) +
                        " deep. Either a comparison -- {kind: \"comparison\", field, op, value} with field one of "
                        "subject|predicate|object|object_value|confidence|valid_from|valid_to|observed_at|status, "
                        "op one of eq|ne|lt|lte|gt|gte, and value a tagged {kind, value} -- or a combination: "
                        "{kind: \"and\"|\"or\"|\"not\", children: [<filter>, ...]}, where each child has this "
                        "same shape and \"not\" takes exactly one. Operand kinds must match the field (int64 for "
                        "ids, double for confidence, timestamp for the time fields, text naming a status for "
                        "status); object_value takes any kind and simply does not match rows whose object value is "
                        "of another kind.")},
                   {"resolve_names",
                    boolean_property("Also return catalog names/values for each returned row, as a parallel "
                                     "\"names\" array -- saves a second round trip through the batch resolvers.")},
                   {"order", enum_property("Ordering key; ties always break on AssertionId.",
                                           {"assertion_id", "valid_from", "observed_at"})},
                   {"newest_first", boolean_property("Reverse the ordering, tie-break included.")},
                   {"limit", integer_property("Maximum rows; 0 or omitted means the ceiling, and a larger value "
                                              "is capped to it.")},
                   {"offset", integer_property("Rows to skip after ordering.")},
                   {"ir_version", integer_property("Query IR version; omitted means the current one.")},
                   {"cursor", string_property("Opaque token from a previous response's next_cursor: resume "
                                              "strictly after the row it names. Must not be combined with "
                                              "offset, and must have been produced under this query's order "
                                              "and newest_first.")},
                   {"max_rows_examined",
                    integer_property("Give up with an error rather than examine more than this many rows; 0 or "
                                     "omitted means no budget. Rows examined rather than elapsed time, so the "
                                     "same query on the same data always gets the same answer.")},
                   {"as_of_commit",
                    integer_property("Answer as of the log after this many records: rows committed later are "
                                     "invisible, and every row's status is what it was then. Not combinable with "
                                     "as_of_observed.")},
                   {"as_of_observed",
                    integer_property("The same question in observed time: rows observed after this are invisible, "
                                     "and a correction counts only if it had been observed by then. This is what "
                                     "known_at should have been -- known_at applies the cutoff but reports status "
                                     "as it stands now.")}},
                  {})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(QueryCommand{require_query(args)});
             }});

        defs.push_back(
            {{"query_spill",
              "Writes a query's rows to disk as columns and returns a descriptor instead of the rows: the "
              "way to take a result too large to travel as JSON-RPC text. Takes the same arguments as "
              "query, plus an optional token naming the result. limit 0 means everything up to " +
                  std::to_string(MAX_SPILL_ROWS) +
                  " rows rather than the JSON ceiling. The response gives the directory, the row count, "
                  "and one fixed-width native-endian file per column, plus a dictionary mapping the ids "
                  "present to their catalog names and values -- a reader is about twenty lines (see "
                  "tools/read_spill.py). Requires the server to have been started with --spill-dir; the "
                  "directory is the server's, and deleting finished results is the operator's business.",
              object_schema(
                  {{"subject", integer_property("Subject EntityId; omitted means any.")},
                   {"predicate", integer_property("Predicate PredicateId; omitted means any.")},
                   {"object", integer_property("Object EntityId; omitted means any.")},
                   {"valid_at", integer_property("Valid-time point, as in query.")},
                   {"observed_from", integer_property("Lower inclusive bound on observed_at.")},
                   {"observed_to", integer_property("Upper inclusive bound on observed_at.")},
                   {"open_ended_only", boolean_property("Only assertions whose valid_to is 0.")},
                   {"statuses", array_property("Statuses to include; omitted means every status.", 5,
                                               enum_property("Assertion status.", {"Active", "Superseded", "Retracted",
                                                                                   "Retraction", "Hypothesis"}))},
                   {"filter", object_property("Filter tree, exactly as in query.")},
                   {"order", enum_property("Ordering key; ties always break on AssertionId.",
                                           {"assertion_id", "valid_from", "observed_at"})},
                   {"newest_first", boolean_property("Reverse the ordering.")},
                   {"limit", integer_property("Maximum rows; 0 or omitted means the spill ceiling.")},
                   {"offset", integer_property("Rows to skip after ordering.")},
                   {"token", string_property("Name for the result directory; generated if omitted. "
                                             "Refused if it already exists.")},
                   {"ir_version", integer_property("Query IR version; omitted means the current one.")},
                   {"cursor", string_property("Resume after a previous page's next_cursor, as in query.")},
                   {"max_rows_examined", integer_property("Row budget, as in query; 0 or omitted means none.")},
                   {"as_of_commit",
                    integer_property("Answer as of the log after this many records: rows committed later are "
                                     "invisible, and every row's status is what it was then. Not combinable with "
                                     "as_of_observed.")},
                   {"as_of_observed",
                    integer_property("The same question in observed time: rows observed after this are invisible, "
                                     "and a correction counts only if it had been observed by then. This is what "
                                     "known_at should have been -- known_at applies the cutoff but reports status "
                                     "as it stands now.")}},
                  {})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 if (context.spill_directory.empty()) {
                     throw std::runtime_error("spilling is not configured; start mcp_server with --spill-dir DIR");
                 }

                 std::string token;
                 if (args.contains("token") && !args.at("token").is_null()) {
                     token = args.at("token").get<std::string>();
                 }

                 return kernel.execute(SpillQueryCommand{require_query(args), context.spill_directory, token});
             }});

        defs.push_back(
            {{"explain_query",
              "Returns the plan `query` would follow for these arguments, without running it: which "
              "source the rows come from (an index, the columnar scan, or the row scan), how many rows "
              "that source yields, the modelled cost, and every alternative the planner weighed with the "
              "reason it was rejected. Takes exactly the same arguments as query. Row counts for index "
              "sources are exact rather than sampled; costs are in nanoseconds from measured constants "
              "and are comparable only within one plan.",
              object_schema(
                  {{"subject", integer_property("Subject EntityId; omitted means any.")},
                   {"predicate", integer_property("Predicate PredicateId; omitted means any.")},
                   {"object", integer_property("Object EntityId; omitted means any.")},
                   {"valid_at", integer_property("Valid-time point, as in query.")},
                   {"observed_from", integer_property("Lower inclusive bound on observed_at.")},
                   {"observed_to", integer_property("Upper inclusive bound on observed_at.")},
                   {"open_ended_only", boolean_property("Only assertions whose valid_to is 0.")},
                   {"statuses", array_property("Statuses to include; omitted means every status.", 5,
                                               enum_property("Assertion status.", {"Active", "Superseded", "Retracted",
                                                                                   "Retraction", "Hypothesis"}))},
                   {"filter", object_property("Filter tree, exactly as in query.")},
                   {"limit", integer_property("Maximum rows; affects the plan only via paging.")},
                   {"offset", integer_property("Rows to skip after ordering.")},
                   {"ir_version", integer_property("Query IR version; omitted means the current one.")},
                   {"as_of_commit",
                    integer_property("Answer as of the log after this many records: rows committed later are "
                                     "invisible, and every row's status is what it was then. Not combinable with "
                                     "as_of_observed.")},
                   {"as_of_observed",
                    integer_property("The same question in observed time: rows observed after this are invisible, "
                                     "and a correction counts only if it had been observed by then. This is what "
                                     "known_at should have been -- known_at applies the cutoff but reports status "
                                     "as it stands now.")}},
                  {})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(ExplainQueryCommand{require_query(args)});
             }});

        defs.push_back(
            {{"aggregate",
              "Aggregates matching assertions instead of returning them: count, count_distinct, sum, "
              "min, max and avg, optionally grouped by subject, predicate, object, status, or a "
              "fixed-width valid-time or observed-time bucket. Selection uses the same arguments as "
              "query (subject/predicate/object, valid_at, the observed window, open_ended_only, "
              "statuses, filter), so an aggregate and a query can never disagree about which rows are "
              "current. Returns {groups: [{key, row_count, values}]}, with values parallel to "
              "aggregations: counts are integers, sum/min/max/avg are numbers or null when no row in "
              "the group had one. Rows whose target is absent or non-numeric are skipped rather than "
              "counted as zero, which is why row_count can exceed the count an average was taken over. "
              "At most " +
                  std::to_string(MAX_GROUP_COUNT) + " groups: exceeding it is an error, never a truncated answer.",
              object_schema(
                  {{"aggregations",
                    array_property("What to compute; at least one.", 16,
                                   object_property("{function: count|count_distinct|sum|min|max|avg, target: "
                                                   "object_value|confidence|valid_from|valid_to|observed_at|"
                                                   "subject|predicate|object}. target is ignored by count."))},
                   {"group_by",
                    array_property("How to group; omitted means one group over everything.", MAX_GROUP_BY_FIELDS,
                                   object_property("{field: subject|predicate|object|status|valid_from_bucket|"
                                                   "observed_at_bucket, bucket_width: <positive integer, required "
                                                   "for the bucket fields>}."))},
                   {"subject", integer_property("Subject EntityId; omitted means any.")},
                   {"predicate", integer_property("Predicate PredicateId; omitted means any.")},
                   {"object", integer_property("Object EntityId; omitted means any.")},
                   {"valid_at", integer_property("Valid-time point, as in query.")},
                   {"observed_from", integer_property("Lower inclusive bound on observed_at.")},
                   {"observed_to", integer_property("Upper inclusive bound on observed_at.")},
                   {"open_ended_only", boolean_property("Only assertions whose valid_to is 0 (open-ended).")},
                   {"statuses", array_property("Statuses to include; omitted means every status.", 5,
                                               enum_property("Assertion status.", {"Active", "Superseded", "Retracted",
                                                                                   "Retraction", "Hypothesis"}))},
                   {"filter", object_property("Filter tree, exactly as in query.")},
                   {"max_groups", integer_property("Group cap; 0 or omitted means the ceiling, and a larger "
                                                   "value is capped to it.")},
                   {"ir_version", integer_property("Query IR version; omitted means the current one.")},
                   {"max_rows_examined", integer_property("Row budget, as in query; 0 or omitted means none.")},
                   {"as_of_commit",
                    integer_property("Answer as of the log after this many records: rows committed later are "
                                     "invisible, and every row's status is what it was then. Not combinable with "
                                     "as_of_observed.")},
                   {"as_of_observed",
                    integer_property("The same question in observed time: rows observed after this are invisible, "
                                     "and a correction counts only if it had been observed by then. This is what "
                                     "known_at should have been -- known_at applies the cutoff but reports status "
                                     "as it stands now.")}},
                  {"aggregations"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 AggregateQuery aggregate;
                 aggregate.selection = require_query(args);
                 aggregate.aggregations = require_aggregations(args, "aggregations");
                 aggregate.group_by = optional_group_by(args, "group_by");
                 aggregate.max_groups = optional_size(args, "max_groups", 0);
                 aggregate.ir_version = aggregate.selection.ir_version;
                 return kernel.execute(AggregateCommand{std::move(aggregate)});
             }});

        defs.push_back(
            {{"describe_predicates",
              "Lists every predicate this store has interned, with the id to use in a query and how many "
              "current (Active, open-ended) rows it has. Call this before guessing a predicate name: a "
              "query naming a predicate that was never interned returns no rows, which is "
              "indistinguishable from the fact being absent.",
              object_schema({}, {})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(DescribePredicatesCommand{});
             }});

        defs.push_back(
            {{"describe_corpus",
              "Describes the store's shape: assertion/entity/predicate counts, distinct subjects and "
              "current objects, a count per status, and the observed-time and valid-from spans. Enough to "
              "size a query -- whether to page or spill, and which time windows contain anything at all. "
              "A snapshot of the moment it was called, not a live view.",
              object_schema({}, {})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(DescribeCorpusCommand{});
             }});

        defs.push_back(
            {{"hypotheses_for", "Returns open (Hypothesis-status) predictions for a subject.",
              object_schema({{"subject", integer_property("Subject EntityId.")}}, {"subject"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(HypothesesForCommand{require_id(args, "subject")});
             }});

        defs.push_back(
            {{"neighbors", "Bounded breadth-first traversal of current-edge neighbors, both directions.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"max_hops", integer_property("Maximum hop count.")}},
                            {"subject", "max_hops"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(NeighborsCommand{require_id(args, "subject"), require_size(args, "max_hops")});
             }});

        defs.push_back(
            {{"co_occurring_predicates", "Currently active predicates for a subject.",
              object_schema({{"subject", integer_property("Subject EntityId.")}}, {"subject"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(CoOccurringPredicatesCommand{require_id(args, "subject")});
             }});

        defs.push_back(
            {{"resolve_entity", "Resolves an id through recorded merge redirects to its canonical id.",
              object_schema({{"id", integer_property("EntityId.")}}, {"id"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args, const ToolContext &context) -> KernelResult {
                 return kernel.execute(ResolveEntityCommand{require_id(args, "id")});
             }});

        return defs;
    }();

    return definitions;
}

} // namespace

const std::vector<ToolSpec> &tool_specs() {
    static const std::vector<ToolSpec> specs = [] {
        std::vector<ToolSpec> result;
        for (const auto &definition : tool_definitions()) {
            result.push_back(definition.spec);
        }
        return result;
    }();

    return specs;
}

ToolCallResult handle_tool_call(KnowledgeKernel &kernel, const std::string &tool_name, const nlohmann::json &arguments,
                                const ToolContext &context) {
    try {
        for (const auto &definition : tool_definitions()) {
            if (definition.spec.name == tool_name) {
                KernelResult result = definition.invoke(kernel, arguments, context);
                return ToolCallResult{kernel_result_to_json(result).dump(), false};
            }
        }

        return ToolCallResult{"unknown tool: " + tool_name, true};
    } catch (const std::exception &error) {
        return ToolCallResult{std::string("error: ") + error.what(), true};
    }
}

} // namespace knk::mcp
