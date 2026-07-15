#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "kernel/checksum.hpp"
#include "kernel/predicate_catalog_log.hpp"

using namespace knk;

namespace {

void predicate_catalog_log_appends_and_reads_records() {
    auto path = std::filesystem::temp_directory_path() / "kernel_predicate_catalog_log_test.idx";
    std::filesystem::remove(path);

    PredicateCatalogLog log(path);

    log.append(PredicateCatalogRecord{1, "works_at"});
    log.append(PredicateCatalogRecord{2, "lives_in"});

    auto records = log.read_all();

    assert(records.size() == 2);
    assert(records[0].id == 1);
    assert(records[0].name == "works_at");
    assert(records[1].id == 2);
    assert(records[1].name == "lives_in");

    std::filesystem::remove(path);
}

void predicate_catalog_log_returns_empty_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_missing_predicate_catalog_log.idx";
    std::filesystem::remove(path);

    PredicateCatalogLog log(path);

    auto records = log.read_all();

    assert(records.empty());
}

void predicate_catalog_log_rejects_invalid_record_size() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_predicate_catalog_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        uint32_t bad_record_size = 1; // below the id(8)+length(4) minimum
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    PredicateCatalogLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void predicate_catalog_log_rejects_missing_or_invalid_header() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_header_predicate_catalog_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("XXXX", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }

    PredicateCatalogLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void predicate_catalog_log_recovers_partial_header_as_empty_log() {
    auto path = std::filesystem::temp_directory_path() / "kernel_partial_header_predicate_catalog_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK", 3);
    }

    PredicateCatalogLog log(path);

    auto records = log.read_all();

    assert(records.empty());

    std::filesystem::remove(path);
}

void predicate_catalog_log_recovers_tail_checksum_mismatch_as_torn_write() {
    auto path = std::filesystem::temp_directory_path() / "kernel_tail_checksum_mismatch_predicate_catalog_log.idx";
    std::filesystem::remove(path);

    PredicateCatalogLog log(path);

    log.append(PredicateCatalogRecord{1, "works_at"});

    {
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

void predicate_catalog_log_rejects_checksum_mismatch_when_followed_by_more_data() {
    auto path = std::filesystem::temp_directory_path() / "kernel_mid_file_checksum_mismatch_predicate_catalog_log.idx";
    std::filesystem::remove(path);

    PredicateCatalogLog log(path);

    log.append(PredicateCatalogRecord{1, "works_at"});

    {
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    log.append(PredicateCatalogRecord{2, "lives_in"});

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void predicate_catalog_log_ignores_incomplete_trailing_record() {
    auto path = std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_predicate_catalog_log.idx";
    std::filesystem::remove(path);

    PredicateCatalogLog log(path);

    log.append(PredicateCatalogRecord{1, "works_at"});

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        uint32_t record_size = 12; // id(8) + length(4), pretending a non-empty name follows
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(6, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].id == 1);

    std::filesystem::remove(path);
}

void predicate_catalog_log_rejects_malformed_name_length() {
    auto path = std::filesystem::temp_directory_path() / "kernel_malformed_name_length_predicate_catalog_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));

        std::vector<char> buffer;
        PredicateId id = 1;
        buffer.insert(buffer.end(), reinterpret_cast<char *>(&id), reinterpret_cast<char *>(&id) + sizeof(id));
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

    PredicateCatalogLog log(path);

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
    predicate_catalog_log_appends_and_reads_records();
    predicate_catalog_log_returns_empty_when_missing();
    predicate_catalog_log_rejects_invalid_record_size();
    predicate_catalog_log_rejects_missing_or_invalid_header();
    predicate_catalog_log_recovers_partial_header_as_empty_log();
    predicate_catalog_log_recovers_tail_checksum_mismatch_as_torn_write();
    predicate_catalog_log_rejects_checksum_mismatch_when_followed_by_more_data();
    predicate_catalog_log_ignores_incomplete_trailing_record();
    predicate_catalog_log_rejects_malformed_name_length();

    std::cout << "All predicate_catalog_log tests passed.\n";
}
