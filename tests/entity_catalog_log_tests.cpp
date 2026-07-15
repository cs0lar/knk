#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "kernel/checksum.hpp"
#include "kernel/entity_catalog_log.hpp"

using namespace knk;

namespace {

void entity_catalog_log_appends_and_reads_records() {
    auto path = std::filesystem::temp_directory_path() / "kernel_entity_catalog_log_test.idx";
    std::filesystem::remove(path);

    EntityCatalogLog log(path);

    log.append(EntityCatalogRecord{1, Value::of_text("Alice")});
    log.append(EntityCatalogRecord{2, Value::of_int64(42)});
    log.append(EntityCatalogRecord{3, Value::of_double(3.5)});
    log.append(EntityCatalogRecord{4, Value::of_bool(true)});
    log.append(EntityCatalogRecord{5, Value::of_timestamp(1719792000)});

    auto records = log.read_all();

    assert(records.size() == 5);
    assert(records[0].id == 1);
    assert(records[0].value == Value::of_text("Alice"));
    assert(records[1].id == 2);
    assert(records[1].value == Value::of_int64(42));
    assert(records[2].id == 3);
    assert(records[2].value == Value::of_double(3.5));
    assert(records[3].id == 4);
    assert(records[3].value == Value::of_bool(true));
    assert(records[4].id == 5);
    assert(records[4].value == Value::of_timestamp(1719792000));

    std::filesystem::remove(path);
}

void entity_catalog_log_returns_empty_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_missing_entity_catalog_log.idx";
    std::filesystem::remove(path);

    EntityCatalogLog log(path);

    auto records = log.read_all();

    assert(records.empty());
}

void entity_catalog_log_rejects_invalid_record_size() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_entity_catalog_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        uint32_t bad_record_size = 1; // below the id(8)+kind(1) minimum
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    EntityCatalogLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void entity_catalog_log_rejects_missing_or_invalid_header() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_header_entity_catalog_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("XXXX", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }

    EntityCatalogLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void entity_catalog_log_recovers_partial_header_as_empty_log() {
    auto path = std::filesystem::temp_directory_path() / "kernel_partial_header_entity_catalog_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK", 3);
    }

    EntityCatalogLog log(path);

    auto records = log.read_all();

    assert(records.empty());

    std::filesystem::remove(path);
}

void entity_catalog_log_recovers_tail_checksum_mismatch_as_torn_write() {
    auto path = std::filesystem::temp_directory_path() / "kernel_tail_checksum_mismatch_entity_catalog_log.idx";
    std::filesystem::remove(path);

    EntityCatalogLog log(path);

    log.append(EntityCatalogRecord{1, Value::of_bool(true)});

    {
        // Flip a byte inside the record payload, which sits right after the 8-byte header and the
        // 4-byte record-size prefix. Nothing follows this record, so it is indistinguishable from
        // a crash mid-append and should be silently dropped.
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    auto records = log.read_all();

    assert(records.empty());

    std::filesystem::remove(path);
}

void entity_catalog_log_rejects_checksum_mismatch_when_followed_by_more_data() {
    auto path = std::filesystem::temp_directory_path() / "kernel_mid_file_checksum_mismatch_entity_catalog_log.idx";
    std::filesystem::remove(path);

    EntityCatalogLog log(path);

    log.append(EntityCatalogRecord{1, Value::of_bool(true)});

    {
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    log.append(EntityCatalogRecord{2, Value::of_bool(false)});

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void entity_catalog_log_ignores_incomplete_trailing_record() {
    auto path = std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_entity_catalog_log.idx";
    std::filesystem::remove(path);

    EntityCatalogLog log(path);

    log.append(EntityCatalogRecord{1, Value::of_bool(true)});

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        uint32_t record_size = 10; // id(8) + kind(1) + bool payload(1)
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(5, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].id == 1);

    std::filesystem::remove(path);
}

void entity_catalog_log_rejects_malformed_text_length() {
    auto path = std::filesystem::temp_directory_path() / "kernel_malformed_text_length_entity_catalog_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));

        // A well-formed frame (correct record_size and crc) whose internal text-length prefix
        // doesn't match the number of bytes actually present -- a corruption category that can
        // only exist for variable-length payloads, unlike the other logs' fixed-size records.
        std::vector<char> buffer;
        EntityId id = 1;
        buffer.insert(buffer.end(), reinterpret_cast<char *>(&id), reinterpret_cast<char *>(&id) + sizeof(id));
        uint8_t kind = static_cast<uint8_t>(ValueKind::Text);
        buffer.push_back(static_cast<char>(kind));
        uint32_t length = 100; // does not match the 3 bytes actually present below
        buffer.insert(buffer.end(), reinterpret_cast<char *>(&length),
                      reinterpret_cast<char *>(&length) + sizeof(length));
        buffer.insert(buffer.end(), {'a', 'b', 'c'});

        uint32_t record_size = static_cast<uint32_t>(buffer.size());
        uint32_t crc = crc32(buffer.data(), buffer.size());

        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        out.write(reinterpret_cast<const char *>(&crc), sizeof(crc));
    }

    EntityCatalogLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

} // namespace

int main() {
    entity_catalog_log_appends_and_reads_records();
    entity_catalog_log_returns_empty_when_missing();
    entity_catalog_log_rejects_invalid_record_size();
    entity_catalog_log_rejects_missing_or_invalid_header();
    entity_catalog_log_recovers_partial_header_as_empty_log();
    entity_catalog_log_recovers_tail_checksum_mismatch_as_torn_write();
    entity_catalog_log_rejects_checksum_mismatch_when_followed_by_more_data();
    entity_catalog_log_ignores_incomplete_trailing_record();
    entity_catalog_log_rejects_malformed_text_length();

    std::cout << "All entity_catalog_log tests passed.\n";
}
