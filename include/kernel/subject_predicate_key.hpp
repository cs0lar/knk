#pragma once

#include <compare>
#include <kernel/ids.hpp>

struct SubjectPredicateKey {
    EntityId subject;
    PredicateId predicate;

    auto operator<=>(const SubjectPredicateKey &) const = default;
};