#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "kernel/assertion_log.hpp"

using namespace knk;

namespace {

void assertion_log_appends_and_reads_assertions() {
    auto path = std::filesystem::temp_directory_path() / "kernel_assertion_log_test.log";
    std::filesystem::remove(path);

    AssertionLog log(path);

    Assertion a{.id = 1,
                .subject = 1,
                .predicate = 10,
                .object = 100,
                .valid_from = 1672531200,
                .valid_to = OPEN_ENDED,
                .observed_at = 1719878400,
                .confidence = 0.95,
                .status = AssertionStatus::Active};

    log.append(a);

    auto assertions = log.read_all();

    assert(assertions.size() == 1);
    assert(assertions[0].id == 1);
    assert(assertions[0].subject == 1);
    assert(assertions[0].predicate == 10);
    assert(assertions[0].object == 100);
    assert(assertions[0].confidence == 0.95);
    assert(assertions[0].status == AssertionStatus::Active);

    std::filesystem::remove(path);
}

void assertion_log_returns_empty_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_missing_assertion_log.log";
    std::filesystem::remove(path);

    AssertionLog log(path);

    auto assertions = log.read_all();

    assert(assertions.empty());
}

void assertion_log_rejects_invalid_record_size() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        uint32_t bad_record_size = static_cast<uint32_t>(sizeof(Assertion)) + 1;
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    AssertionLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void assertion_log_rejects_missing_or_invalid_header() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_header_log.log";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("XXXX", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }

    AssertionLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void assertion_log_recovers_partial_header_as_empty_log() {
    auto path = std::filesystem::temp_directory_path() / "kernel_partial_header_log.log";
    std::filesystem::remove(path);

    {
        // Fewer than the full 8-byte header, simulating a crash mid-write on the very
        // first-ever append to a brand-new file -- by construction, zero records could
        // have been durably completed yet.
        std::ofstream out(path, std::ios::binary);
        out.write("KNK", 3);
    }

    AssertionLog log(path);

    auto assertions = log.read_all();

    assert(assertions.empty());

    std::filesystem::remove(path);
}

void assertion_log_recovers_tail_checksum_mismatch_as_torn_write() {
    auto path = std::filesystem::temp_directory_path() / "kernel_tail_checksum_mismatch_log.log";
    std::filesystem::remove(path);

    AssertionLog log(path);

    Assertion a{.id = 1,
                .subject = 1,
                .predicate = 10,
                .object = 100,
                .valid_from = 1672531200,
                .valid_to = OPEN_ENDED,
                .observed_at = 1719878400,
                .confidence = 0.95,
                .status = AssertionStatus::Active};

    log.append(a);

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

    auto assertions = log.read_all();

    assert(assertions.empty());

    std::filesystem::remove(path);
}

void assertion_log_rejects_checksum_mismatch_when_followed_by_more_data() {
    auto path = std::filesystem::temp_directory_path() / "kernel_mid_file_checksum_mismatch_log.log";
    std::filesystem::remove(path);

    AssertionLog log(path);

    Assertion a{.id = 1,
                .subject = 1,
                .predicate = 10,
                .object = 100,
                .valid_from = 1672531200,
                .valid_to = OPEN_ENDED,
                .observed_at = 1719878400,
                .confidence = 0.95,
                .status = AssertionStatus::Active};
    Assertion b{.id = 2,
                .subject = 2,
                .predicate = 10,
                .object = 200,
                .valid_from = 1672531200,
                .valid_to = OPEN_ENDED,
                .observed_at = 1719878400,
                .confidence = 0.95,
                .status = AssertionStatus::Active};

    log.append(a);

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

    log.append(b);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void assertion_log_ignores_incomplete_trailing_record() {
    auto path = std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_log.log";
    std::filesystem::remove(path);

    AssertionLog log(path);

    Assertion a{.id = 1,
                .subject = 1,
                .predicate = 10,
                .object = 100,
                .valid_from = 1672531200,
                .valid_to = OPEN_ENDED,
                .observed_at = 1719878400,
                .confidence = 0.95,
                .status = AssertionStatus::Active};

    log.append(a);

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        uint32_t record_size = static_cast<uint32_t>(sizeof(Assertion));
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(sizeof(Assertion) / 2, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto assertions = log.read_all();

    assert(assertions.size() == 1);
    assert(assertions[0].id == 1);

    std::filesystem::remove(path);
}

} // namespace

int main() {
    assertion_log_appends_and_reads_assertions();
    assertion_log_returns_empty_when_missing();
    assertion_log_rejects_invalid_record_size();
    assertion_log_rejects_missing_or_invalid_header();
    assertion_log_recovers_partial_header_as_empty_log();
    assertion_log_recovers_tail_checksum_mismatch_as_torn_write();
    assertion_log_rejects_checksum_mismatch_when_followed_by_more_data();
    assertion_log_ignores_incomplete_trailing_record();

    std::cout << "All assertion_log tests passed.\n";
}