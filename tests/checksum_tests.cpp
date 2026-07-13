#include <cassert>
#include <cstring>
#include <iostream>

#include "kernel/checksum.hpp"

using namespace knk;

namespace {

void crc32_of_empty_input_is_zero() { assert(crc32(nullptr, 0) == 0); }

void crc32_matches_known_check_value() {
    // Standard CRC-32 (IEEE 802.3) check value for the ASCII string "123456789".
    const char *input = "123456789";
    assert(crc32(input, std::strlen(input)) == 0xCBF43926);
}

void crc32_is_deterministic_for_same_input() {
    const char *input = "knowledge kernel";
    auto len = std::strlen(input);

    assert(crc32(input, len) == crc32(input, len));
}

void crc32_differs_when_a_single_byte_changes() {
    char a[] = "knowledge kernel";
    char b[] = "knowledge Kernel";

    assert(crc32(a, sizeof(a) - 1) != crc32(b, sizeof(b) - 1));
}

} // namespace

int main() {
    crc32_of_empty_input_is_zero();
    crc32_matches_known_check_value();
    crc32_is_deterministic_for_same_input();
    crc32_differs_when_a_single_byte_changes();

    std::cout << "All checksum tests passed.\n";
}
