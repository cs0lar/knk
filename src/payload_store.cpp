#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "kernel/checksum.hpp"
#include "kernel/durability.hpp"
#include "kernel/payload_store.hpp"

namespace knk {

namespace {

constexpr std::array<char, 4> PAYLOAD_MAGIC{'K', 'N', 'K', 'D'};
constexpr uint32_t PAYLOAD_FORMAT_VERSION = 1;
constexpr size_t HEADER_SIZE = PAYLOAD_MAGIC.size() + sizeof(uint32_t);

constexpr char PAYLOAD_EXTENSION[] = ".payload";

} // namespace

PayloadStore::PayloadStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::filesystem::path PayloadStore::path_for(EntityId id) const {
    return directory_ / (std::to_string(id) + PAYLOAD_EXTENSION);
}

void PayloadStore::write(EntityId id, std::span<const std::byte> content) {
    uint64_t length = content.size();
    uint32_t crc = content.empty() ? crc32(nullptr, 0) : crc32(content.data(), content.size());

    write_file_atomically(path_for(id), [&](std::ostream &out) {
        out.write(PAYLOAD_MAGIC.data(), static_cast<std::streamsize>(PAYLOAD_MAGIC.size()));

        uint32_t version = PAYLOAD_FORMAT_VERSION;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));

        out.write(reinterpret_cast<const char *>(&length), sizeof(length));
        if (!content.empty()) {
            out.write(reinterpret_cast<const char *>(content.data()), static_cast<std::streamsize>(content.size()));
        }
        out.write(reinterpret_cast<const char *>(&crc), sizeof(crc));

        if (!out) {
            throw std::runtime_error("failed to write payload for entity " + std::to_string(id));
        }
    });
}

std::optional<std::vector<std::byte>> PayloadStore::read(EntityId id) const {
    auto path = path_for(id);

    std::error_code error;
    auto file_size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open payload for entity " + std::to_string(id));
    }

    std::array<char, HEADER_SIZE> header{};
    in.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (!in) {
        throw std::runtime_error("payload for entity " + std::to_string(id) + " has a truncated header");
    }

    std::array<char, 4> magic{};
    std::copy(header.begin(), header.begin() + 4, magic.begin());
    if (magic != PAYLOAD_MAGIC) {
        throw std::runtime_error("payload for entity " + std::to_string(id) + " has an invalid header");
    }

    uint32_t version = 0;
    std::memcpy(&version, header.data() + 4, sizeof(version));
    if (version != PAYLOAD_FORMAT_VERSION) {
        throw std::runtime_error("payload for entity " + std::to_string(id) + " has an unsupported format version");
    }

    uint64_t length = 0;
    in.read(reinterpret_cast<char *>(&length), sizeof(length));
    if (!in) {
        throw std::runtime_error("payload for entity " + std::to_string(id) + " has a truncated length field");
    }

    // Validated up front, before allocating/reading the content buffer, so a corrupted length field
    // can never drive a huge allocation -- same defensive ordering as SnapshotStore's record_count.
    uintmax_t expected_size = HEADER_SIZE + sizeof(length) + length + sizeof(uint32_t);
    if (file_size != expected_size) {
        throw std::runtime_error("payload for entity " + std::to_string(id) + " has an inconsistent length field");
    }

    std::vector<std::byte> content(length);
    if (length > 0) {
        in.read(reinterpret_cast<char *>(content.data()), static_cast<std::streamsize>(length));
        if (!in) {
            throw std::runtime_error("payload for entity " + std::to_string(id) + " has truncated content");
        }
    }

    uint32_t stored_crc = 0;
    in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));
    if (!in) {
        throw std::runtime_error("payload for entity " + std::to_string(id) + " has a truncated checksum");
    }

    uint32_t computed_crc = content.empty() ? crc32(nullptr, 0) : crc32(content.data(), content.size());
    if (computed_crc != stored_crc) {
        throw std::runtime_error("payload for entity " + std::to_string(id) + " failed checksum validation");
    }

    return content;
}

std::vector<EntityId> PayloadStore::existing_ids() const {
    std::vector<EntityId> ids;

    std::error_code error;
    if (!std::filesystem::exists(directory_, error) || error) {
        return ids;
    }

    for (const auto &entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const auto &path = entry.path();
        if (path.extension() != PAYLOAD_EXTENSION) {
            continue;
        }

        try {
            ids.push_back(static_cast<EntityId>(std::stoull(path.stem().string())));
        } catch (const std::exception &) {
            throw std::runtime_error("payload directory contains an unrecognized file: " + path.string());
        }
    }

    return ids;
}

} // namespace knk
