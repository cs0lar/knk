#pragma once

#include <filesystem>
#include <vector>

#include <kernel/assertion.hpp>

namespace knk {

class AssertionLog {
  public:
    explicit AssertionLog(std::filesystem::path path);

    void append(const Assertion &assertion);

    std::vector<Assertion> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk