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

    std::optional<EntityId> find_entity(const Value &value) const;
    std::optional<PredicateId> find_predicate(const std::string &name) const;

    std::optional<Value> entity_value(EntityId id) const;
    std::optional<std::string> predicate_name(PredicateId id) const;

    EntityId next_entity_id() const;
    PredicateId next_predicate_id() const;

  private:
    std::unordered_map<Value, EntityId> entity_ids_;
    std::unordered_map<EntityId, Value> entity_values_;
    std::unordered_map<std::string, PredicateId> predicate_ids_;
    std::unordered_map<PredicateId, std::string> predicate_names_;

    EntityId next_entity_id_ = 1;
    PredicateId next_predicate_id_ = 1;
};

} // namespace knk
