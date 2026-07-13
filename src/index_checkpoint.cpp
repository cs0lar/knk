#include <array>
#include <cstdint>
#include <fstream>

#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"
#include "kernel/index_checkpoint.hpp"

namespace knk {

namespace {

constexpr std::array<char, 4> CHECKPOINT_MAGIC{'K', 'N', 'K', 'C'};
constexpr uint32_t CHECKPOINT_FORMAT_VERSION = 1;

} // namespace

IndexCheckpoint::IndexCheckpoint(std::filesystem::path path) : path_(std::move(path)) {}

void IndexCheckpoint::write(AssertionId last_fully_indexed_id) {
    write_file_atomically(path_, [&](std::ostream &out) {
        out.write(CHECKPOINT_MAGIC.data(), static_cast<std::streamsize>(CHECKPOINT_MAGIC.size()));

        uint32_t version = CHECKPOINT_FORMAT_VERSION;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));

        out.write(reinterpret_cast<const char *>(&last_fully_indexed_id), sizeof(last_fully_indexed_id));

        uint32_t crc = crc32(&last_fully_indexed_id, sizeof(last_fully_indexed_id));
        out.write(reinterpret_cast<const char *>(&crc), sizeof(crc));
    });
}

AssertionId IndexCheckpoint::read() const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return 0;
    }

    std::array<char, 4> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || magic != CHECKPOINT_MAGIC) {
        return 0;
    }

    uint32_t version = 0;
    in.read(reinterpret_cast<char *>(&version), sizeof(version));
    if (!in || version != CHECKPOINT_FORMAT_VERSION) {
        return 0;
    }

    AssertionId last_fully_indexed_id = 0;
    in.read(reinterpret_cast<char *>(&last_fully_indexed_id), sizeof(last_fully_indexed_id));
    if (!in) {
        return 0;
    }

    uint32_t stored_crc = 0;
    in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));
    if (!in) {
        return 0;
    }

    if (crc32(&last_fully_indexed_id, sizeof(last_fully_indexed_id)) != stored_crc) {
        return 0;
    }

    return last_fully_indexed_id;
}

} // namespace knk
