#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "kernel/assertion_log.hpp"
#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"

namespace knk {

namespace {

constexpr uint32_t ASSERTION_RECORD_SIZE = sizeof(Assertion);
constexpr std::array<char, 4> LOG_MAGIC{'K', 'N', 'K', '1'};
constexpr uint32_t LOG_FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = LOG_MAGIC.size() + sizeof(uint32_t);
constexpr size_t FRAME_SIZE = sizeof(uint32_t) + sizeof(Assertion) + sizeof(uint32_t);

void write_or_throw(std::ofstream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write fact log segment");
    }
}

void write_header(std::ofstream &out) {
    write_or_throw(out, LOG_MAGIC.data(), static_cast<std::streamsize>(LOG_MAGIC.size()));

    uint32_t version = LOG_FORMAT_VERSION;
    write_or_throw(out, reinterpret_cast<const char *>(&version), sizeof(version));
}

// Returns false if the segment is empty, or has a torn header (fewer than HEADER_SIZE bytes). A torn
// header can only legitimately occur in the active (last) segment, from a crash mid-write on its
// very first-ever append; tolerate_partial is false for any earlier segment, where a torn header is
// impossible under normal operation and therefore unambiguous corruption. Throws unconditionally when
// a full-size header has the wrong magic/version, which a torn write cannot produce.
bool read_and_validate_header(std::ifstream &in, bool tolerate_partial) {
    std::array<char, HEADER_SIZE> header{};
    in.read(header.data(), static_cast<std::streamsize>(header.size()));

    auto got = static_cast<size_t>(in.gcount());
    if (got < HEADER_SIZE) {
        if (tolerate_partial) {
            return false;
        }
        throw std::runtime_error("torn header in a non-active fact log segment");
    }

    std::array<char, 4> magic{};
    std::copy(header.begin(), header.begin() + 4, magic.begin());

    if (magic != LOG_MAGIC) {
        throw std::runtime_error("invalid or missing fact log segment header");
    }

    uint32_t version = 0;
    std::memcpy(&version, header.data() + 4, sizeof(version));

    if (version != LOG_FORMAT_VERSION) {
        throw std::runtime_error("unsupported fact log segment format version");
    }

    return true;
}

// Shared tail-tolerant per-record read loop. A bad record_size always throws. A checksum mismatch or
// short read at true EOF is silently dropped only when tolerate_trailing_anomaly is true (this segment
// is the active/last one); otherwise it throws, since a torn trailing frame in an already-rolled-from
// segment is impossible under normal operation (see AssertionLog's class comment) and therefore
// unambiguous corruption, not a crash artifact.
std::vector<Assertion> read_records(std::ifstream &in, bool tolerate_trailing_anomaly) {
    std::vector<Assertion> assertions;

    while (true) {
        uint32_t record_size = 0;

        in.read(reinterpret_cast<char *>(&record_size), sizeof(record_size));

        if (in.eof()) {
            break;
        }

        if (!in) {
            break;
        }

        if (record_size != ASSERTION_RECORD_SIZE) {
            throw std::runtime_error("invalid fact log segment record size");
        }

        Assertion assertion{};
        in.read(reinterpret_cast<char *>(&assertion), sizeof(assertion));

        if (!in) {
            if (tolerate_trailing_anomaly) {
                break; // ignore incomplete trailing record for now
            }
            throw std::runtime_error("incomplete trailing record in a non-active fact log segment");
        }

        uint32_t stored_crc = 0;
        in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));

        if (!in) {
            if (tolerate_trailing_anomaly) {
                break; // ignore incomplete trailing record for now
            }
            throw std::runtime_error("incomplete trailing record in a non-active fact log segment");
        }

        if (crc32(&assertion, sizeof(assertion)) != stored_crc) {
            // A torn write can only ever leave garbage at the true end of the file, so a checksum
            // mismatch with nothing after it is treated the same as an incomplete trailing record --
            // but only when this segment is the active/last one; more segments existing after this one
            // proves it can't be an ordinary crash artifact.
            if (in.peek() == std::char_traits<char>::eof() && tolerate_trailing_anomaly) {
                break;
            }

            throw std::runtime_error("fact log segment checksum mismatch");
        }

        assertions.push_back(assertion);
    }

    return assertions;
}

std::string segment_filename(size_t segment_index) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "%010zu", segment_index);
    return std::string(buffer) + ".seg";
}

std::filesystem::path segment_path(const std::filesystem::path &segment_directory, size_t segment_index) {
    return segment_directory / segment_filename(segment_index);
}

// Lists the indices of existing segment files, ascending. Cheap: a directory scan, no record parsing.
std::vector<size_t> existing_segment_indices(const std::filesystem::path &segment_directory) {
    std::vector<size_t> indices;

    if (!std::filesystem::exists(segment_directory)) {
        return indices;
    }

    for (const auto &entry : std::filesystem::directory_iterator(segment_directory)) {
        if (entry.path().extension() != ".seg") {
            continue;
        }

        try {
            indices.push_back(std::stoull(entry.path().stem().string()));
        } catch (const std::exception &) {
            continue; // ignore unrelated/malformed filenames
        }
    }

    std::sort(indices.begin(), indices.end());

    return indices;
}

} // namespace

