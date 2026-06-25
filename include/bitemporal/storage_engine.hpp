#pragma once

#include <filesystem>
#include <vector>

#include <bitemporal/assertion.hpp>
#include <bitemporal/fact_log.hpp>
#include <bitemporal/storage_config.hpp>

namespace bt {

class StorageEngine {
public:
	explicit StorageEngine(StorageConfig config);

	void append_assertion(const Assertion& assertion);

	std::vector<Assertion> load_assertions() const;

	const StorageConfig& config() const;

private:
	StorageConfig config_;
	FactLog fact_log_;
}

}