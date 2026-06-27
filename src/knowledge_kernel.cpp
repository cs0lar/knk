#include <kernel/knowledge_kernel.hpp>

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <optional>

namespace knk {

AssertionId KnowledgeKernel::commit(
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

	assertions_.push_back(a);

	subject_index_[subject].push_back(id);
	current_index_[subject][predicate].push_back(id);

	return id;
}

std::optional<Assertion> KnowledgeKernel::get(AssertionId id) const {
	if (id == 0 || id >= next_id_) {
		return std::nullopt;
	}

	return assertions_[id - 1];
}

std::optional<Assertion> KnowledgeKernel::retract(AssertionId id) {
	if (id == 0 || id >= next_id_) {
		return std::nullopt;
	}
	assertions_[id - 1].status = AssertionStatus::Retracted;
	return assertions_[id - 1];
}

std::vector<Assertion> KnowledgeKernel::assertions_for_subject(EntityId subject) const {
	std::vector<Assertion> result;

	auto it = subject_index_.find(subject);
	if (it == subject_index_.end()) {
		return result;
	}

	for (AssertionId id : it->second) {
		auto assertion = get(id);
		if (assertion.has_value()) {
			result.push_back(*assertion);
		}
	}

	return result;
}

std::vector<Assertion> KnowledgeKernel::current(EntityId subject) const {
	std::vector<Assertion> result;

	auto subject_id = current_index_.find(subject);
	if (subject_id == current_index_.end()) {
		return result;
	}

	for (const auto& [predicate, ids] : subject_id->second) {
		for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
			auto assertion = get(*it);

			if (
			    assertion.has_value() &&
			    assertion->status == AssertionStatus::Active &&
			    assertion->valid_to == OPEN_ENDED
			) {
				result.push_back(*assertion);
				break;
			}
		}
	}

	return result;
}

std::vector<Assertion> KnowledgeKernel::valid_at(EntityId subject, Timestamp t) const {
	std::vector<Assertion> result;

	auto it = subject_index_.find(subject);
	if (it == subject_index_.end()) {
		return result;
	}

	for (AssertionId id : it->second) {
		auto assertion = get(id);
		if (!assertion.has_value()) {
			continue;
		}

		bool starts_before_or_at = assertion->valid_from <= t;
		bool ends_after = assertion->valid_to == OPEN_ENDED || t < assertion->valid_to;

		if (
		    assertion->status == AssertionStatus::Active &&
		    starts_before_or_at &&
		    ends_after
		) {
			result.push_back(*assertion);
		}
	}

	return result;
}

std::vector<Assertion> KnowledgeKernel::known_at(EntityId subject, Timestamp t) const {
	std::vector<Assertion> result;

	auto it = subject_index_.find(subject);
	if (it == subject_index_.end()) {
		return result;
	}

	for (AssertionId id : it->second) {
		auto assertion = get(id);
		if (!assertion.has_value()) {
			continue;
		}

		bool is_observed = assertion->observed_at <= t;

		if (
		    assertion->status == AssertionStatus::Active &&
		    is_observed
		) {
			result.push_back(*assertion);
		}
	}

	return result;
}

std::vector<Assertion> KnowledgeKernel::valid_at_known_at(
    EntityId subject,
    Timestamp valid_time,
    Timestamp observed_time
) const {
	std::vector<Assertion> result;

	auto it = subject_index_.find(subject);
	if (it == subject_index_.end()) {
		return result;
	}

	for (AssertionId id : it->second) {
		auto assertion = get(id);
		if (!assertion.has_value()) {
			continue;
		}

		bool starts_before_or_at = assertion->valid_from <= valid_time;
		bool ends_after = assertion->valid_to == OPEN_ENDED || valid_time < assertion->valid_to;
		bool is_observed = assertion->observed_at <= observed_time;

		if (
		    assertion->status == AssertionStatus::Active &&
		    is_observed &&
		    starts_before_or_at &&
		    ends_after
		) {
			result.push_back(*assertion);
		}
	}

	return result;
}
}