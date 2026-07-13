#include <cassert>
#include <fstream>
#include <iostream>

#include "kernel/index_checkpoint.hpp"

using namespace knk;

namespace {

void index_checkpoint_round_trips_a_written_value() {
    auto path = std::filesystem::temp_directory_path() / "kernel_index_checkpoint_round_trip.dat";
    std::filesystem::remove(path);

    IndexCheckpoint checkpoint(path);
    checkpoint.write(42);

    assert(checkpoint.read() == 42);

    std::filesystem::remove(path);
}

void index_checkpoint_returns_zero_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_index_checkpoint_missing.dat";
    std::filesystem::remove(path);

    IndexCheckpoint checkpoint(path);

    assert(checkpoint.read() == 0);
}

void index_checkpoint_write_replaces_prior_value() {
    auto path = std::filesystem::temp_directory_path() / "kernel_index_checkpoint_replace.dat";
    std::filesystem::remove(path);

    IndexCheckpoint checkpoint(path);
    checkpoint.write(1);
    checkpoint.write(2);

    assert(checkpoint.read() == 2);

    std::filesystem::remove(path);
}

void index_checkpoint_never_throws_on_corrupt_file() {
    auto path = std::filesystem::temp_directory_path() / "kernel_index_checkpoint_corrupt.dat";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("garbage!", 8);
    }

    IndexCheckpoint checkpoint(path);

    assert(checkpoint.read() == 0);

    std::filesystem::remove(path);
}

void index_checkpoint_never_throws_when_stored_value_is_tampered_with() {
    auto path = std::filesystem::temp_directory_path() / "kernel_index_checkpoint_tampered.dat";
    std::filesystem::remove(path);

    IndexCheckpoint checkpoint(path);
    checkpoint.write(7);

    {
        // Flip a byte inside the stored AssertionId payload, which sits right after the 8-byte header.
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8);
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8);
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    assert(checkpoint.read() == 0);

    std::filesystem::remove(path);
}

} // namespace

int main() {
    index_checkpoint_round_trips_a_written_value();
    index_checkpoint_returns_zero_when_missing();
    index_checkpoint_write_replaces_prior_value();
    index_checkpoint_never_throws_on_corrupt_file();
    index_checkpoint_never_throws_when_stored_value_is_tampered_with();

    std::cout << "All index_checkpoint tests passed.\n";
}
