#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <vector>

#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"
#include "kernel/predicate_catalog_log.hpp"

namespace knk {

namespace {

constexpr std::array<char, 4> LOG_MAGIC{'K', 'N', 'K', '1'};
constexpr uint32_t LOG_FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = LOG_MAGIC.size() + sizeof(uint32_t);

// id (8) + name length prefix (4) is the smallest a payload can ever be (an empty name).
constexpr size_t MIN_PAYLOAD_SIZE = sizeof(PredicateId) + sizeof(uint32_t);

void write_or_throw(std::ostream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write predicate catalog log");
    }
}

void write_header(std::ostream &out) {
    write_or_throw(out, LOG_MAGIC.data(), static_cast<std::streamsize>(LOG_MAGIC.size()));

    uint32_t version = LOG_FORMAT_VERSION;
    write_or_throw(out, reinterpret_cast<const char *>(&version), sizeof(version));
}

void append_bytes(std::vector<char> &buffer, const void *data, size_t size) {
    const char *bytes = reinterpret_cast<const char *>(data);
    buffer.insert(buffer.end(), bytes, bytes + size);
}

std::vector<char> serialize_record(const PredicateCatalogRecord &record) {
    std::vector<char> buffer;
    append_bytes(buffer, &record.id, sizeof(record.id));

    uint32_t length = static_cast<uint32_t>(record.name.size());
    append_bytes(buffer, &length, sizeof(length));
    append_bytes(buffer, record.name.data(), record.name.size());

    return buffer;
}

void write_record(std::ostream &out, const PredicateCatalogRecord &record) {
    auto buffer = serialize_record(record);

    uint32_t record_size = static_cast<uint32_t>(buffer.size());
    uint32_t crc = crc32(buffer.data(), buffer.size());

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, buffer.data(), static_cast<std::streamsize>(buffer.size()));
    write_or_throw(out, reinterpret_cast<const char *>(&crc), sizeof(crc));
}

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
        throw std::runtime_error("invalid or missing predicate catalog log header");
    }

    uint32_t version = 0;
    std::memcpy(&version, header.data() + 4, sizeof(version));

    if (version != LOG_FORMAT_VERSION) {
        throw std::runtime_error("unsupported predicate catalog log format version");
    }

    return true;
}

} // namespace

PredicateCatalogLog::PredicateCatalogLog(std::filesystem::path path) : path_(std::move(path)) {}

void PredicateCatalogLog::append(const PredicateCatalogRecord &record) {
    std::filesystem::create_directories(path_.parent_path());

    bool file_is_new = !std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0;

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open predicate catalog log for append");
    }

    if (file_is_new) {
        write_header(out);
    }

    write_record(out, record);

    out.close();
    fsync_file(path_);
}

std::vector<PredicateCatalogRecord> PredicateCatalogLog::read_all() const {
    std::vector<PredicateCatalogRecord> records;

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

        if (record_size < MIN_PAYLOAD_SIZE) {
            throw std::runtime_error("invalid predicate catalog log record size");
        }

        std::vector<char> buffer(record_size);
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));

        if (!in) {
            break; // incomplete trailing record
        }

        uint32_t stored_crc = 0;
        in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));

        if (!in) {
            break; // incomplete trailing crc
        }

        if (crc32(buffer.data(), buffer.size()) != stored_crc) {
            if (in.peek() == std::char_traits<char>::eof()) {
                break;
            }

            throw std::runtime_error("predicate catalog log checksum mismatch");
        }

        PredicateId id = 0;
        std::memcpy(&id, buffer.data(), sizeof(id));

        uint32_t length = 0;
        std::memcpy(&length, buffer.data() + sizeof(id), sizeof(length));

        if (buffer.size() - sizeof(id) - sizeof(length) != length) {
            throw std::runtime_error("malformed predicate catalog log name length");
        }

        std::string name(buffer.data() + sizeof(id) + sizeof(length), length);

        records.push_back(PredicateCatalogRecord{id, std::move(name)});
    }

    return records;
}

} // namespace knk
