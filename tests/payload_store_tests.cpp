#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>

#include "kernel/payload_store.hpp"

using namespace knk;

namespace {

std::vector<std::byte> make_content(const std::string &text) {
    std::vector<std::byte> content(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        content[i] = static_cast<std::byte>(text[i]);
    }
    return content;
}

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("kernel_payload_store_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void payload_round_trips_content() {
    auto root = test_root("round_trip");
    PayloadStore store(root);

    auto content = make_content("hello, world");
    store.write(1, content);

    auto loaded = store.read(1);
    assert(loaded.has_value());
    assert(*loaded == content);

    std::filesystem::remove_all(root);
}

void payload_round_trips_empty_content() {
    auto root = test_root("empty");
    PayloadStore store(root);

    store.write(1, {});

    auto loaded = store.read(1);
    assert(loaded.has_value());
    assert(loaded->empty());

    std::filesystem::remove_all(root);
}

void read_returns_nullopt_when_missing() {
    auto root = test_root("missing");
    PayloadStore store(root);

    assert(!store.read(42).has_value());

    std::filesystem::remove_all(root);
}

void write_replaces_prior_payload() {
    auto root = test_root("replace");
    PayloadStore store(root);

    store.write(1, make_content("first"));
    store.write(1, make_content("second, and longer"));

    auto loaded = store.read(1);
    assert(loaded.has_value());
    assert(*loaded == make_content("second, and longer"));

    std::filesystem::remove_all(root);
}

void distinct_ids_do_not_collide() {
    auto root = test_root("distinct_ids");
    PayloadStore store(root);

    store.write(1, make_content("alice's document"));
    store.write(2, make_content("bob's document"));

    assert(*store.read(1) == make_content("alice's document"));
    assert(*store.read(2) == make_content("bob's document"));

    std::filesystem::remove_all(root);
}

void existing_ids_lists_every_written_payload() {
    auto root = test_root("existing_ids");
    PayloadStore store(root);

    store.write(5, make_content("a"));
    store.write(1, make_content("b"));
    store.write(9, make_content("c"));

    auto ids = store.existing_ids();
    std::sort(ids.begin(), ids.end());

    assert(ids.size() == 3);
    assert(ids[0] == 1);
    assert(ids[1] == 5);
    assert(ids[2] == 9);

    std::filesystem::remove_all(root);
}

void existing_ids_is_empty_when_directory_is_missing() {
    auto root = test_root("no_directory");
    PayloadStore store(root);

    assert(store.existing_ids().empty());

    std::filesystem::remove_all(root);
}

void read_throws_on_invalid_header() {
    auto root = test_root("invalid_header");
    PayloadStore store(root);
    store.write(1, make_content("valid"));

    auto path = root / "1.payload";
    {
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(0);
        io.write("XXXX", 4);
    }

    bool threw = false;
    try {
        store.read(1);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    std::filesystem::remove_all(root);
}

void read_throws_on_inconsistent_length() {
    auto root = test_root("inconsistent_length");
    PayloadStore store(root);
    store.write(1, make_content("valid"));

    auto path = root / "1.payload";
    {
        // Length field sits right after the 8-byte header.
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8);
        uint64_t bogus_length = 999999;
        io.write(reinterpret_cast<const char *>(&bogus_length), sizeof(bogus_length));
    }

    bool threw = false;
    try {
        store.read(1);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    std::filesystem::remove_all(root);
}

void read_throws_on_checksum_mismatch() {
    auto root = test_root("checksum_mismatch");
    PayloadStore store(root);
    store.write(1, make_content("valid"));

    auto path = root / "1.payload";
    {
        // Flip the last byte of the file, which sits inside the trailing crc32.
        auto size = std::filesystem::file_size(path);
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekg(static_cast<std::streamoff>(size) - 1);
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(static_cast<std::streamoff>(size) - 1);
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    bool threw = false;
    try {
        store.read(1);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    payload_round_trips_content();
    payload_round_trips_empty_content();
    read_returns_nullopt_when_missing();
    write_replaces_prior_payload();
    distinct_ids_do_not_collide();
    existing_ids_lists_every_written_payload();
    existing_ids_is_empty_when_directory_is_missing();
    read_throws_on_invalid_header();
    read_throws_on_inconsistent_length();
    read_throws_on_checksum_mismatch();

    std::cout << "All payload_store tests passed.\n";
}
