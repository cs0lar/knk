#pragma once

#include <kernel/ids.hpp>
#include <kernel/time.hpp>
#include <kernel/status.hpp>


struct Assertion {
	AssertionId id;

	EntityId subject;
	PredicateId predicate;
	EntityId object;

	Timestamp valid_from;
	Timestamp valid_to;
	Timestamp observed_at;

	double confidence;

	AssertionStatus status;
};