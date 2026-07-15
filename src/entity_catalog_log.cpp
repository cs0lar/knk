#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <vector>

#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"
#include "kernel/entity_catalog_log.hpp"

namespace knk {

namespace {

constexpr std::array<char, 4> LOG_MAGIC{'K', 'N', 'K', '1'};
constexpr uint32_t LOG_FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = LOG_MAGIC.size() + sizeof(uint32_t);

// id (8) + kind (1) is the smallest a payload can ever be (Bool still adds one more byte, but
// nothing adds less); anything under this can never be a valid frame, torn or otherwise.
constexpr size_t MIN_PAYLOAD_SIZE = sizeof(EntityId) + sizeof(uint8_t);

void write_or_throw(std::ostream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write entity catalog log");
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

std::vector<char> serialize_record(const EntityCatalogRecord &record) {
    std::vector<char> buffer;
    append_bytes(buffer, &record.id, sizeof(record.id));

    uint8_t kind = static_cast<uint8_t>(record.value.kind);
    append_bytes(buffer, &kind, sizeof(kind));

    switch (record.value.kind) {
    case ValueKind::Text: {
        uint32_t length = static_cast<uint32_t>(record.value.text.size());
        append_bytes(buffer, &length, sizeof(length));
        append_bytes(buffer, record.value.text.data(), record.value.text.size());
        break;
    }
    case ValueKind::Int64:
        append_bytes(buffer, &record.value.int64_value, sizeof(record.value.int64_value));
        break;
    case ValueKind::Double:
        append_bytes(buffer, &record.value.double_value, sizeof(record.value.double_value));
        break;
    case ValueKind::Bool: {
        uint8_t flag = record.value.bool_value ? 1 : 0;
        append_bytes(buffer, &flag, sizeof(flag));
        break;
    }
    case ValueKind::Timestamp:
        append_bytes(buffer, &record.value.timestamp_value, sizeof(record.value.timestamp_value));
        break;
    }

    return buffer;
}

// Parses a record's payload bytes (everything after the id) into a Value, throwing on any
// malformed encoding -- there is no tail tolerance for this, matching how other logs always throw
// on a mismatched record_size: once framing itself is inconsistent, there is no safe resync point.
Value parse_value(const char *data, size_t size) {
    uint8_t kind_byte = static_cast<uint8_t>(data[0]);
    const char *payload = data + 1;
    size_t payload_size = size - 1;

    switch (static_cast<ValueKind>(kind_byte)) {
    case ValueKind::Text: {
        if (payload_size < sizeof(uint32_t)) {
            throw std::runtime_error("malformed entity catalog log text value");
        }
        uint32_t length = 0;
        std::memcpy(&length, payload, sizeof(length));
        if (payload_size - sizeof(uint32_t) != length) {
            throw std::runtime_error("malformed entity catalog log text value length");
        }
        return Value::of_text(std::string(payload + sizeof(uint32_t), length));
    }
    case ValueKind::Int64: {
        if (payload_size != sizeof(int64_t)) {
            throw std::runtime_error("malformed entity catalog log int64 value");
        }
        int64_t v = 0;
        std::memcpy(&v, payload, sizeof(v));
        return Value::of_int64(v);
    }
    case ValueKind::Double: {
        if (payload_size != sizeof(double)) {
            throw std::runtime_error("malformed entity catalog log double value");
        }
        double v = 0.0;
        std::memcpy(&v, payload, sizeof(v));
        return Value::of_double(v);
    }
    case ValueKind::Bool: {
        if (payload_size != sizeof(uint8_t)) {
            throw std::runtime_error("malformed entity catalog log bool value");
        }
        return Value::of_bool(payload[0] != 0);
    }
    case ValueKind::Timestamp: {
        if (payload_size != sizeof(Timestamp)) {
            throw std::runtime_error("malformed entity catalog log timestamp value");
        }
        Timestamp v = 0;
        std::memcpy(&v, payload, sizeof(v));
        return Value::of_timestamp(v);
    }
    }

    throw std::runtime_error("unknown entity catalog log value kind");
}

void write_record(std::ostream &out, const EntityCatalogRecord &record) {
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
        throw std::runtime_error("invalid or missing entity catalog log header");
    }

    uint32_t version = 0;
    std::memcpy(&version, header.data() + 4, sizeof(version));

    if (version != LOG_FORMAT_VERSION) {
        throw std::runtime_error("unsupported entity catalog log format version");
    }

    return true;
}

} // namespace

EntityCatalogLog::EntityCatalogLog(std::filesystem::path path) : path_(std::move(path)) {}

void EntityCatalogLog::append(const EntityCatalogRecord &record) {
    std::filesystem::create_directories(path_.parent_path());

    bool file_is_new = !std::filesystem::exists(path_) || std::filesystem::file_size(path_) == 0;

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open entity catalog log for append");
    }

    if (file_is_new) {
        write_header(out);
    }

    write_record(out, record);

    out.close();
    fsync_file(path_);
}

std::vector<EntityCatalogRecord> EntityCatalogLog::read_all() const {
    std::vector<EntityCatalogRecord> records;

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
            throw std::runtime_error("invalid entity catalog log record size");
        }

        std::vector<char> buffer(record_size);
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));

        if (!in) {
            break; // incomplete trailing record, same tail tolerance as the other logs
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

            throw std::runtime_error("entity catalog log checksum mismatch");
        }

        EntityId id = 0;
        std::memcpy(&id, buffer.data(), sizeof(id));

        Value value = parse_value(buffer.data() + sizeof(id), buffer.size() - sizeof(id));

        records.push_back(EntityCatalogRecord{id, std::move(value)});
    }

    return records;
}

} // namespace knk
