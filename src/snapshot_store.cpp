#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <system_error>

#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"
#include "kernel/snapshot_store.hpp"

namespace knk {

namespace {

constexpr std::array<char, 4> SNAPSHOT_MAGIC{'K', 'N', 'K', 'S'};
// v2: records carry the status they were appended with, not the effective one (see the header).
constexpr uint32_t SNAPSHOT_FORMAT_VERSION = 2;
constexpr size_t HEADER_SIZE = SNAPSHOT_MAGIC.size() + sizeof(uint32_t);

// last_snapshotted_id, record_count, and the raw assertion bytes are checksummed together as one
// contiguous buffer -- crc32() only takes a single buffer, and a snapshot is rare/explicit rather
// than on the hot commit path, so the extra copy is an acceptable simplicity tradeoff.
// `appended_status` overrides each record's status byte when non-empty, which is how write() stores the
// appended status without copying the whole vector first. read() passes an empty span: the records it
// just parsed already carry the stored byte, so both callers checksum identical bytes.
std::vector<char> build_payload(uint64_t last_snapshotted_id, uint64_t record_count,
                                const std::vector<Assertion> &assertions, std::span<const uint8_t> appended_status) {
    std::vector<char> payload(sizeof(last_snapshotted_id) + sizeof(record_count) +
                              assertions.size() * sizeof(Assertion));

    size_t offset = 0;
    std::memcpy(payload.data() + offset, &last_snapshotted_id, sizeof(last_snapshotted_id));
    offset += sizeof(last_snapshotted_id);
    std::memcpy(payload.data() + offset, &record_count, sizeof(record_count));
    offset += sizeof(record_count);

    for (size_t i = 0; i < assertions.size(); ++i) {
        Assertion record = assertions[i];
        if (!appended_status.empty()) {
            record.status = static_cast<AssertionStatus>(appended_status[i]);
        }
        std::memcpy(payload.data() + offset + i * sizeof(Assertion), &record, sizeof(Assertion));
    }

    return payload;
}

} // namespace

SnapshotStore::SnapshotStore(std::filesystem::path path) : path_(std::move(path)) {}

void SnapshotStore::write(AssertionId last_snapshotted_id, const std::vector<Assertion> &assertions,
                          std::span<const uint8_t> appended_status) {
    if (appended_status.size() != assertions.size()) {
        throw std::runtime_error("snapshot appended_status must be parallel to assertions");
    }

    uint64_t record_count = assertions.size();
    auto payload = build_payload(last_snapshotted_id, record_count, assertions, appended_status);
    uint32_t crc = crc32(payload.data(), payload.size());

    write_file_atomically(path_, [&](std::ostream &out) {
        out.write(SNAPSHOT_MAGIC.data(), static_cast<std::streamsize>(SNAPSHOT_MAGIC.size()));

        uint32_t version = SNAPSHOT_FORMAT_VERSION;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));

        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.write(reinterpret_cast<const char *>(&crc), sizeof(crc));
    });
}

std::optional<SnapshotData> SnapshotStore::read() const {
    std::error_code error;
    auto file_size = std::filesystem::file_size(path_, error);
    if (error) {
        return std::nullopt;
    }

    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    std::array<char, 4> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != SNAPSHOT_MAGIC) {
        return std::nullopt;
    }

    uint32_t version = 0;
    in.read(reinterpret_cast<char *>(&version), sizeof(version));
    if (!in || version != SNAPSHOT_FORMAT_VERSION) {
        return std::nullopt;
    }

    uint64_t last_snapshotted_id = 0;
    in.read(reinterpret_cast<char *>(&last_snapshotted_id), sizeof(last_snapshotted_id));
    if (!in) {
        return std::nullopt;
    }

    uint64_t record_count = 0;
    in.read(reinterpret_cast<char *>(&record_count), sizeof(record_count));
    if (!in) {
        return std::nullopt;
    }

    // Ids are assigned 1..N with no gaps (one log record per id), so a genuine snapshot always has
    // record_count == last_snapshotted_id; any mismatch means the file is corrupt or foreign.
    if (record_count != last_snapshotted_id) {
        return std::nullopt;
    }

    // Validate the file is exactly the expected size before allocating/reading the assertion
    // array -- this rejects a corrupted, implausibly large record_count without ever attempting a
    // huge allocation, keeping read() exception-free.
    uintmax_t expected_size = HEADER_SIZE + sizeof(last_snapshotted_id) + sizeof(record_count) +
                              record_count * sizeof(Assertion) + sizeof(uint32_t);
    if (file_size != expected_size) {
        return std::nullopt;
    }

    std::vector<Assertion> assertions(record_count);
    if (record_count > 0) {
        in.read(reinterpret_cast<char *>(assertions.data()),
                static_cast<std::streamsize>(record_count * sizeof(Assertion)));
        if (!in) {
            return std::nullopt;
        }
    }

    uint32_t stored_crc = 0;
    in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));
    if (!in) {
        return std::nullopt;
    }

    auto payload = build_payload(last_snapshotted_id, record_count, assertions, {});
    if (crc32(payload.data(), payload.size()) != stored_crc) {
        return std::nullopt;
    }

    return SnapshotData{last_snapshotted_id, std::move(assertions)};
}

} // namespace knk
