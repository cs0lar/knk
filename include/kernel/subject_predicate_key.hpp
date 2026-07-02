#pragma once

#include <compare>
#include <kernel/ids.hpp>

namespace knk {

struct SubjectPredicateKey {
    EntityId subject;
    PredicateId predicate;

    auto operator<=>(const SubjectPredicateKey &) const = default;
};

} // namespace knk