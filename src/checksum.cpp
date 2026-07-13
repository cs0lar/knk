#include <array>

#include "kernel/checksum.hpp"

namespace knk {

namespace {

constexpr uint32_t CRC32_POLYNOMIAL = 0xEDB88320;

std::array<uint32_t, 256> make_crc32_table() {
    std::array<uint32_t, 256> table{};

    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ CRC32_POLYNOMIAL : crc >> 1;
        }
        table[i] = crc;
    }

    return table;
}

const std::array<uint32_t, 256> &crc32_table() {
    static const std::array<uint32_t, 256> table = make_crc32_table();
    return table;
}

} // namespace

uint32_t crc32(const void *data, size_t size) {
    const auto &table = crc32_table();
    const auto *bytes = static_cast<const unsigned char *>(data);

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

} // namespace knk
