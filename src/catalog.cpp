#include <algorithm>

#include "kernel/catalog.hpp"

namespace knk {

void Catalog::add_entity(EntityId id, const Value &value) {
    entity_ids_[value] = id;
    entity_values_[id] = value;
    next_entity_id_ = std::max(next_entity_id_, id + 1);
}

void Catalog::add_predicate(PredicateId id, const std::string &name) {
    predicate_ids_[name] = id;
    predicate_names_[id] = name;
    next_predicate_id_ = std::max(next_predicate_id_, id + 1);
}

EntityId Catalog::allocate_entity_id() {
    EntityId id = next_entity_id_;
    next_entity_id_ = id + 1;
    return id;
}

void Catalog::note_allocated_entity_id(EntityId id) { next_entity_id_ = std::max(next_entity_id_, id + 1); }

std::optional<EntityId> Catalog::find_entity(const Value &value) const {
    auto it = entity_ids_.find(value);
    if (it == entity_ids_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<PredicateId> Catalog::find_predicate(const std::string &name) const {
    auto it = predicate_ids_.find(name);
    if (it == predicate_ids_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<Value> Catalog::entity_value(EntityId id) const {
    auto it = entity_values_.find(id);
    if (it == entity_values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> Catalog::predicate_name(PredicateId id) const {
    auto it = predicate_names_.find(id);
    if (it == predicate_names_.end()) {
        return std::nullopt;
    }
    return it->second;
}

EntityId Catalog::next_entity_id() const { return next_entity_id_; }

PredicateId Catalog::next_predicate_id() const { return next_predicate_id_; }

} // namespace knk
