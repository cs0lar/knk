#pragma once

#include <compare>
#include <functional>
#include <kernel/ids.hpp>

namespace knk {

struct SubjectPredicateKey {
    EntityId subject;
    PredicateId predicate;

    auto operator<=>(const SubjectPredicateKey &) const = default;
};

} // namespace knk

template <>
struct std::hash<knk::SubjectPredicateKey> {
    std::size_t operator()(const knk::SubjectPredicateKey &key) const {
        std::size_t h1 = std::hash<knk::EntityId>{}(key.subject);
        std::size_t h2 = std::hash<knk::PredicateId>{}(key.predicate);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};