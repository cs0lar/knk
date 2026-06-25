#include <bitemporal/assertion_store.hpp>

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <optional>

namespace bt {
AssertionId AssertionStore::append(
    EntityId subject,
    PredicateId predicate,
    EntityId object,
    Timestamp valid_from,
    Timestamp valid_to,
    Timestamp observed_at,
    double confidence
) {
	AssertionId id = next_id_++;

	Assertion a {
		id,
		subject,
		predicate,
		object,
		valid_from,
		valid_to,
		observed_at,
		confidence,
		AssertionStatus::Active
	};

	facts_.push_back(a);

	subject_index_[subject].push_back(id);
	current_index_[subject][predicate].push_back(id);

	return id;
}

std::optional<Assertion> AssertionStore::get(AssertionId id) const {
	if (id == 0 || id >= next_id_) {
		return std::nullopt;
	}

	return facts_[id - 1];
}

std::vector<Assertion> AssertionStore::facts_for_subject(EntityId subject) const {
	std::vector<Assertion> result;

	auto it = subject_index_.find(subject);
	if (it == subject_index_.end()) {
		return result;
	}

	for (AssertionId id : it->second) {
		auto fact = get(id);
		if (fact.has_value()) {
			result.push_back(*fact);
		}
	}

	return result;
}

std::vector<Assertion> AssertionStore::current_facts(EntityId subject) const {
	std::vector<Assertion> result;

	auto subject_id = current_index_.find(subject);
	if (subject_id == current_index_.end()) {
		return result;
	}

	for (const auto& [predicate, ids] : subject_id->second) {
		for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
			auto fact = get(*it);

			if (
			    fact.has_value() &&
			    fact->status == AssertionStatus::Active &&
			    fact->valid_to == OPEN_ENDED
			) {
				result.push_back(*fact);
				break;
			}
		}
	}

	return result;
}

std::vector<Assertion> AssertionStore::valid_at(EntityId subject, Timestamp t) const {
	std::vector<Assertion> result;

	auto it = subject_index_.find(subject);
	if (it == subject_index_.end()) {
		return result;
	}

	for (AssertionId id : it->second) {
		auto fact = get(id);
		if (!fact.has_value()) {
			continue;
		}

		bool starts_before_or_at = fact->valid_from <= t;
		bool ends_after = fact->valid_to == OPEN_ENDED || t < fact->valid_to;

		if (
		    fact->status == AssertionStatus::Active &&
		    starts_before_or_at &&
		    ends_after
		) {
			result.push_back(*fact);
		}
	}

	return result;
}
}