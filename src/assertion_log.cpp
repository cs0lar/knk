#include <kernel/assertion_log.hpp>

#include <fstream>
#include <stdexcept>

namespace knk {

namespace {

constexpr uint32_t ASSERTION_RECORD_SIZE = sizeof(Assertion);

void write_or_throw(std::ofstream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write fact log");
    }
}

} // namespace

AssertionLog::AssertionLog(std::filesystem::path path) : path_(std::move(path)) {}

void AssertionLog::append(const Assertion &assertion) {
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open fact log for append");
    }

    uint32_t record_size = ASSERTION_RECORD_SIZE;

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, reinterpret_cast<const char *>(&assertion), sizeof(assertion));
}

std::vector<Assertion> AssertionLog::read_all() const {
    std::vector<Assertion> assertions;

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return assertions;
    }

    while (true) {
        uint32_t record_size = 0;

        in.read(reinterpret_cast<char *>(&record_size), sizeof(record_size));

        if (in.eof()) {
            break;
        }

        if (!in) {
            break;
        }

        if (record_size != ASSERTION_RECORD_SIZE) {
            throw std::runtime_error("invalid fact log record size");
        }

        Assertion assertion{};
        in.read(reinterpret_cast<char *>(&assertion), sizeof(assertion));

        if (!in) {
            break; // ignore incomplete trailing record for now
        }

        assertions.push_back(assertion);
    }

    return assertions;
}

} // namespace knk