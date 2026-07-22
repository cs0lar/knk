#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/time.hpp"
#include "kernel/value.hpp"

namespace knk {

// A closed, serializable reification of every public KnowledgeKernel operation an external caller
// (an agent today; an MCP/HTTP/gRPC boundary added later) might issue. Each struct holds exactly the
// arguments of the mirrored method, and KnowledgeKernel::execute maps one command to one method
// call, 1:1, with no added business logic. This is deliberately NOT a query language: every command
// mirrors an existing method exactly, reified as data so a boundary can serialize a call instead of
// doing direct C++ method dispatch. No network transport or wire encoding is added in this phase --
// the point is that these are plain-data structs that a future boundary can serialize.
//
// The internal replay hooks apply()/mark_superseded()/mark_retracted() are intentionally excluded:
// they mutate in-memory status without a corresponding durable record (bypassing durable-before-
// visible and append-only) and exist only for log replay, not as operations a caller should issue.

// --- Mutating commands -----------------------------------------------------------

struct CommitCommand {
    EntityId subject;
    PredicateId predicate;
    EntityId object;
    Timestamp valid_from;
    Timestamp valid_to;
    Timestamp observed_at;
    double confidence;
};

struct CommitByNameCommand {
    std::string subject_name;
    std::string predicate_name;
    Value object;
    Timestamp valid_from;
    Timestamp valid_to;
    Timestamp observed_at;
    double confidence;
};

struct CommitRetractionCommand {
    EntityId subject;
    PredicateId predicate;
    EntityId object;
    Timestamp valid_from;
    Timestamp valid_to;
    Timestamp observed_at;
    double confidence;
    AssertionId retracts_id;
};

struct CommitSupersedingCommand {
    EntityId subject;
    PredicateId predicate;
    EntityId object;
    Timestamp valid_from;
    Timestamp valid_to;
    Timestamp observed_at;
    double confidence;
    AssertionId supersedes_id;
};

struct WriteSnapshotCommand {};

struct InternEntityCommand {
    std::string name;
};

struct InternValueCommand {
    Value value;
};

struct InternPredicateCommand {
    std::string name;
};

struct InternDocumentCommand {
    std::vector<std::byte> content;
};

struct RecordProvenanceCommand {
    AssertionId assertion_id;
    EntityId source;
    Timestamp recorded_at;
    std::string method;
};

struct CommitHypothesisCommand {
    EntityId subject;
    PredicateId predicate;
    EntityId object;
    Timestamp valid_from;
    Timestamp valid_to;
    Timestamp observed_at;
    double confidence;
    EntityId source;
    Timestamp recorded_at;
    std::string method;
};

struct MergeEntitiesCommand {
    EntityId keep;
    EntityId absorb;
    Timestamp merged_at;
};

struct ArchiveSegmentsBeforeCommand {
    AssertionId assertion_id;
};

// --- Query commands --------------------------------------------------------------

struct GetCommand {
    AssertionId id;
};

struct AssertionsForSubjectCommand {
    EntityId subject;
};

struct CurrentCommand {
    EntityId subject;
};

struct CurrentByObjectCommand {
    EntityId object;
};

struct CurrentByPredicateCommand {
    PredicateId predicate;
};

struct ValidAtCommand {
    EntityId subject;
    Timestamp valid_time;
};

struct KnownAtCommand {
    EntityId subject;
    Timestamp observed_time;
};

struct ValidAtKnownAtCommand {
    EntityId subject;
    Timestamp valid_time;
    Timestamp observed_time;
};

struct ValidTimeTimelineCommand {
    EntityId subject;
    PredicateId predicate;
};

struct ObservedTimeTimelineCommand {
    EntityId subject;
    PredicateId predicate;
};

struct CommitHistoryCommand {
    EntityId subject;
    PredicateId predicate;
};

struct ChangesSinceCommand {
    Timestamp observed_since;
};

struct ExplainCommand {
    AssertionId id;
};

struct FindConflictsCommand {
    EntityId subject;
    PredicateId predicate;
};

struct FindEntityCommand {
    std::string name;
};

struct FindValueCommand {
    Value value;
};

struct FindPredicateCommand {
    std::string name;
};

struct EntityNameCommand {
    EntityId id;
};

struct EntityValueCommand {
    EntityId id;
};

struct PredicateNameCommand {
    PredicateId id;
};

struct DocumentContentCommand {
    EntityId id;
};

struct ProvenanceForCommand {
    AssertionId assertion_id;
};

struct HypothesesForCommand {
    EntityId subject;
};

struct NeighborsCommand {
    EntityId subject;
    size_t max_hops;
};

struct CoOccurringPredicatesCommand {
    EntityId subject;
};

struct ResolveEntityCommand {
    EntityId id;
};

using KernelCommand = std::variant<
    CommitCommand, CommitByNameCommand, CommitRetractionCommand, CommitSupersedingCommand, WriteSnapshotCommand,
    InternEntityCommand, InternValueCommand, InternPredicateCommand, InternDocumentCommand, RecordProvenanceCommand,
    CommitHypothesisCommand, MergeEntitiesCommand, ArchiveSegmentsBeforeCommand, GetCommand,
    AssertionsForSubjectCommand, CurrentCommand, CurrentByObjectCommand, CurrentByPredicateCommand, ValidAtCommand,
    KnownAtCommand, ValidAtKnownAtCommand, ValidTimeTimelineCommand, ObservedTimeTimelineCommand, CommitHistoryCommand,
    ChangesSinceCommand, ExplainCommand, FindConflictsCommand, FindEntityCommand, FindValueCommand,
    FindPredicateCommand, EntityNameCommand, EntityValueCommand, PredicateNameCommand, DocumentContentCommand,
    ProvenanceForCommand, HypothesesForCommand, NeighborsCommand, CoOccurringPredicatesCommand, ResolveEntityCommand>;

} // namespace knk
