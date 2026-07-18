#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <vector>

#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"
#include "kernel/provenance_log.hpp"

namespace knk {

namespace {

constexpr std::array<char, 4> LOG_MAGIC{'K', 'N', 'K', '1'};
constexpr uint32_t LOG_FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = LOG_MAGIC.size() + sizeof(uint32_t);

// assertion_id (8) + source (8) + recorded_at (8) + method length prefix (4) is the smallest a
// payload can ever be (an empty method string).
constexpr size_t MIN_PAYLOAD_SIZE = sizeof(AssertionId) + sizeof(EntityId) + sizeof(Timestamp) + sizeof(uint32_t);

void write_or_throw(std::ostream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write provenance log");
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

std::vector<char> serialize_record(const ProvenanceRecord &record) {
    std::vector<char> buffer;
    append_bytes(buffer, &record.assertion_id, sizeof(record.assertion_id));
    append_bytes(buffer, &record.source, sizeof(record.source));
    append_bytes(buffer, &record.recorded_at, sizeof(record.recorded_at));

    uint32_t length = static_cast<uint32_t>(record.method.size());
    append_bytes(buffer, &length, sizeof(length));
    append_bytes(buffer, record.method.data(), record.method.size());

    return buffer;
}

void write_record(std::ostream &out, const ProvenanceRecord &record) {
    auto buffer = serialize_record(record);

    uint32_t record_size = static_cast<uint32_t>(buffer.size());
    uint32_t crc = crc32(buffer.data(), buffer.size());

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, buffer.data(), static_cast<std::streamsize>(buffer.size()));
    write_or_throw(out, reinterpret_cast<const char *>(&crc), sizeof(crc));
}

// See SubjectIndexLog::read_and_validate_header (src/subject_index_log.cpp) for the reasoning
// behind torn-header tolerance vs. a full-size-but-wrong header always throwing.
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
        throw std::runtime_error("invalid or missing provenance log header");
    }

    uint32_t version = 0;
    std::memcpy(&version, header.data() + 4, sizeof(version));

    if (version != LOG_FORMAT_VERSION) {
        throw std::runtime_error("unsupported provenance log format version");
    }

    return true;
}

} // namespace

ProvenanceLog::ProvenanceLog(std::filesystem::path path) : path_(std::move(path)) {}

void ProvenanceLog::append(const ProvenanceRecord &record) {
    std::filesystem::create_directories(path_.parent_path());

    bool file_is_new = !std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0;

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open provenance log for append");
    }

    if (file_is_new) {
        write_header(out);
    }

    write_record(out, record);

    out.close();
    fsync_file(path_);
}

std::vector<ProvenanceRecord> ProvenanceLog::read_all() const {
    std::vector<ProvenanceRecord> records;

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
            throw std::runtime_error("invalid provenance log record size");
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

            throw std::runtime_error("provenance log checksum mismatch");
        }

        size_t offset = 0;

        AssertionId assertion_id = 0;
        std::memcpy(&assertion_id, buffer.data() + offset, sizeof(assertion_id));
        offset += sizeof(assertion_id);

        EntityId source = 0;
        std::memcpy(&source, buffer.data() + offset, sizeof(source));
        offset += sizeof(source);

        Timestamp recorded_at = 0;
        std::memcpy(&recorded_at, buffer.data() + offset, sizeof(recorded_at));
        offset += sizeof(recorded_at);

        uint32_t length = 0;
        std::memcpy(&length, buffer.data() + offset, sizeof(length));
        offset += sizeof(length);

        if (buffer.size() - offset != length) {
            throw std::runtime_error("malformed provenance log method length");
        }

        std::string method(buffer.data() + offset, length);

        records.push_back(ProvenanceRecord{assertion_id, source, recorded_at, std::move(method)});
    }

    return records;
}

} // namespace knk
