#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kernel/aggregate.hpp"
#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/provenance_log.hpp"
#include "kernel/query.hpp"
#include "kernel/query_plan.hpp"
#include "kernel/schema.hpp"
#include "kernel/spill.hpp"
#include "kernel/value.hpp"

namespace knk {

// The result of KnowledgeKernel::execute: one closed variant over every distinct return type of the
// mirrored public methods.
//
// Note that AssertionId/EntityId/PredicateId are all uint64_t aliases (see ids.hpp), so an
// id-returning command (commit family, intern_entity/value/predicate/document) yields the single
// `AssertionId` alternative regardless of which id kind it semantically is; likewise
// find_entity/find_value/find_predicate share the single `std::optional<AssertionId>` alternative.
// The caller already knows the semantic id kind from the command it issued, so collapsing them costs
// nothing and keeps the variant well-formed (a variant cannot hold two identical alternatives).
//
// Void-returning commands (write_snapshot, record_provenance) yield std::monostate.
//
// std::vector<EntityId> covers neighbors (vector<EntityId>), co_occurring_predicates
// (vector<PredicateId>), and commit_batch (vector<AssertionId>) for the same reason the scalar ids
// above collapse: EntityId/PredicateId/AssertionId are the same uint64_t alias, so those return
// types are identical and share one alternative.
//
// The batch reads each return a vector of their single resolver's optional, and collapse the same way
// the singles do: entity_name_batch and predicate_name_batch share vector<optional<string>>, exactly as
// entity_name and predicate_name share optional<string>.
// QueryResult (Phase 10) is its own alternative rather than reusing vector<Assertion>: it carries a
// truncated flag alongside the rows, which a bare vector cannot express.
using KernelResult = std::variant<std::monostate,                               // write_snapshot, record_provenance
                                  AssertionId,                                  // commit family; intern_* (ids)
                                  std::optional<Assertion>,                     // get
                                  std::vector<Assertion>,                       // current/valid_at/.../explain
                                  std::vector<std::pair<Assertion, Assertion>>, // find_conflicts
                                  std::optional<AssertionId>,                   // find_entity/find_value/find_predicate
                                  std::optional<std::string>,                   // entity_name/predicate_name
                                  std::optional<Value>,                         // entity_value
                                  std::optional<std::vector<std::byte>>,        // document_content
                                  std::optional<ProvenanceRecord>,              // provenance_for
                                  std::vector<EntityId>, // neighbors, co_occurring_predicates, commit_batch
                                  std::vector<std::optional<std::string>>, // entity_name_batch/predicate_name_batch
                                  std::vector<std::optional<Value>>,       // entity_value_batch
                                  std::vector<std::optional<ProvenanceRecord>>, // provenance_for_batch
                                  QueryResult,                                  // query
                                  AggregateResult,                              // aggregate
                                  QueryPlan,                                    // explain_query
                                  SpillDescriptor,                              // spill_query
                                  std::vector<PredicateSummary>,                // describe_predicates
                                  CorpusSummary>;                               // describe_corpus

} // namespace knk
