#pragma once

#include <optional>
#include <string_view>

namespace knk {

enum class AssertionStatus { Active, Superseded, Retracted, Retraction, Hypothesis };

// The canonical spelling of each status. Promoted out of json_codec.cpp (2026-09-28) when the query
// IR's Status filter needed the same names: two independent switch statements over the same enum are
// exactly the kind of duplication that drifts the first time a status is added.
inline const char *status_name(AssertionStatus status) {
    switch (status) {
    case AssertionStatus::Active:
        return "Active";
    case AssertionStatus::Superseded:
        return "Superseded";
    case AssertionStatus::Retracted:
        return "Retracted";
    case AssertionStatus::Retraction:
        return "Retraction";
    case AssertionStatus::Hypothesis:
        return "Hypothesis";
    }

    return "Active"; // unreachable for a valid enumerator; keeps the function total
}

// Returns nullopt for an unrecognized name; callers decide whether that is an error (the JSON codec
// and the query IR both treat it as one) rather than having the mapping decide for them.
inline std::optional<AssertionStatus> status_from_name(std::string_view name) {
    if (name == "Active") {
        return AssertionStatus::Active;
    }
    if (name == "Superseded") {
        return AssertionStatus::Superseded;
    }
    if (name == "Retracted") {
        return AssertionStatus::Retracted;
    }
    if (name == "Retraction") {
        return AssertionStatus::Retraction;
    }
    if (name == "Hypothesis") {
        return AssertionStatus::Hypothesis;
    }

    return std::nullopt;
}

} // namespace knk
