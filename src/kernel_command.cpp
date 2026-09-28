#include <type_traits>
#include <utility>
#include <variant>

#include "kernel/kernel_command.hpp"
#include "kernel/kernel_result.hpp"
#include "kernel/knowledge_kernel.hpp"

namespace knk {

namespace {

// Dependent-false helper: makes the final `else` static_assert fire only if a new KernelCommand
// alternative is added without a matching dispatch branch below (an unhandled type would otherwise
// fall through the if-constexpr chain and reach the end of the visitor with no return).
template <class> inline constexpr bool always_false = false;

} // namespace

KernelResult KnowledgeKernel::execute(const KernelCommand &command) {
    return std::visit(
        [this](const auto &cmd) -> KernelResult {
            using T = std::decay_t<decltype(cmd)>;

            if constexpr (std::is_same_v<T, CommitCommand>) {
                return commit(cmd.subject, cmd.predicate, cmd.object, cmd.valid_from, cmd.valid_to, cmd.observed_at,
                              cmd.confidence);
            } else if constexpr (std::is_same_v<T, CommitByNameCommand>) {
                return commit_by_name(cmd.subject_name, cmd.predicate_name, cmd.object, cmd.valid_from, cmd.valid_to,
                                      cmd.observed_at, cmd.confidence);
            } else if constexpr (std::is_same_v<T, CommitBatchCommand>) {
                return commit_batch(cmd.entries);
            } else if constexpr (std::is_same_v<T, CommitBatchByNameCommand>) {
                return commit_batch_by_name(cmd.entries);
            } else if constexpr (std::is_same_v<T, CommitRetractionCommand>) {
                return commit_retraction(cmd.subject, cmd.predicate, cmd.object, cmd.valid_from, cmd.valid_to,
                                         cmd.observed_at, cmd.confidence, cmd.retracts_id);
            } else if constexpr (std::is_same_v<T, CommitSupersedingCommand>) {
                return commit_superseding(cmd.subject, cmd.predicate, cmd.object, cmd.valid_from, cmd.valid_to,
                                          cmd.observed_at, cmd.confidence, cmd.supersedes_id);
            } else if constexpr (std::is_same_v<T, WriteSnapshotCommand>) {
                write_snapshot();
                return std::monostate{};
            } else if constexpr (std::is_same_v<T, InternEntityCommand>) {
                return intern_entity(cmd.name);
            } else if constexpr (std::is_same_v<T, InternValueCommand>) {
                return intern_value(cmd.value);
            } else if constexpr (std::is_same_v<T, InternPredicateCommand>) {
                return intern_predicate(cmd.name);
            } else if constexpr (std::is_same_v<T, InternDocumentCommand>) {
                return intern_document(cmd.content);
            } else if constexpr (std::is_same_v<T, RecordProvenanceCommand>) {
                record_provenance(cmd.assertion_id, cmd.source, cmd.recorded_at, cmd.method);
                return std::monostate{};
            } else if constexpr (std::is_same_v<T, RecordProvenanceBatchCommand>) {
                record_provenance_batch(cmd.records);
                return std::monostate{};
            } else if constexpr (std::is_same_v<T, CommitHypothesisCommand>) {
                return commit_hypothesis(cmd.subject, cmd.predicate, cmd.object, cmd.valid_from, cmd.valid_to,
                                         cmd.observed_at, cmd.confidence, cmd.source, cmd.recorded_at, cmd.method);
            } else if constexpr (std::is_same_v<T, MergeEntitiesCommand>) {
                merge_entities(cmd.keep, cmd.absorb, cmd.merged_at);
                return std::monostate{};
            } else if constexpr (std::is_same_v<T, ArchiveSegmentsBeforeCommand>) {
                archive_segments_before(cmd.assertion_id);
                return std::monostate{};
            } else if constexpr (std::is_same_v<T, GetCommand>) {
                return get(cmd.id);
            } else if constexpr (std::is_same_v<T, AssertionsForSubjectCommand>) {
                return assertions_for_subject(cmd.subject, cmd.limit);
            } else if constexpr (std::is_same_v<T, CurrentCommand>) {
                return current(cmd.subject);
            } else if constexpr (std::is_same_v<T, CurrentByNameCommand>) {
                return current_by_name(cmd.subject_name);
            } else if constexpr (std::is_same_v<T, CurrentByObjectCommand>) {
                return current_by_object(cmd.object);
            } else if constexpr (std::is_same_v<T, CurrentByPredicateCommand>) {
                return current_by_predicate(cmd.predicate);
            } else if constexpr (std::is_same_v<T, ValidAtCommand>) {
                return valid_at(cmd.subject, cmd.valid_time);
            } else if constexpr (std::is_same_v<T, KnownAtCommand>) {
                return known_at(cmd.subject, cmd.observed_time);
            } else if constexpr (std::is_same_v<T, ValidAtKnownAtCommand>) {
                return valid_at_known_at(cmd.subject, cmd.valid_time, cmd.observed_time);
            } else if constexpr (std::is_same_v<T, ValidTimeTimelineCommand>) {
                return valid_time_timeline(cmd.subject, cmd.predicate);
            } else if constexpr (std::is_same_v<T, ObservedTimeTimelineCommand>) {
                return observed_time_timeline(cmd.subject, cmd.predicate);
            } else if constexpr (std::is_same_v<T, CommitHistoryCommand>) {
                return commit_history(cmd.subject, cmd.predicate, cmd.limit);
            } else if constexpr (std::is_same_v<T, ChangesSinceCommand>) {
                return changes_since(cmd.observed_since, cmd.limit, cmd.newest_first);
            } else if constexpr (std::is_same_v<T, ExplainCommand>) {
                return explain(cmd.id);
            } else if constexpr (std::is_same_v<T, FindConflictsCommand>) {
                return find_conflicts(cmd.subject, cmd.predicate);
            } else if constexpr (std::is_same_v<T, FindEntityCommand>) {
                return find_entity(cmd.name);
            } else if constexpr (std::is_same_v<T, FindValueCommand>) {
                return find_value(cmd.value);
            } else if constexpr (std::is_same_v<T, FindPredicateCommand>) {
                return find_predicate(cmd.name);
            } else if constexpr (std::is_same_v<T, EntityNameCommand>) {
                return entity_name(cmd.id);
            } else if constexpr (std::is_same_v<T, EntityValueCommand>) {
                return entity_value(cmd.id);
            } else if constexpr (std::is_same_v<T, PredicateNameCommand>) {
                return predicate_name(cmd.id);
            } else if constexpr (std::is_same_v<T, DocumentContentCommand>) {
                return document_content(cmd.id);
            } else if constexpr (std::is_same_v<T, ProvenanceForCommand>) {
                return provenance_for(cmd.assertion_id);
            } else if constexpr (std::is_same_v<T, QueryCommand>) {
                return query(cmd.query);
            } else if constexpr (std::is_same_v<T, AggregateCommand>) {
                return aggregate(cmd.query);
            } else if constexpr (std::is_same_v<T, EntityNameBatchCommand>) {
                return entity_name_batch(cmd.ids);
            } else if constexpr (std::is_same_v<T, EntityValueBatchCommand>) {
                return entity_value_batch(cmd.ids);
            } else if constexpr (std::is_same_v<T, PredicateNameBatchCommand>) {
                return predicate_name_batch(cmd.ids);
            } else if constexpr (std::is_same_v<T, ProvenanceForBatchCommand>) {
                return provenance_for_batch(cmd.assertion_ids);
            } else if constexpr (std::is_same_v<T, HypothesesForCommand>) {
                return hypotheses_for(cmd.subject);
            } else if constexpr (std::is_same_v<T, NeighborsCommand>) {
                return neighbors(cmd.subject, cmd.max_hops);
            } else if constexpr (std::is_same_v<T, CoOccurringPredicatesCommand>) {
                return co_occurring_predicates(cmd.subject);
            } else if constexpr (std::is_same_v<T, ResolveEntityCommand>) {
                return resolve_entity(cmd.id);
            } else {
                static_assert(always_false<T>, "unhandled KernelCommand alternative");
            }
        },
        command);
}

} // namespace knk
