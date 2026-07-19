#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "kernel/ids.hpp"
#include "kernel/value.hpp"

namespace knk {

// In-memory EntityId/PredicateId <-> name/value mappings. Unlike IndexManager, Catalog is not a
// derived projection of assertions.log -- it has exactly one source of truth, its own persisted
// log files, so add_entity/add_predicate are the single mutation entry point used identically for
// a fresh interning commit and for full replay at startup (there is no separate "restore" path).
class Catalog {
  public:
    void add_entity(EntityId id, const Value &value);
    void add_predicate(PredicateId id, const std::string &name);

    // Mints a fresh EntityId for a document (PayloadStore-backed content), sharing the same id
    // counter/space as add_entity's name/value ids but recording no name/value mapping for it --
    // a document has no reverse "content -> id" lookup, so there is nothing to put in entity_ids_/
    // entity_values_. The counter still needs to survive restarts, so a replayed document id is
    // fed back in via note_allocated_entity_id below.
    EntityId allocate_entity_id();

    // Advances next_entity_id_ past a document id discovered by replaying PayloadStore's contents
    // at startup, without adding any name/value mapping -- the id-space-continuity half of
    // allocate_entity_id's bump, replayed out of band since documents aren't stored in
    // entities.log.
    void note_allocated_entity_id(EntityId id);

    std::optional<EntityId> find_entity(const Value &value) const;
    std::optional<PredicateId> find_predicate(const std::string &name) const;

    std::optional<Value> entity_value(EntityId id) const;
    std::optional<std::string> predicate_name(PredicateId id) const;

    EntityId next_entity_id() const;
    PredicateId next_predicate_id() const;

    // Records a one-way redirect: absorbed is superseded by surviving for entity-resolution purposes.
    // The single mutation entry point for both a fresh merge_entities call and full replay at startup,
    // mirroring add_entity/add_predicate. Does not itself resolve chains -- resolve() below does that
    // transitively at read time -- so a later merge of `surviving` into some third id does not require
    // rewriting this entry.
    void add_merge(EntityId absorbed, EntityId surviving);

    // Follows merge redirects transitively (merging A into B, then B into C, makes resolve(A) == C)
    // until reaching an id with no outgoing redirect. Identity for an id that was never absorbed.
    // Guards against a malformed cycle (never expected in practice, since merges are meant to be
    // forward-only) by stopping and returning the last id reached rather than looping forever.
    EntityId resolve(EntityId id) const;

  private:
    std::unordered_map<Value, EntityId> entity_ids_;
    std::unordered_map<EntityId, Value> entity_values_;
    std::unordered_map<std::string, PredicateId> predicate_ids_;
    std::unordered_map<PredicateId, std::string> predicate_names_;
    std::unordered_map<EntityId, EntityId> merge_redirects_;

    EntityId next_entity_id_ = 1;
    PredicateId next_predicate_id_ = 1;
};

} // namespace knk
