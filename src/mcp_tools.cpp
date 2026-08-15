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

std::vector<std::byte> require_bytes(const nlohmann::json &args, const char *key) {
    return base64_decode(args.at(key).get<std::string>());
}

struct ToolDefinition {
    ToolSpec spec;
    std::function<KernelResult(KnowledgeKernel &, const nlohmann::json &)> invoke;
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
             [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
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
             [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                 return kernel.execute(
                     CommitByNameCommand{require_string(args, "subject_name"), require_string(args, "predicate_name"),
                                         require_value(args, "object"), require_timestamp(args, "valid_from"),
                                         require_timestamp(args, "valid_to"), require_timestamp(args, "observed_at"),
                                         require_double(args, "confidence")});
             }});

        defs.push_back({{"commit_retraction", "Commits a retraction record for an existing assertion.",
                         object_schema({{"subject", integer_property("Subject EntityId.")},
                                        {"predicate", integer_property("Predicate PredicateId.")},
                                        {"object", integer_property("Object EntityId.")},
                                        {"valid_from", integer_property("Valid-from timestamp.")},
                                        {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                                        {"observed_at", integer_property("Observed-at timestamp.")},
                                        {"confidence", number_property("Confidence in [0,1].")},
                                        {"retracts_id", integer_property("AssertionId being retracted.")}},
                                       {"subject", "predicate", "object", "valid_from", "valid_to", "observed_at",
                                        "confidence", "retracts_id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(CommitRetractionCommand{
                                require_id(args, "subject"), require_id(args, "predicate"), require_id(args, "object"),
                                require_timestamp(args, "valid_from"), require_timestamp(args, "valid_to"),
                                require_timestamp(args, "observed_at"), require_double(args, "confidence"),
                                require_id(args, "retracts_id")});
                        }});

        defs.push_back({{"commit_superseding", "Commits a replacement assertion, marking the superseded one as such.",
                         object_schema({{"subject", integer_property("Subject EntityId.")},
                                        {"predicate", integer_property("Predicate PredicateId.")},
                                        {"object", integer_property("Object EntityId.")},
                                        {"valid_from", integer_property("Valid-from timestamp.")},
                                        {"valid_to", integer_property("Valid-to timestamp; 0 means open-ended.")},
                                        {"observed_at", integer_property("Observed-at timestamp.")},
                                        {"confidence", number_property("Confidence in [0,1].")},
                                        {"supersedes_id", integer_property("AssertionId being superseded.")}},
                                       {"subject", "predicate", "object", "valid_from", "valid_to", "observed_at",
                                        "confidence", "supersedes_id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(CommitSupersedingCommand{
                                require_id(args, "subject"), require_id(args, "predicate"), require_id(args, "object"),
                                require_timestamp(args, "valid_from"), require_timestamp(args, "valid_to"),
                                require_timestamp(args, "observed_at"), require_double(args, "confidence"),
                                require_id(args, "supersedes_id")});
                        }});

        defs.push_back(
            {{"write_snapshot", "Persists a full snapshot of current in-memory assertions.", object_schema({}, {})},
             [](KnowledgeKernel &kernel, const nlohmann::json &) -> KernelResult {
                 return kernel.execute(WriteSnapshotCommand{});
             }});

        defs.push_back({{"intern_entity", "Interns a named entity, returning its EntityId (idempotent).",
                         object_schema({{"name", string_property("Entity name.")}}, {"name"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(InternEntityCommand{require_string(args, "name")});
                        }});

        defs.push_back({{"intern_value", "Interns a typed literal value, returning its EntityId (idempotent).",
                         object_schema({{"value", value_property("The literal to intern.")}}, {"value"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(InternValueCommand{require_value(args, "value")});
                        }});

        defs.push_back({{"intern_predicate", "Interns a named predicate, returning its PredicateId (idempotent).",
                         object_schema({{"name", string_property("Predicate name.")}}, {"name"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(InternPredicateCommand{require_string(args, "name")});
                        }});

        defs.push_back({{"intern_document", "Interns raw document bytes, returning an EntityId.",
                         object_schema({{"content", base64_string_property("Document content.")}}, {"content"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(InternDocumentCommand{require_bytes(args, "content")});
                        }});

        defs.push_back({{"record_provenance", "Records which source produced a given assertion, and by what method.",
                         object_schema({{"assertion_id", integer_property("Target AssertionId.")},
                                        {"source", integer_property("Source EntityId.")},
                                        {"recorded_at", integer_property("Timestamp the provenance was recorded.")},
                                        {"method", string_property("Free-text method description.")}},
                                       {"assertion_id", "source", "recorded_at", "method"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(RecordProvenanceCommand{
                                require_id(args, "assertion_id"), require_id(args, "source"),
                                require_timestamp(args, "recorded_at"), require_string(args, "method")});
                        }});

        defs.push_back({{"commit_hypothesis", "Commits a labeled, machine-suggested (Hypothesis-status) assertion.",
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
                                       {"subject", "predicate", "object", "valid_from", "valid_to", "observed_at",
                                        "confidence", "source", "recorded_at", "method"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(CommitHypothesisCommand{
                                require_id(args, "subject"), require_id(args, "predicate"), require_id(args, "object"),
                                require_timestamp(args, "valid_from"), require_timestamp(args, "valid_to"),
                                require_timestamp(args, "observed_at"), require_double(args, "confidence"),
                                require_id(args, "source"), require_timestamp(args, "recorded_at"),
                                require_string(args, "method")});
                        }});

        defs.push_back({{"merge_entities", "Merges absorb into keep: a one-way, append-only redirect.",
                         object_schema({{"keep", integer_property("Surviving EntityId.")},
                                        {"absorb", integer_property("Absorbed EntityId.")},
                                        {"merged_at", integer_property("Timestamp of the merge decision.")}},
                                       {"keep", "absorb", "merged_at"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(MergeEntitiesCommand{require_id(args, "keep"),
                                                                       require_id(args, "absorb"),
                                                                       require_timestamp(args, "merged_at")});
                        }});

        defs.push_back({{"archive_segments_before",
                         "Archives (compacts, does not delete) log segments entirely before an "
                         "AssertionId.",
                         object_schema({{"assertion_id", integer_property("Archive-before boundary AssertionId.")}},
                                       {"assertion_id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            kernel.execute(ArchiveSegmentsBeforeCommand{require_id(args, "assertion_id")});
                            return std::monostate{};
                        }});

        defs.push_back({{"get", "Fetches a single assertion by id.",
                         object_schema({{"id", integer_property("AssertionId.")}}, {"id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(GetCommand{require_id(args, "id")});
                        }});

        defs.push_back(
            {{"assertions_for_subject", "Returns every recorded assertion (any status) for a subject.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"limit", integer_property("Maximum number of results (omit or 0 for no limit).")}},
                            {"subject"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                 return kernel.execute(
                     AssertionsForSubjectCommand{require_id(args, "subject"), optional_size(args, "limit", 0)});
             }});

        defs.push_back({{"current", "Returns every currently active, open-ended assertion for a subject.",
                         object_schema({{"subject", integer_property("Subject EntityId.")}}, {"subject"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(CurrentCommand{require_id(args, "subject")});
                        }});

        defs.push_back({{"current_by_name",
                         "Returns every currently active, open-ended assertion for a subject looked up by name; "
                         "empty (not an error) if the name was never interned.",
                         object_schema({{"subject_name", string_property("Subject entity name.")}}, {"subject_name"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(CurrentByNameCommand{require_string(args, "subject_name")});
                        }});

        defs.push_back({{"current_by_object", "Reverse-direction lookup: who currently has the given entity as object.",
                         object_schema({{"object", integer_property("Object EntityId.")}}, {"object"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(CurrentByObjectCommand{require_id(args, "object")});
                        }});

        defs.push_back({{"current_by_predicate",
                         "Kernel-wide lookup: every currently active assertion for a predicate, any "
                         "subject.",
                         object_schema({{"predicate", integer_property("Predicate PredicateId.")}}, {"predicate"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(CurrentByPredicateCommand{require_id(args, "predicate")});
                        }});

        defs.push_back({{"valid_at", "Returns assertions valid at a given point in valid time.",
                         object_schema({{"subject", integer_property("Subject EntityId.")},
                                        {"valid_time", integer_property("Valid-time cutoff.")}},
                                       {"subject", "valid_time"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(
                                ValidAtCommand{require_id(args, "subject"), require_timestamp(args, "valid_time")});
                        }});

        defs.push_back({{"known_at",
                         "Returns assertions observed by a given point in observed time and still currently "
                         "Active.",
                         object_schema({{"subject", integer_property("Subject EntityId.")},
                                        {"observed_time", integer_property("Observed-time cutoff.")}},
                                       {"subject", "observed_time"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(
                                KnownAtCommand{require_id(args, "subject"), require_timestamp(args, "observed_time")});
                        }});

        defs.push_back({{"valid_at_known_at", "Combines valid_at and known_at cutoffs.",
                         object_schema({{"subject", integer_property("Subject EntityId.")},
                                        {"valid_time", integer_property("Valid-time cutoff.")},
                                        {"observed_time", integer_property("Observed-time cutoff.")}},
                                       {"subject", "valid_time", "observed_time"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(ValidAtKnownAtCommand{require_id(args, "subject"),
                                                                        require_timestamp(args, "valid_time"),
                                                                        require_timestamp(args, "observed_time")});
                        }});

        defs.push_back(
            {{"valid_time_timeline", "Returns active assertions for a subject/predicate sorted by valid_from.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")}},
                            {"subject", "predicate"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                 return kernel.execute(
                     ValidTimeTimelineCommand{require_id(args, "subject"), require_id(args, "predicate")});
             }});

        defs.push_back(
            {{"observed_time_timeline", "Returns active assertions for a subject/predicate sorted by observed_at.",
              object_schema({{"subject", integer_property("Subject EntityId.")},
                             {"predicate", integer_property("Predicate PredicateId.")}},
                            {"subject", "predicate"})},
             [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
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
             [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
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
             [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                 return kernel.execute(ChangesSinceCommand{require_timestamp(args, "observed_since"),
                                                           optional_size(args, "limit", 0),
                                                           optional_bool(args, "newest_first", false)});
             }});

        defs.push_back({{"explain", "Walks the supersession/retraction chain from an assertion back to its root.",
                         object_schema({{"id", integer_property("AssertionId to explain.")}}, {"id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(ExplainCommand{require_id(args, "id")});
                        }});

        defs.push_back({{"find_conflicts",
                         "Finds overlapping active assertions for a subject/predicate with different "
                         "objects.",
                         object_schema({{"subject", integer_property("Subject EntityId.")},
                                        {"predicate", integer_property("Predicate PredicateId.")}},
                                       {"subject", "predicate"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(
                                FindConflictsCommand{require_id(args, "subject"), require_id(args, "predicate")});
                        }});

        defs.push_back({{"find_entity", "Looks up a previously interned entity's id by name.",
                         object_schema({{"name", string_property("Entity name.")}}, {"name"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(FindEntityCommand{require_string(args, "name")});
                        }});

        defs.push_back({{"find_value", "Looks up a previously interned literal value's id.",
                         object_schema({{"value", value_property("The literal to look up.")}}, {"value"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(FindValueCommand{require_value(args, "value")});
                        }});

        defs.push_back({{"find_predicate", "Looks up a previously interned predicate's id by name.",
                         object_schema({{"name", string_property("Predicate name.")}}, {"name"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(FindPredicateCommand{require_string(args, "name")});
                        }});

        defs.push_back({{"entity_name", "Resolves a previously interned entity's name.",
                         object_schema({{"id", integer_property("EntityId.")}}, {"id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(EntityNameCommand{require_id(args, "id")});
                        }});

        defs.push_back({{"entity_value", "Resolves a previously interned entity's literal value.",
                         object_schema({{"id", integer_property("EntityId.")}}, {"id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(EntityValueCommand{require_id(args, "id")});
                        }});

        defs.push_back({{"predicate_name", "Resolves a previously interned predicate's name.",
                         object_schema({{"id", integer_property("PredicateId.")}}, {"id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(PredicateNameCommand{require_id(args, "id")});
                        }});

        defs.push_back({{"document_content", "Fetches previously interned document bytes.",
                         object_schema({{"id", integer_property("Document EntityId.")}}, {"id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(DocumentContentCommand{require_id(args, "id")});
                        }});

        defs.push_back({{"provenance_for", "Resolves recorded provenance for an assertion.",
                         object_schema({{"assertion_id", integer_property("AssertionId.")}}, {"assertion_id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(ProvenanceForCommand{require_id(args, "assertion_id")});
                        }});

        defs.push_back({{"hypotheses_for", "Returns open (Hypothesis-status) predictions for a subject.",
                         object_schema({{"subject", integer_property("Subject EntityId.")}}, {"subject"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(HypothesesForCommand{require_id(args, "subject")});
                        }});

        defs.push_back({{"neighbors", "Bounded breadth-first traversal of current-edge neighbors, both directions.",
                         object_schema({{"subject", integer_property("Subject EntityId.")},
                                        {"max_hops", integer_property("Maximum hop count.")}},
                                       {"subject", "max_hops"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(
                                NeighborsCommand{require_id(args, "subject"), require_size(args, "max_hops")});
                        }});

        defs.push_back({{"co_occurring_predicates", "Currently active predicates for a subject.",
                         object_schema({{"subject", integer_property("Subject EntityId.")}}, {"subject"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
                            return kernel.execute(CoOccurringPredicatesCommand{require_id(args, "subject")});
                        }});

        defs.push_back({{"resolve_entity", "Resolves an id through recorded merge redirects to its canonical id.",
                         object_schema({{"id", integer_property("EntityId.")}}, {"id"})},
                        [](KnowledgeKernel &kernel, const nlohmann::json &args) -> KernelResult {
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

ToolCallResult handle_tool_call(KnowledgeKernel &kernel, const std::string &tool_name,
                                const nlohmann::json &arguments) {
    try {
        for (const auto &definition : tool_definitions()) {
            if (definition.spec.name == tool_name) {
                KernelResult result = definition.invoke(kernel, arguments);
                return ToolCallResult{kernel_result_to_json(result).dump(), false};
            }
        }

        return ToolCallResult{"unknown tool: " + tool_name, true};
    } catch (const std::exception &error) {
        return ToolCallResult{std::string("error: ") + error.what(), true};
    }
}

} // namespace knk::mcp
