#pragma once

#include <filesystem>

namespace bt {

struct StorageConfig {
	std::filesystem::path root;

	std::filesystem::path fact_log_path() const {
		return root / "facts.log";
	}

	std::filesystem::path index_directory() const {
		return root / "indexes";
	}

	std::filesystem::path payload_directory() const {
		return root / "payloads";
	}
};

}