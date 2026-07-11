#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "kernel/current_index_log.hpp"

using namespace knk;

namespace {

void current_index_log_appends_and_reads_records() {
    auto path = std::filesystem::temp_directory_path() / "kernel_current_index_log_test.idx";
    std::filesystem::remove(path);

    CurrentIndexLog log(path);

    log.append(CurrentIndexRecord{1, 10, 1, true});
    log.append(CurrentIndexRecord{1, 10, 2, true});
    log.append(CurrentIndexRecord{1, 10, 1, false});

    auto records = log.read_all();

    assert(records.size() == 3);
    assert(records[0].subject == 1);
    assert(records[0].predicate == 10);
    assert(records[0].assertion_id == 1);
    assert(records[0].active == true);
    assert(records[1].assertion_id == 2);
    assert(records[1].active == true);
    assert(records[2].assertion_id == 1);
    assert(records[2].active == false);

    std::filesystem::remove(path);
}

void current_index_log_returns_empty_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_missing_current_index_log.idx";
    std::filesystem::remove(path);

    CurrentIndexLog log(path);

    auto records = log.read_all();

    assert(records.empty());
}

void current_index_log_rejects_invalid_record_size() {
    auto path = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_current_index_log.idx";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        uint32_t bad_record_size = static_cast<uint32_t>(sizeof(CurrentIndexRecord)) + 1;
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    CurrentIndexLog log(path);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove(path);
}

void current_index_log_ignores_incomplete_trailing_record() {
    auto path = std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_current_index_log.idx";
    std::filesystem::remove(path);

    CurrentIndexLog log(path);

    log.append(CurrentIndexRecord{1, 10, 1, true});

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        uint32_t record_size = static_cast<uint32_t>(sizeof(CurrentIndexRecord));
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(sizeof(CurrentIndexRecord) / 2, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].assertion_id == 1);

    std::filesystem::remove(path);
}

void current_index_log_overwrite_all_replaces_prior_contents() {
    auto path = std::filesystem::temp_directory_path() / "kernel_overwrite_current_index_log.idx";
    std::filesystem::remove(path);

    CurrentIndexLog log(path);

    log.append(CurrentIndexRecord{1, 10, 1, true});
    log.append(CurrentIndexRecord{2, 20, 2, true});

    log.overwrite_all({CurrentIndexRecord{3, 30, 4, true}});

    auto records = log.read_all();

    assert(records.size() == 1);
    assert(records[0].subject == 3);
    assert(records[0].predicate == 30);
    assert(records[0].assertion_id == 4);
    assert(records[0].active == true);

    std::filesystem::remove(path);
}

} // namespace

int main() {
    current_index_log_appends_and_reads_records();
    current_index_log_returns_empty_when_missing();
    current_index_log_rejects_invalid_record_size();
    current_index_log_ignores_incomplete_trailing_record();
    current_index_log_overwrite_all_replaces_prior_contents();

    std::cout << "All current_index_log tests passed.\n";
}