AssertionLog::AssertionLog(std::filesystem::path segment_directory, size_t max_records_per_segment)
    : segment_directory_(std::move(segment_directory)), max_records_per_segment_(max_records_per_segment),
      active_segment_index_(0), active_segment_count_(0) {
    auto indices = existing_segment_indices(segment_directory_);
    if (indices.empty()) {
        return;
    }

    active_segment_index_ = indices.back();

    // A file-size-based estimate, not a full parse: construction must never throw, even if the active
    // segment's content is malformed -- any real corruption instead surfaces lazily, the first time
    // read_all()/read_after() actually parses it. An imprecise estimate here only affects when append()
    // decides to roll to a new segment (a soft capacity bound); it never affects the correctness of
    // reads, which always re-derive the active segment's true content from disk independently.
    std::error_code error;
    auto file_size = std::filesystem::file_size(segment_path(segment_directory_, active_segment_index_), error);
    if (!error && file_size >= HEADER_SIZE) {
        active_segment_count_ = (file_size - HEADER_SIZE) / FRAME_SIZE;
    }
}

void AssertionLog::append(const Assertion &assertion) {
    if (active_segment_count_ >= max_records_per_segment_) {
        ++active_segment_index_;
        active_segment_count_ = 0;
    }

    auto path = segment_path(segment_directory_, active_segment_index_);

    std::filesystem::create_directories(segment_directory_);

    bool segment_is_new = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;

    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        throw std::runtime_error("failed to open fact log segment for append");
    }

    if (segment_is_new) {
        write_header(out);
    }

    uint32_t record_size = ASSERTION_RECORD_SIZE;
    uint32_t crc = crc32(&assertion, sizeof(assertion));

    write_or_throw(out, reinterpret_cast<const char *>(&record_size), sizeof(record_size));
    write_or_throw(out, reinterpret_cast<const char *>(&assertion), sizeof(assertion));
    write_or_throw(out, reinterpret_cast<const char *>(&crc), sizeof(crc));

    out.close();
    fsync_file(path);

    ++active_segment_count_;
}

std::vector<Assertion> AssertionLog::read_all() const {
    std::vector<Assertion> all;

    auto indices = existing_segment_indices(segment_directory_);
    for (size_t i = 0; i < indices.size(); ++i) {
        bool is_last = (i + 1 == indices.size());

        std::ifstream in(segment_path(segment_directory_, indices[i]), std::ios::binary);
        if (!in) {
            continue;
        }

        if (!read_and_validate_header(in, /*tolerate_partial=*/is_last)) {
            continue;
        }

        auto records = read_records(in, /*tolerate_trailing_anomaly=*/is_last);
        all.insert(all.end(), records.begin(), records.end());
    }

    return all;
}

std::vector<Assertion> AssertionLog::read_after(AssertionId last_seen_id) const {
    if (last_seen_id == 0) {
        return read_all();
    }

    std::vector<Assertion> result;

    auto indices = existing_segment_indices(segment_directory_);
    for (size_t i = 0; i < indices.size(); ++i) {
        size_t index = indices[i];
        bool is_last = (i + 1 == indices.size());

        // Every non-last segment is guaranteed to hold exactly max_records_per_segment_ complete
        // records (see class comment), so this range is exact, not an estimate.
        AssertionId segment_start_id = static_cast<AssertionId>(index) * max_records_per_segment_ + 1;
        AssertionId segment_end_id = segment_start_id + max_records_per_segment_ - 1;

        if (!is_last && segment_end_id <= last_seen_id) {
            continue; // entire segment already seen, skip without opening it
        }

        std::ifstream in(segment_path(segment_directory_, index), std::ios::binary);
        if (!in) {
            continue;
        }

        if (!read_and_validate_header(in, /*tolerate_partial=*/is_last)) {
            continue;
        }

        if (last_seen_id >= segment_start_id) {
            auto skip_count = last_seen_id - segment_start_id + 1;
            auto offset = static_cast<std::streamoff>(HEADER_SIZE + skip_count * FRAME_SIZE);
            in.seekg(offset, std::ios::beg);
        }

        auto records = read_records(in, /*tolerate_trailing_anomaly=*/is_last);
        result.insert(result.end(), records.begin(), records.end());
    }

    return result;
}

AssertionId AssertionLog::record_count_hint() const {
    auto indices = existing_segment_indices(segment_directory_);
    if (indices.empty()) {
        return 0;
    }

    size_t last_index = indices.back();
    AssertionId completed_count = static_cast<AssertionId>(last_index) * max_records_per_segment_;

    std::error_code error;
    auto file_size = std::filesystem::file_size(segment_path(segment_directory_, last_index), error);
    if (error || file_size < HEADER_SIZE) {
        return completed_count;
    }

    return completed_count + (file_size - HEADER_SIZE) / FRAME_SIZE;
}

} // namespace knk
