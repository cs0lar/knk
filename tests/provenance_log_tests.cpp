#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "kernel/checksum.hpp"
#include "kernel/provenance_log.hpp"

using namespace knk;

namespace {

void provenance_log_appends_and_reads_records() {
    auto path = std::filesystem::temp_directory_path() / "kernel_provenance_log_test.log";
    std::filesystem::remove(path);

    ProvenanceLog log(path);

    log.append(ProvenanceRecord{1, 500, 1719792000, "manual_entry"});
    log.append(ProvenanceRecord{2, 501, 1719878400, ""});

    auto records = log.read_all();

    assert(records.size() == 2);
    assert(records[0].assertion_id == 1);
    assert(records[0].source == 500);
    assert(records[0].recorded_at == 1719792000);
    assert(records[0].method == "manual_entry");
    assert(records[1].assertion_id == 2);
    assert(records[1].source == 501);
    assert(records[1].recorded_at == 1719878400);
    assert(records[1].method.empty());

    std::filesystem::remove(path);
}

void provenance_log_append_batch_writes_every_record_in_order() {
    auto path = std::filesystem::temp_directory_path() / "kernel_provenance_log_append_batch_test.log";
    std::filesystem::remove(path);

    ProvenanceLog log(path);

    // Variable-length method strings, empty one included, since this log frames a length-prefixed
    // payload rather than a fixed-size struct like the index logs.
    std::vector<ProvenanceRecord> batch{ProvenanceRecord{1, 500, 1719792000, "manual_entry"},
                                        ProvenanceRecord{2, 501, 1719878400, ""},
                                        ProvenanceRecord{3, 502, 1719961200, "restated_from_legacy_field"}};
    log.append_batch(batch);

    auto records = log.read_all();

    assert(records.size() == 3);
    assert(records[0].assertion_id == 1 && records[0].method == "manual_entry");
    assert(records[1].assertion_id == 2 && records[1].method.empty());
    assert(records[2].assertion_id == 3 && records[2].method == "restated_from_legacy_field");

    // A batch writes the same header-then-frames layout a sequence of appends would, so a later
    // single append continues the same file rather than starting a second header.
    log.append(ProvenanceRecord{4, 503, 1720450412, "manual_entry"});
    assert(log.read_all().size() == 4);

    std::filesystem::remove(path);
}

void provenance_log_append_batch_is_a_no_op_for_an_empty_batch() {
    auto path = std::filesystem::temp_directory_path() / "kernel_provenance_log_append_batch_empty_test.log";
    std::filesystem::remove(path);

    ProvenanceLog log(path);

    log.append_batch({});

    assert(!std::filesystem::exists(path));
    assert(log.read_all().empty());

    std::filesystem::remove(path);
}

void provenance_log_returns_empty_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_missing_provenance_log.log";
    std::filesystem::remove(path);

    ProvenanceLog log(path);

    auto records = log.read_all();

    assert(records.empty());
}

void provenance_log_rejects_invalid_record_size() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_provenance_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        uint32_t bad_record_size = 1; // below the id(8)+source(8)+recorded_at(8)+length(4) minimum
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    ProvenanceLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void provenance_log_rejects_missing_or_invalid_header() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_header_provenance_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("XXXX", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }

    ProvenanceLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void provenance_log_recovers_partial_header_as_empty_log() {
    auto path = std::filesystem::temp_directory_path() / "kernel_partial_header_provenance_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK", 3);
    }

    ProvenanceLog log(path);

    auto records = log.read_all();

    assert(records.empty());

    std::filesystem::remove(path);
}

void provenance_log_recovers_tail_checksum_mismatch_as_torn_write() {
    auto path = std::filesystem::temp_directory_path() / "kernel_tail_checksum_mismatch_provenance_log.log";
    std::filesystem::remove(path);

    ProvenanceLog log(path);

    log.append(ProvenanceRecord{1, 500, 1719792000, "manual_entry"});

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

void provenance_log_rejects_checksum_mismatch_when_followed_by_more_data() {
    auto path = std::filesystem::temp_directory_path() / "kernel_mid_file_checksum_mismatch_provenance_log.log";
    std::filesystem::remove(path);

    ProvenanceLog log(path);

    log.append(ProvenanceRecord{1, 500, 1719792000, "manual_entry"});

    {
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    log.append(ProvenanceRecord{2, 501, 1719878400, "import"});

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void provenance_log_ignores_incomplete_trailing_record() {
    auto path = std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_provenance_log.log";
    std::filesystem::remove(path);

    ProvenanceLog log(path);

    log.append(ProvenanceRecord{1, 500, 1719792000, "manual_entry"});

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        uint32_t record_size = 32; // id(8)+source(8)+recorded_at(8)+length(4), pretending a method follows
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(10, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].assertion_id == 1);

    std::filesystem::remove(path);
}

void provenance_log_rejects_malformed_method_length() {
    auto path = std::filesystem::temp_directory_path() / "kernel_malformed_method_length_provenance_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));

        std::vector<char> buffer;
        AssertionId assertion_id = 1;
        buffer.insert(buffer.end(), reinterpret_cast<char *>(&assertion_id),
                      reinterpret_cast<char *>(&assertion_id) + sizeof(assertion_id));
        EntityId source = 500;
        buffer.insert(buffer.end(), reinterpret_cast<char *>(&source),
                      reinterpret_cast<char *>(&source) + sizeof(source));
        Timestamp recorded_at = 1719792000;
        buffer.insert(buffer.end(), reinterpret_cast<char *>(&recorded_at),
                      reinterpret_cast<char *>(&recorded_at) + sizeof(recorded_at));
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

    ProvenanceLog log(path);

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
    provenance_log_appends_and_reads_records();
    provenance_log_append_batch_writes_every_record_in_order();
    provenance_log_append_batch_is_a_no_op_for_an_empty_batch();
    provenance_log_returns_empty_when_missing();
    provenance_log_rejects_invalid_record_size();
    provenance_log_rejects_missing_or_invalid_header();
    provenance_log_recovers_partial_header_as_empty_log();
    provenance_log_recovers_tail_checksum_mismatch_as_torn_write();
    provenance_log_rejects_checksum_mismatch_when_followed_by_more_data();
    provenance_log_ignores_incomplete_trailing_record();
    provenance_log_rejects_malformed_method_length();

    std::cout << "All provenance_log tests passed.\n";
}
