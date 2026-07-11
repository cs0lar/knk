#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/subject_index_log.hpp"

namespace knk {

namespace {

constexpr uint32_t SUBJECT_INDEX_RECORD_SIZE = sizeof(SubjectIndexRecord);

void write_or_throw(std::ofstream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write subject index log");
    }
}

} // namespace

SubjectIndexLog::SubjectIndexLog(std::filesystem::path path) : path_(std::move(path)) {}

void SubjectIndexLog::append(const SubjectIndexRecord &record) {
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream out(path_, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open subect index log for append");
    }

    uint32_t record_size = SUBJECT_INDEX_RECORD_SIZE;

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, reinterpret_cast<const char *>(&record), sizeof(record));
}

void SubjectIndexLog::overwrite_all(const std::vector<SubjectIndexRecord> &records) {
    std::filesystem::create_directories(path_.parent_path());

    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open subject index log for overwrite");
    }

    uint32_t record_size = SUBJECT_INDEX_RECORD_SIZE;

    for (const auto &record : records) {
        write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
        write_or_throw(out, reinterpret_cast<const char *>(&record), sizeof(record));
    }
}

std::vector<SubjectIndexRecord> SubjectIndexLog::read_all() const {
    std::vector<SubjectIndexRecord> records;

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

        if (record_size != SUBJECT_INDEX_RECORD_SIZE) {
            throw std::runtime_error("invalid subject index log record size");
        }

        SubjectIndexRecord record{};
        in.read(reinterpret_cast<char *>(&record), sizeof(record));

        if (!in) {
            break; // ignoring incomplete trailing record for now
        }

        records.push_back(record);
    }

    return records;
}

} // namespace knk