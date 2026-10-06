#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "kernel/snapshot_store.hpp"

using namespace knk;

namespace {

Assertion make_assertion(AssertionId id, EntityId subject) {
    return Assertion{id, subject, 1, 2, 0, 0, 100, 1.0, AssertionStatus::Active};
}

// v2 stores the status each record was *appended* with, supplied alongside the records because the
// in-memory ones carry the effective status (see snapshot_store.hpp). Everything here is committed Active.
std::vector<uint8_t> active_statuses(size_t count) {
    return std::vector<uint8_t>(count, static_cast<uint8_t>(AssertionStatus::Active));
}

void snapshot_store_round_trips_written_assertions() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_round_trip.dat";
    std::filesystem::remove(path);

    std::vector<Assertion> assertions{make_assertion(1, 10), make_assertion(2, 20)};

    SnapshotStore store(path);
    store.write(2, assertions, active_statuses(assertions.size()));

    auto loaded = store.read();
    assert(loaded.has_value());
    assert(loaded->last_snapshotted_id == 2);
    assert(loaded->assertions.size() == 2);
    assert(loaded->assertions[0].id == 1 && loaded->assertions[0].subject == 10);
    assert(loaded->assertions[1].id == 2 && loaded->assertions[1].subject == 20);

    std::filesystem::remove(path);
}

void snapshot_store_returns_nullopt_when_missing() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_missing.dat";
    std::filesystem::remove(path);

    SnapshotStore store(path);

    assert(!store.read().has_value());
}

void snapshot_store_write_replaces_prior_snapshot() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_replace.dat";
    std::filesystem::remove(path);

    SnapshotStore store(path);
    store.write(1, {make_assertion(1, 10)}, active_statuses(1));
    store.write(2, {make_assertion(1, 10), make_assertion(2, 20)}, active_statuses(2));

    auto loaded = store.read();
    assert(loaded.has_value());
    assert(loaded->last_snapshotted_id == 2);
    assert(loaded->assertions.size() == 2);

    std::filesystem::remove(path);
}

void snapshot_store_round_trips_empty_snapshot() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_empty.dat";
    std::filesystem::remove(path);

    SnapshotStore store(path);
    store.write(0, {}, {});

    auto loaded = store.read();
    assert(loaded.has_value());
    assert(loaded->last_snapshotted_id == 0);
    assert(loaded->assertions.empty());

    std::filesystem::remove(path);
}

void snapshot_store_never_throws_on_garbage_file() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_garbage.dat";
    std::filesystem::remove(path);

    {
        std::ofstream out(path, std::ios::binary);
        out.write("not a snapshot!!", 16);
    }

    SnapshotStore store(path);

    assert(!store.read().has_value());

    std::filesystem::remove(path);
}

void snapshot_store_never_throws_when_checksum_is_tampered_with() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_tampered_crc.dat";
    std::filesystem::remove(path);

    SnapshotStore store(path);
    store.write(1, {make_assertion(1, 10)}, active_statuses(1));

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

    assert(!store.read().has_value());

    std::filesystem::remove(path);
}

void snapshot_store_never_throws_when_record_count_is_inconsistent() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_bad_count.dat";
    std::filesystem::remove(path);

    SnapshotStore store(path);
    store.write(1, {make_assertion(1, 10)}, active_statuses(1));

    {
        // Corrupt record_count (right after the 8-byte header + 8-byte last_snapshotted_id) so it no
        // longer matches last_snapshotted_id, without ever letting a huge bogus value drive an
        // allocation.
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(16);
        uint64_t bogus_count = 999999;
        io.write(reinterpret_cast<const char *>(&bogus_count), sizeof(bogus_count));
    }

    assert(!store.read().has_value());

    std::filesystem::remove(path);
}

// v2 (Phase 19) stores the status a record was *appended* with, not the one it currently has. Pinned
// because nothing else in this file would notice the difference, and the whole point of the change is
// that a snapshot of a closed row must still say what it was committed as.
void snapshot_store_writes_the_appended_status_not_the_effective_one() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_appended_status.dat";
    std::filesystem::remove(path);

    Assertion superseded_now = make_assertion(1, 10);
    superseded_now.status = AssertionStatus::Superseded; // what it is today

    SnapshotStore store(path);
    std::vector<uint8_t> appended{static_cast<uint8_t>(AssertionStatus::Hypothesis)}; // what it was committed as

    store.write(1, {superseded_now}, appended);

    auto loaded = store.read();
    assert(loaded.has_value());
    assert(loaded->assertions.size() == 1);
    assert(loaded->assertions.front().status == AssertionStatus::Hypothesis);
}

void snapshot_store_rejects_a_mismatched_status_array() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_bad_status_array.dat";
    std::filesystem::remove(path);

    SnapshotStore store(path);

    bool threw = false;
    try {
        store.write(2, {make_assertion(1, 10), make_assertion(2, 20)}, active_statuses(1));
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    std::filesystem::remove(path);
}

// A snapshot written by an older build is not readable by this one, and says so by being unusable rather
// than by being misread -- the format version is the whole mechanism, so it gets a test.
void snapshot_store_refuses_an_older_format_version() {
    auto path = std::filesystem::temp_directory_path() / "kernel_snapshot_v1.dat";
    std::filesystem::remove(path);

    SnapshotStore store(path);
    store.write(1, {make_assertion(1, 10)}, active_statuses(1));

    {
        // Rewrite the version word in the header (4 magic bytes, then uint32 version) as v1.
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(4);
        uint32_t old_version = 1;
        io.write(reinterpret_cast<const char *>(&old_version), sizeof(old_version));
    }

    assert(!store.read().has_value());

    std::filesystem::remove(path);
}

} // namespace

int main() {
    snapshot_store_round_trips_written_assertions();
    snapshot_store_writes_the_appended_status_not_the_effective_one();
    snapshot_store_rejects_a_mismatched_status_array();
    snapshot_store_refuses_an_older_format_version();
    snapshot_store_returns_nullopt_when_missing();
    snapshot_store_write_replaces_prior_snapshot();
    snapshot_store_round_trips_empty_snapshot();
    snapshot_store_never_throws_on_garbage_file();
    snapshot_store_never_throws_when_checksum_is_tampered_with();
    snapshot_store_never_throws_when_record_count_is_inconsistent();

    std::cout << "All snapshot_store tests passed.\n";
}
