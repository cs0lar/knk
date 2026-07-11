#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <vector>

#include "kernel/current_index_log.hpp"
#include "kernel/ids.hpp"

namespace knk {

namespace {

constexpr uint32_t CURRENT_INDEX_RECORD_SIZE = sizeof(CurrentIndexRecord);

void write_or_throw(std::ofstream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write current index log");
    }
}

} // namespace

CurrentIndexLog::CurrentIndexLog(std::filesystem::path path) : path_(std::move(path)) {}

void CurrentIndexLog::append(const CurrentIndexRecord &record) {
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open current index log for append");
    }

    uint32_t record_size = CURRENT_INDEX_RECORD_SIZE;

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, reinterpret_cast<const char *>(&record), sizeof(record));
}

void CurrentIndexLog::overwrite_all(const std::vector<CurrentIndexRecord> &records) {
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open current index log for overwrite");
    }

    uint32_t record_size = CURRENT_INDEX_RECORD_SIZE;

    for (const auto &record : records) {
        write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
        write_or_throw(out, reinterpret_cast<const char *>(&record), sizeof(record));
    }
}

std::vector<CurrentIndexRecord> CurrentIndexLog::read_all() const {
    std::vector<CurrentIndexRecord> records;

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
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

        if (record_size != CURRENT_INDEX_RECORD_SIZE) {
            throw std::runtime_error("invalid current index log record size");
        }

        CurrentIndexRecord record{};
        in.read(reinterpret_cast<char *>(&record), sizeof(record));

        if (!in) {
            break; // ignore incomplete trailing record for now
        }

        records.push_back(record);
    }

    return records;
}

} // namespace knk
