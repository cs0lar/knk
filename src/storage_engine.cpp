#include <bitemporal/storage_engine.hpp>

namespace bt {

StorageEngine::StorageEngine(StorageConfig config)
	: config_(std::move(config)),
	  fact_log_(config_.fact_log_path()) {
	std::filesystem::create_directories(config_.root);
	std::filesystem::create_directories(config_.index_directory());
	std::filesystem::create_directories(config_.payload_directory());
}

void StorageEngine::append_assertion(const Assertion& assertion) {
	fact_log_.append(assertion);
}

std::vector<Assertion> StorageEngine::load_assertions() const {
	return fact_log_.read_all();
}

const StorageConfig& StorageEngine::config() const {
	return config_;
}

}