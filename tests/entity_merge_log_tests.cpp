#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "kernel/entity_merge_log.hpp"

using namespace knk;

namespace {

void entity_merge_log_appends_and_reads_records() {
    auto path = std::filesystem::temp_directory_path() / "kernel_entity_merge_log_test.log";
    std::filesystem::remove(path);

    EntityMergeLog log(path);

    log.append(EntityMergeRecord{2, 1, 1672531200});
    log.append(EntityMergeRecord{3, 1, 1719792000});

    auto records = log.read_all();

    assert(records.size() == 2);
    assert(records[0].absorbed == 2);
    assert(records[0].surviving == 1);
    assert(records[0].merged_at == 1672531200);
    assert(records[1].absorbed == 3);
    assert(records[1].surviving == 1);
    assert(records[1].merged_at == 1719792000);

    std::filesystem::remove(path);
}

void entity_merge_log_returns_empty_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_missing_entity_merge_log.log";
    std::filesystem::remove(path);

    EntityMergeLog log(path);

    auto records = log.read_all();

    assert(records.empty());
}

void entity_merge_log_rejects_invalid_record_size() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_entity_merge_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        uint32_t bad_record_size = static_cast<uint32_t>(sizeof(EntityMergeRecord)) + 1;
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    EntityMergeLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void entity_merge_log_rejects_missing_or_invalid_header() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_header_entity_merge_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("XXXX", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }

    EntityMergeLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void entity_merge_log_recovers_partial_header_as_empty_log() {
    auto path = std::filesystem::temp_directory_path() / "kernel_partial_header_entity_merge_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK", 3);
    }

    EntityMergeLog log(path);

    auto records = log.read_all();

    assert(records.empty());

    std::filesystem::remove(path);
}

void entity_merge_log_recovers_tail_checksum_mismatch_as_torn_write() {
    auto path = std::filesystem::temp_directory_path() / "kernel_tail_checksum_mismatch_entity_merge_log.log";
    std::filesystem::remove(path);

    EntityMergeLog log(path);

    log.append(EntityMergeRecord{2, 1, 1672531200});

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

void entity_merge_log_rejects_checksum_mismatch_when_followed_by_more_data() {
    auto path = std::filesystem::temp_directory_path() / "kernel_mid_file_checksum_mismatch_entity_merge_log.log";
    std::filesystem::remove(path);

    EntityMergeLog log(path);

    log.append(EntityMergeRecord{2, 1, 1672531200});

    {
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    log.append(EntityMergeRecord{3, 1, 1719792000});

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void entity_merge_log_ignores_incomplete_trailing_record() {
    auto path = std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_entity_merge_log.log";
    std::filesystem::remove(path);

    EntityMergeLog log(path);

    log.append(EntityMergeRecord{2, 1, 1672531200});

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        uint32_t record_size = static_cast<uint32_t>(sizeof(EntityMergeRecord));
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(sizeof(EntityMergeRecord) / 2, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].absorbed == 2);
    assert(records[0].surviving == 1);

    std::filesystem::remove(path);
}

} // namespace

int main() {
    entity_merge_log_appends_and_reads_records();
    entity_merge_log_returns_empty_when_missing();
    entity_merge_log_rejects_invalid_record_size();
    entity_merge_log_rejects_missing_or_invalid_header();
    entity_merge_log_recovers_partial_header_as_empty_log();
    entity_merge_log_recovers_tail_checksum_mismatch_as_torn_write();
    entity_merge_log_rejects_checksum_mismatch_when_followed_by_more_data();
    entity_merge_log_ignores_incomplete_trailing_record();

    std::cout << "All entity_merge_log tests passed.\n";
}
