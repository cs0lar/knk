#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "kernel/subject_index_log.hpp"

using namespace knk;

namespace {

void subject_index_log_appends_and_reads_records() {
    auto path = std::filesystem::temp_directory_path() / "kernel_subject_index_log_test.idx";
    std::filesystem::remove(path);

    SubjectIndexLog log(path);

    log.append(SubjectIndexRecord{1, 1});
    log.append(SubjectIndexRecord{1, 2});
    log.append(SubjectIndexRecord{2, 3});

    auto records = log.read_all();

    assert(records.size() == 3);
    assert(records[0].subject == 1);
    assert(records[0].assertion_id == 1);
    assert(records[1].subject == 1);
    assert(records[1].assertion_id == 2);
    assert(records[2].subject == 2);
    assert(records[2].assertion_id == 3);

    std::filesystem::remove(path);
}

void subject_index_log_returns_empty_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_missing_subject_index_log.idx";
    std::filesystem::remove(path);

    SubjectIndexLog log(path);

    auto records = log.read_all();

    assert(records.empty());
}

void subject_index_log_rejects_invalid_record_size() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_subject_index_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        uint32_t bad_record_size = static_cast<uint32_t>(sizeof(SubjectIndexRecord)) + 1;
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    SubjectIndexLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void subject_index_log_rejects_missing_or_invalid_header() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_header_subject_index_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("XXXX", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }

    SubjectIndexLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void subject_index_log_recovers_partial_header_as_empty_log() {
    auto path = std::filesystem::temp_directory_path() / "kernel_partial_header_subject_index_log.idx";
    std::filesystem::remove(path);

    {
        // Fewer than the full 8-byte header, simulating a crash mid-write on the very
        // first-ever append to a brand-new file -- by construction, zero records could
        // have been durably completed yet.
        std::ofstream out(path, std::ios::binary);
        out.write("KNK", 3);
    }

    SubjectIndexLog log(path);

    auto records = log.read_all();

    assert(records.empty());

    std::filesystem::remove(path);
}

void subject_index_log_recovers_tail_checksum_mismatch_as_torn_write() {
    auto path = std::filesystem::temp_directory_path() / "kernel_tail_checksum_mismatch_subject_index_log.idx";
    std::filesystem::remove(path);

    SubjectIndexLog log(path);

    log.append(SubjectIndexRecord{1, 1});

    {
        // Flip a byte inside the record payload, which sits right after the 8-byte
        // header and the 4-byte record-size prefix. Nothing follows this record, so it
        // is indistinguishable from a crash mid-append and should be silently dropped.
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

void subject_index_log_rejects_checksum_mismatch_when_followed_by_more_data() {
    auto path = std::filesystem::temp_directory_path() / "kernel_mid_file_checksum_mismatch_subject_index_log.idx";
    std::filesystem::remove(path);

    SubjectIndexLog log(path);

    log.append(SubjectIndexRecord{1, 1});

    {
        // Flip a byte inside the first record's payload before a second, valid record is
        // appended after it -- data can't validly follow a torn write, so this is
        // unambiguous corruption, not a crash artifact, and must still throw.
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    log.append(SubjectIndexRecord{2, 2});

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void subject_index_log_ignores_incomplete_trailing_record() {
    auto path = std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_subject_index_log.idx";
    std::filesystem::remove(path);

    SubjectIndexLog log(path);

    log.append(SubjectIndexRecord{1, 1});

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        uint32_t record_size = static_cast<uint32_t>(sizeof(SubjectIndexRecord));
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(sizeof(SubjectIndexRecord) / 2, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].assertion_id == 1);

    std::filesystem::remove(path);
}

void subject_index_log_overwrite_all_replaces_prior_contents() {
    auto path = std::filesystem::temp_directory_path() / "kernel_overwrite_subject_index_log.idx";
    std::filesystem::remove(path);

    SubjectIndexLog log(path);

    log.append(SubjectIndexRecord{1, 1});
    log.append(SubjectIndexRecord{2, 2});

    log.overwrite_all({SubjectIndexRecord{3, 4}});

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].subject == 3);
    assert(records[0].assertion_id == 4);

    std::filesystem::remove(path);
}

} // namespace

int main() {
    subject_index_log_appends_and_reads_records();
    subject_index_log_returns_empty_when_missing();
    subject_index_log_rejects_invalid_record_size();
    subject_index_log_rejects_missing_or_invalid_header();
    subject_index_log_recovers_partial_header_as_empty_log();
    subject_index_log_recovers_tail_checksum_mismatch_as_torn_write();
    subject_index_log_rejects_checksum_mismatch_when_followed_by_more_data();
    subject_index_log_ignores_incomplete_trailing_record();
    subject_index_log_overwrite_all_replaces_prior_contents();

    std::cout << "All subject_index_log tests passed.\n";
}
