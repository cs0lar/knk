#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <stdexcept>
#include <vector>

#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"
#include "kernel/ids.hpp"
#include "kernel/subject_index_log.hpp"

namespace knk {

namespace {

constexpr uint32_t SUBJECT_INDEX_RECORD_SIZE = sizeof(SubjectIndexRecord);
constexpr std::array<char, 4> LOG_MAGIC{'K', 'N', 'K', '1'};
constexpr uint32_t LOG_FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = LOG_MAGIC.size() + sizeof(uint32_t);

void write_or_throw(std::ostream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write subject index log");
    }
}

void write_header(std::ostream &out) {
    write_or_throw(out, LOG_MAGIC.data(), static_cast<std::streamsize>(LOG_MAGIC.size()));

    uint32_t version = LOG_FORMAT_VERSION;
    write_or_throw(out, reinterpret_cast<const char *>(&version), sizeof(version));
}

void write_record(std::ostream &out, const SubjectIndexRecord &record) {
    uint32_t record_size = SUBJECT_INDEX_RECORD_SIZE;
    uint32_t crc = crc32(&record, sizeof(record));

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, reinterpret_cast<const char *>(&record), sizeof(record));
    write_or_throw(out, reinterpret_cast<const char *>(&crc), sizeof(crc));
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
        throw std::runtime_error("invalid or missing subject index log header");
    }

    uint32_t version = 0;
    std::memcpy(&version, header.data() + 4, sizeof(version));

    if (version != LOG_FORMAT_VERSION) {
        throw std::runtime_error("unsupported subject index log format version");
    }

    return true;
}

} // namespace

SubjectIndexLog::SubjectIndexLog(std::filesystem::path path) : path_(std::move(path)) {}

void SubjectIndexLog::append(const SubjectIndexRecord &record) {
    append_batch(std::span<const SubjectIndexRecord>(&record, 1));
}

void SubjectIndexLog::append_batch(std::span<const SubjectIndexRecord> records) {
    if (records.empty()) {
        return;
    }

    std::filesystem::create_directories(path_.parent_path());

    bool file_is_new = !std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0;

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open subect index log for append");
    }

    if (file_is_new) {
        write_header(out);
    }

    for (const auto &record : records) {
        write_record(out, record);
    }

    out.close();
    fsync_file(path_);
}

void SubjectIndexLog::overwrite_all(const std::vector<SubjectIndexRecord> &records) {
    write_file_atomically(path_, [&](std::ostream &out) {
        write_header(out);

        for (const auto &record : records) {
            write_record(out, record);
        }
    });
}

std::vector<SubjectIndexRecord> SubjectIndexLog::read_all() const {
    std::vector<SubjectIndexRecord> records;

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return records;
    }

    if (!read_and_validate_header(in)) {
        return records;
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

        if (record_size != SUBJECT_INDEX_RECORD_SIZE) {
            throw std::runtime_error("invalid subject index log record size");
        }

        SubjectIndexRecord record{};
        in.read(reinterpret_cast<char *>(&record), sizeof(record));

        if (!in) {
            break; // ignoring incomplete trailing record for now
        }

        uint32_t stored_crc = 0;
        in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));

        if (!in) {
            break; // ignoring incomplete trailing record for now
        }

        if (crc32(&record, sizeof(record)) != stored_crc) {
            // A torn write can only ever leave garbage at the true end of the file, so a checksum
            // mismatch with nothing after it is treated the same as an incomplete trailing record.
            // A checksum mismatch with valid-length data following it, however, cannot be a crash
            // artifact -- that is unambiguous corruption.
            if (in.peek() == std::char_traits<char>::eof()) {
                break;
            }

            throw std::runtime_error("subject index log checksum mismatch");
        }

        records.push_back(record);
    }

    return records;
}

} // namespace knk
