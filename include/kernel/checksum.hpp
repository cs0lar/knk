#pragma once

#include <cstddef>
#include <cstdint>

namespace knk {

uint32_t crc32(const void *data, size_t size);

// Incremental CRC-32, for a checksum over data that arrives in pieces. Start from CRC32_INIT, call
// crc32_update once per piece, and crc32_finalize at the end; the result equals crc32() over the
// concatenation of the pieces.
//
// Added for the columnar store (Phase 14), whose manifest holds one checksum per column: recomputing it
// over every row on each append would make appending O(store) instead of O(appended), i.e. quadratic
// over the life of the store. Note that finalization is a plain XOR and therefore invertible, which is
// what lets a store reload its running state from the finalized checksum in its manifest.
constexpr uint32_t CRC32_INIT = 0xFFFFFFFF;

uint32_t crc32_update(uint32_t crc, const void *data, size_t size);

uint32_t crc32_finalize(uint32_t crc);

} // namespace knk
