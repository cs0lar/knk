#pragma once

#include <bitemporal/ids.hpp>
#include <bitemporal/time.hpp>
#include <bitemporal/status.hpp>


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