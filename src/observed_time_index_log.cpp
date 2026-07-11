#include <fstream>
#include <stdexcept>

#include "kernel/observed_time_index_log.hpp"

namespace knk {

namespace {

constexpr uint32_t OBSERVED_TIME_INDEX_RECORD_SIZE = sizeof(ObservedTimeIndexRecord);

void write_or_throw(std::ofstream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write observed-time index log");
    }
}

} // namespace

ObservedTimeIndexLog::ObservedTimeIndexLog(std::filesystem::path path) : path_(std::move(path)) {}

void ObservedTimeIndexLog::append(const ObservedTimeIndexRecord &record) {
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open observed-time index log for append");
    }

    uint32_t record_size = OBSERVED_TIME_INDEX_RECORD_SIZE;

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, reinterpret_cast<const char *>(&record), sizeof(record));
}

void ObservedTimeIndexLog::overwrite_all(const std::vector<ObservedTimeIndexRecord> &records) {
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open observed-time index log for overwrite");
    }

    uint32_t record_size = OBSERVED_TIME_INDEX_RECORD_SIZE;

    for (const auto &record : records) {
        write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
        write_or_throw(out, reinterpret_cast<const char *>(&record), sizeof(record));
    }
}

std::vector<ObservedTimeIndexRecord> ObservedTimeIndexLog::read_all() const {
    std::vector<ObservedTimeIndexRecord> records;

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

        if (record_size != OBSERVED_TIME_INDEX_RECORD_SIZE) {
            throw std::runtime_error("invalid observed-time index log record size");
        }

        ObservedTimeIndexRecord record{};
        in.read(reinterpret_cast<char *>(&record), sizeof(record));

        if (!in) {
            break; // ignore incomplete trailing record for now
        }

        records.push_back(record);
    }

    return records;
}

} // namespace knk
