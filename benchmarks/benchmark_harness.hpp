#pragma once

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>

// Minimal shared timing/reporting helpers for the benchmarks/ executables. Deliberately not a
// generic micro-benchmark framework (no statistical sampling, no registration macros) -- each
// benchmark drives its own loop and reports the numbers that matter for that hot path, matching the
// repo's "no unnecessary dependencies" style used for tests.
namespace knk::benchmark {

class Timer {
  public:
    Timer() : start_(std::chrono::steady_clock::now()) {}

    double elapsed_seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }

  private:
    std::chrono::steady_clock::time_point start_;
};

// Prints one aligned "<label>: <value> <unit>" line so output stays easy to diff between runs
// (e.g. before/after an optimization, per the Performance Rules workflow in AGENTS.md).
inline void report(const std::string &label, double value, const std::string &unit) {
    std::cout << std::left << std::setw(48) << label << std::right << std::setw(14) << std::fixed
              << std::setprecision(3) << value << " " << unit << "\n";
}

inline void section(const std::string &title) {
    std::cout << "\n== " << title << " ==\n";
}

} // namespace knk::benchmark
