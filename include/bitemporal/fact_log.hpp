#pragma once

#include <filesystem>
#include <vector>

#include <bitemporal/assertion.hpp>

namespace bt {

class FactLog {
public:
	explicit FactLog(std::filesystem::path path);

	void append(const Assertion& assertion);

	std::vector<Assertion> read_all() const;

private:
	std::filesystem::path path_;
};

}