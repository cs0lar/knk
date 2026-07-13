#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "kernel/durability.hpp"

using namespace knk;

namespace {

std::string read_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

void fsync_file_succeeds_for_an_existing_file() {
    auto path = std::filesystem::temp_directory_path() / "kernel_fsync_existing_file.tmp";
    {
        std::ofstream out(path, std::ios::binary);
        out << "hello";
    }

    fsync_file(path); // should not throw

    std::filesystem::remove(path);
}

void fsync_file_throws_for_a_missing_path() {
    auto path = std::filesystem::temp_directory_path() / "kernel_fsync_missing_file.tmp";
    std::filesystem::remove(path);

    bool threw = false;
    try {
        fsync_file(path);
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);
}

void write_file_atomically_writes_full_contents() {
    auto path = std::filesystem::temp_directory_path() / "kernel_atomic_write_test.dat";
    std::filesystem::remove(path);

    write_file_atomically(path, [](std::ostream &out) { out << "first-version"; });

    assert(read_file(path) == "first-version");

    std::filesystem::remove(path);
}

void write_file_atomically_replaces_prior_contents_and_cleans_up_temp_file() {
    auto path = std::filesystem::temp_directory_path() / "kernel_atomic_replace_test.dat";
    std::filesystem::remove(path);

    write_file_atomically(path, [](std::ostream &out) { out << "first-version"; });
    write_file_atomically(path, [](std::ostream &out) { out << "second-version, which is longer"; });

    assert(read_file(path) == "second-version, which is longer");

    auto tmp_path = path;
    tmp_path += ".tmp";
    assert(!std::filesystem::exists(tmp_path));

    std::filesystem::remove(path);
}

void write_file_atomically_creates_parent_directories() {
    auto root = std::filesystem::temp_directory_path() / "kernel_atomic_write_parent_dirs_test";
    std::filesystem::remove_all(root);

    auto path = root / "nested" / "file.dat";
    write_file_atomically(path, [](std::ostream &out) { out << "content"; });

    assert(read_file(path) == "content");

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    fsync_file_succeeds_for_an_existing_file();
    fsync_file_throws_for_a_missing_path();
    write_file_atomically_writes_full_contents();
    write_file_atomically_replaces_prior_contents_and_cleans_up_temp_file();
    write_file_atomically_creates_parent_directories();

    std::cout << "All durability tests passed.\n";
}
