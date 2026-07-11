#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "kernel/observed_time_index_log.hpp"

using namespace knk;

namespace {

void observed_time_index_log_appends_and_reads_records() {
    auto path = std::filesystem::temp_directory_path() / "kernel_observed_time_index_log_test.idx";
    std::filesystem::remove(path);

    ObservedTimeIndexLog log(path);

    log.append(ObservedTimeIndexRecord{1, 1672531200, 1});
    log.append(ObservedTimeIndexRecord{1, 1719792000, 2});
    log.append(ObservedTimeIndexRecord{2, 1704067200, 3});

    auto records = log.read_all();

    assert(records.size() == 3);
    assert(records[0].subject == 1);
    assert(records[0].observed_at == 1672531200);
    assert(records[0].assertion_id == 1);
    assert(records[1].subject == 1);
    assert(records[1].observed_at == 1719792000);
    assert(records[1].assertion_id == 2);
    assert(records[2].subject == 2);
    assert(records[2].observed_at == 1704067200);
    assert(records[2].assertion_id == 3);

    std::filesystem::remove(path);
}

void observed_time_index_log_returns_empty_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_missing_observed_time_index_log.idx";
    std::filesystem::remove(path);

    ObservedTimeIndexLog log(path);

    auto records = log.read_all();

    assert(records.empty());
}

void observed_time_index_log_rejects_invalid_record_size() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_observed_time_index_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        uint32_t bad_record_size = static_cast<uint32_t>(sizeof(ObservedTimeIndexRecord)) + 1;
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    ObservedTimeIndexLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void observed_time_index_log_ignores_incomplete_trailing_record() {
    auto path =
        std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_observed_time_index_log.idx";
    std::filesystem::remove(path);

    ObservedTimeIndexLog log(path);

    log.append(ObservedTimeIndexRecord{1, 1672531200, 1});

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        uint32_t record_size = static_cast<uint32_t>(sizeof(ObservedTimeIndexRecord));
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(sizeof(ObservedTimeIndexRecord) / 2, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].assertion_id == 1);

    std::filesystem::remove(path);
}

void observed_time_index_log_overwrite_all_replaces_prior_contents() {
    auto path = std::filesystem::temp_directory_path() / "kernel_overwrite_observed_time_index_log.idx";
    std::filesystem::remove(path);

    ObservedTimeIndexLog log(path);

    log.append(ObservedTimeIndexRecord{1, 1672531200, 1});
    log.append(ObservedTimeIndexRecord{2, 1704067200, 2});

    log.overwrite_all({ObservedTimeIndexRecord{3, 1719792000, 4}});

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].subject == 3);
    assert(records[0].observed_at == 1719792000);
    assert(records[0].assertion_id == 4);

    std::filesystem::remove(path);
}

} // namespace

int main() {
    observed_time_index_log_appends_and_reads_records();
    observed_time_index_log_returns_empty_when_missing();
    observed_time_index_log_rejects_invalid_record_size();
    observed_time_index_log_ignores_incomplete_trailing_record();
    observed_time_index_log_overwrite_all_replaces_prior_contents();

    std::cout << "All observed_time_index_log tests passed.\n";
}
