#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "kernel/assertion_log.hpp"
#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"

namespace knk {

namespace {

constexpr uint32_t ASSERTION_RECORD_SIZE = sizeof(Assertion);
constexpr std::array<char, 4> LOG_MAGIC{'K', 'N', 'K', '1'};
constexpr uint32_t LOG_FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = LOG_MAGIC.size() + sizeof(uint32_t);

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

// Returns false if the file is empty, or has a torn header (fewer than HEADER_SIZE bytes) from a
// crash mid-write on the very first-ever append -- by construction no record frame can exist
// without a complete header preceding it, so either case unambiguously means zero durably
// completed records. Throws only when a full-size header has the wrong magic/version, which a
// torn write cannot produce -- that is a genuine format mismatch, not a crash artifact.
bool read_and_validate_header(std::ifstream &in) {
    std::array<char, HEADER_SIZE> header{};
    in.read(header.data(), static_cast<std::streamsize>(header.size()));

    auto got = static_cast<size_t>(in.gcount());
    if (got < HEADER_SIZE) {
        return false;
    }

    std::array<char, 4> magic{};
    std::copy(header.begin(), header.begin() + 4, magic.begin());

    if (magic != LOG_MAGIC) {
        throw std::runtime_error("invalid or missing fact log header");
    }

    uint32_t version = 0;
    std::memcpy(&version, header.data() + 4, sizeof(version));

    if (version != LOG_FORMAT_VERSION) {
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

    out.close();
    fsync_file(path_);
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
            // A torn write can only ever leave garbage at the true end of the file, so a checksum
            // mismatch with nothing after it is treated the same as an incomplete trailing record.
            // A checksum mismatch with valid-length data following it, however, cannot be a crash
            // artifact -- that is unambiguous corruption.
            if (in.peek() == std::char_traits<char>::eof()) {
                break;
            }

            throw std::runtime_error("fact log checksum mismatch");
        }

        assertions.push_back(assertion);
    }

    return assertions;
}

} // namespace knk
