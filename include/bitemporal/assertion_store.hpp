#pragma once

#include <optional>
#include <vector>
#include <unordered_map>

#include <bitemporal/ids.hpp>
#include <bitemporal/time.hpp>
#include <bitemporal/status.hpp>
#include <bitemporal/assertion.hpp>

namespace bitemporal::assertion_store {
class AssertionStore {
public:
	AssertionId append(
	    EntityId subject,
	    PredicateId predicate,
	    EntityId object,
	    Timestamp valid_from,
	    Timestamp valid_to,
	    Timestamp observed_at,
	    double confidence
	);

	std::optional<Assertion> get(AssertionId id) const;

	std::vector<Assertion> facts_for_subject(EntityId subject) const;

	std::vector<Assertion> current_facts(EntityId subject) const;

	std::vector<Assertion> valid_at(EntityId subject, Timestamp valid_time) const;

	std::vector<Assertion> known_at(EntityId subject, Timestamp observed_time) const;

	std::vector<Assertion> valid_at_known_at(
	    EntityId subject,
	    Timestamp valid_time,
	    Timestamp observed_time
	) const;
private:
	AssertionId next_id_ = 1;

	std::vector<Assertion> facts_;

	std::unordered_map<EntityId, std::vector<AssertionId>> subject_index_;

	std::unordered_map<EntityId, std::unordered_map<PredicateId, std::vector<AssertionId>>> current_index_;
};
}
