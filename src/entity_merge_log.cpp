#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"
#include "kernel/entity_merge_log.hpp"

namespace knk {

namespace {

constexpr uint32_t ENTITY_MERGE_RECORD_SIZE = sizeof(EntityMergeRecord);
constexpr std::array<char, 4> LOG_MAGIC{'K', 'N', 'K', '1'};
constexpr uint32_t LOG_FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = LOG_MAGIC.size() + sizeof(uint32_t);

void write_or_throw(std::ostream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write entity merge log");
    }
}

void write_header(std::ostream &out) {
    write_or_throw(out, LOG_MAGIC.data(), static_cast<std::streamsize>(LOG_MAGIC.size()));

    uint32_t version = LOG_FORMAT_VERSION;
    write_or_throw(out, reinterpret_cast<const char *>(&version), sizeof(version));
}

void write_record(std::ostream &out, const EntityMergeRecord &record) {
    uint32_t record_size = ENTITY_MERGE_RECORD_SIZE;
    uint32_t crc = crc32(&record, sizeof(record));

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, reinterpret_cast<const char *>(&record), sizeof(record));
    write_or_throw(out, reinterpret_cast<const char *>(&crc), sizeof(crc));
}

// See ObservedTimeIndexLog::read_and_validate_header (src/observed_time_index_log.cpp) for the
// reasoning behind torn-header tolerance vs. a full-size-but-wrong header always throwing.
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
        throw std::runtime_error("invalid or missing entity merge log header");
    }

    uint32_t version = 0;
    std::memcpy(&version, header.data() + 4, sizeof(version));

    if (version != LOG_FORMAT_VERSION) {
        throw std::runtime_error("unsupported entity merge log format version");
    }

    return true;
}

} // namespace

EntityMergeLog::EntityMergeLog(std::filesystem::path path) : path_(std::move(path)) {}

void EntityMergeLog::append(const EntityMergeRecord &record) {
    std::filesystem::create_directories(path_.parent_path());

    bool file_is_new = !std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0;

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open entity merge log for append");
    }

    if (file_is_new) {
        write_header(out);
    }

    write_record(out, record);

    out.close();
    fsync_file(path_);
}

std::vector<EntityMergeRecord> EntityMergeLog::read_all() const {
    std::vector<EntityMergeRecord> records;

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

        if (record_size != ENTITY_MERGE_RECORD_SIZE) {
            throw std::runtime_error("invalid entity merge log record size");
        }

        EntityMergeRecord record{};
        in.read(reinterpret_cast<char *>(&record), sizeof(record));

        if (!in) {
            break; // incomplete trailing record, same tail tolerance as the other logs
        }

        uint32_t stored_crc = 0;
        in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));

        if (!in) {
            break; // incomplete trailing crc
        }

        if (crc32(&record, sizeof(record)) != stored_crc) {
            if (in.peek() == std::char_traits<char>::eof()) {
                break;
            }

            throw std::runtime_error("entity merge log checksum mismatch");
        }

        records.push_back(record);
    }

    return records;
}

} // namespace knk
