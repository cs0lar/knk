#include <array>
#include <fstream>
#include <stdexcept>

#include "kernel/assertion_log.hpp"
#include "kernel/checksum.hpp"

namespace knk {

namespace {

constexpr uint32_t ASSERTION_RECORD_SIZE = sizeof(Assertion);
constexpr std::array<char, 4> LOG_MAGIC{'K', 'N', 'K', '1'};
constexpr uint32_t LOG_FORMAT_VERSION = 1;

void write_or_throw(std::ofstream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write fact log");
    }
}

void write_header(std::ofstream &out) {
    write_or_throw(out, LOG_MAGIC.data(), static_cast<std::streamsize>(LOG_MAGIC.size()));

    uint32_t version = LOG_FORMAT_VERSION;
    write_or_throw(out, reinterpret_cast<const char *>(&version), sizeof(version));
}

// Returns false if the file is empty (no header, treated as an empty log).
bool read_and_validate_header(std::ifstream &in) {
    std::array<char, 4> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));

    if (in.gcount() == 0) {
        return false;
    }

    if (!in || static_cast<size_t>(in.gcount()) != magic.size() || magic != LOG_MAGIC) {
        throw std::runtime_error("invalid or missing fact log header");
    }

    uint32_t version = 0;
    in.read(reinterpret_cast<char *>(&version), sizeof(version));

    if (!in || version != LOG_FORMAT_VERSION) {
        throw std::runtime_error("unsupported fact log format version");
    }

    return true;
}

} // namespace

AssertionLog::AssertionLog(std::filesystem::path path) : path_(std::move(path)) {}

void AssertionLog::append(const Assertion &assertion) {
    std::filesystem::create_directories(path_.parent_path());

    bool file_is_new = !std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0;

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open fact log for append");
    }

    if (file_is_new) {
        write_header(out);
    }

    uint32_t record_size = ASSERTION_RECORD_SIZE;
    uint32_t crc = crc32(&assertion, sizeof(assertion));

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, reinterpret_cast<const char *>(&assertion), sizeof(assertion));
    write_or_throw(out, reinterpret_cast<const char *>(&crc), sizeof(crc));
}

std::vector<Assertion> AssertionLog::read_all() const {
    std::vector<Assertion> assertions;

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return assertions;
    }

    if (!read_and_validate_header(in)) {
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

        uint32_t stored_crc = 0;
        in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));

        if (!in) {
            break; // ignore incomplete trailing record for now
        }

        if (crc32(&assertion, sizeof(assertion)) != stored_crc) {
            throw std::runtime_error("fact log checksum mismatch");
        }

        assertions.push_back(assertion);
    }

    return assertions;
}

} // namespace knk