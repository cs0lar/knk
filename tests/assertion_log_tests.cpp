#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "kernel/assertion_log.hpp"

using namespace knk;

namespace {

constexpr size_t HEADER_SIZE = 8;
constexpr size_t FRAME_SIZE = sizeof(uint32_t) + sizeof(Assertion) + sizeof(uint32_t);

std::filesystem::path segment_file(const std::filesystem::path &dir, size_t index) {
    char buffer[11];
    std::snprintf(buffer, sizeof(buffer), "%010zu", index);
    return dir / (std::string(buffer) + ".seg");
}

Assertion make_assertion(AssertionId id, EntityId subject) {
    return Assertion{.id = id,
                     .subject = subject,
                     .predicate = 10,
                     .object = 100,
                     .valid_from = 1672531200,
                     .valid_to = OPEN_ENDED,
                     .observed_at = 1719878400,
                     .confidence = 0.95,
                     .status = AssertionStatus::Active};
}

void assertion_log_appends_and_reads_assertions() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_assertion_log_test";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);

    log.append(make_assertion(1, 1));

    auto assertions = log.read_all();

    assert(assertions.size() == 1);
    assert(assertions[0].id == 1);
    assert(assertions[0].subject == 1);
    assert(assertions[0].predicate == 10);
    assert(assertions[0].object == 100);
    assert(assertions[0].confidence == 0.95);
    assert(assertions[0].status == AssertionStatus::Active);

    std::filesystem::remove_all(dir);
}

void assertion_log_returns_empty_when_missing() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_missing_assertion_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);

    auto assertions = log.read_all();

    assert(assertions.empty());
}

void assertion_log_rejects_invalid_record_size() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_invalid_record_size_log";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream out(segment_file(dir, 0), std::ios::binary);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        uint32_t bad_record_size = static_cast<uint32_t>(sizeof(Assertion)) + 1;
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    AssertionLog log(dir, 100);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove_all(dir);
}

void assertion_log_rejects_missing_or_invalid_header() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_invalid_header_log";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        std::ofstream out(segment_file(dir, 0), std::ios::binary);
        out.write("XXXX", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    }

    AssertionLog log(dir, 100);

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove_all(dir);
}

void assertion_log_recovers_partial_header_as_empty_log() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_partial_header_log";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    {
        // Fewer than the full 8-byte header, simulating a crash mid-write on the very
        // first-ever append to a brand-new segment -- by construction, zero records could
        // have been durably completed yet.
        std::ofstream out(segment_file(dir, 0), std::ios::binary);
        out.write("KNK", 3);
    }

    AssertionLog log(dir, 100);

    auto assertions = log.read_all();

    assert(assertions.empty());

    std::filesystem::remove_all(dir);
}

void assertion_log_recovers_tail_checksum_mismatch_as_torn_write() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_tail_checksum_mismatch_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);

    log.append(make_assertion(1, 1));

    {
        // Flip a byte inside the record payload, which sits right after the 8-byte
        // header and the 4-byte record-size prefix. Nothing follows this record anywhere
        // in the log, so it is indistinguishable from a crash mid-append and should be
        // silently dropped.
        std::fstream io(segment_file(dir, 0), std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    auto assertions = log.read_all();

    assert(assertions.empty());

    std::filesystem::remove_all(dir);
}

void assertion_log_rejects_checksum_mismatch_when_followed_by_more_data() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_mid_file_checksum_mismatch_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);

    log.append(make_assertion(1, 1));

    {
        // Flip a byte inside the first record's payload before a second, valid record is
        // appended after it in the same segment -- data can't validly follow a torn write,
        // so this is unambiguous corruption, not a crash artifact, and must still throw.
        std::fstream io(segment_file(dir, 0), std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    log.append(make_assertion(2, 2));

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove_all(dir);
}

void assertion_log_ignores_incomplete_trailing_record() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_incomplete_trailing_record_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);

    log.append(make_assertion(1, 1));

    {
        std::ofstream out(segment_file(dir, 0), std::ios::binary | std::ios::app);
        uint32_t record_size = static_cast<uint32_t>(sizeof(Assertion));
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));

        std::vector<char> partial_payload(sizeof(Assertion) / 2, 0);
        out.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    auto assertions = log.read_all();

    assert(assertions.size() == 1);
    assert(assertions[0].id == 1);

    std::filesystem::remove_all(dir);
}

void assertion_log_read_after_zero_behaves_like_read_all() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_read_after_zero_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);
    log.append(make_assertion(1, 1));
    log.append(make_assertion(2, 2));

    auto all = log.read_all();
    auto after_zero = log.read_after(0);

    assert(all.size() == after_zero.size());
    assert(after_zero.size() == 2);
    assert(after_zero[0].id == 1);
    assert(after_zero[1].id == 2);

    std::filesystem::remove_all(dir);
}

void assertion_log_read_after_returns_only_records_committed_after_the_given_id() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_read_after_tail_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);
    log.append(make_assertion(1, 1));
    log.append(make_assertion(2, 2));
    log.append(make_assertion(3, 3));

    auto tail = log.read_after(1);

    assert(tail.size() == 2);
    assert(tail[0].id == 2);
    assert(tail[1].id == 3);

    std::filesystem::remove_all(dir);
}

void assertion_log_read_after_beyond_available_records_returns_empty() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_read_after_beyond_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);
    log.append(make_assertion(1, 1));

    auto tail = log.read_after(5);

    assert(tail.empty());

    std::filesystem::remove_all(dir);
}

void assertion_log_read_after_recovers_tail_checksum_mismatch_as_torn_write() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_read_after_tail_checksum_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);
    log.append(make_assertion(1, 1));
    log.append(make_assertion(2, 2));

    {
        // Flip a byte inside the second record's payload; nothing follows it anywhere in the
        // log, so it must be silently dropped rather than throwing, exactly like read_all()'s
        // tail-tolerance.
        std::fstream io(segment_file(dir, 0), std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + FRAME_SIZE + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + FRAME_SIZE + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    auto tail = log.read_after(1);

    assert(tail.empty());

    std::filesystem::remove_all(dir);
}

void assertion_log_record_count_hint_matches_appended_record_count() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_record_count_hint_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);

    assert(log.record_count_hint() == 0);

    log.append(make_assertion(1, 1));
    log.append(make_assertion(2, 2));
    log.append(make_assertion(3, 3));

    assert(log.record_count_hint() == 3);

    std::filesystem::remove_all(dir);
}

void assertion_log_rolls_over_to_a_new_segment_when_capacity_is_reached() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_segment_rollover_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 2);

    log.append(make_assertion(1, 1));
    log.append(make_assertion(2, 2));

    assert(std::filesystem::exists(segment_file(dir, 0)));
    assert(!std::filesystem::exists(segment_file(dir, 1)));

    log.append(make_assertion(3, 3));

    assert(std::filesystem::exists(segment_file(dir, 1)));

    auto all = log.read_all();
    assert(all.size() == 3);
    assert(all[0].id == 1 && all[1].id == 2 && all[2].id == 3);

    std::filesystem::remove_all(dir);
}

void assertion_log_append_batch_writes_every_record_in_order() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_append_batch_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);

    std::vector<Assertion> batch{make_assertion(1, 1), make_assertion(2, 2), make_assertion(3, 3)};
    log.append_batch(batch);

    auto all = log.read_all();
    assert(all.size() == 3);
    assert(all[0].id == 1 && all[1].id == 2 && all[2].id == 3);
    assert(all[0].subject == 1 && all[1].subject == 2 && all[2].subject == 3);

    // One batch is one segment file with one header, not one file per record.
    assert(std::filesystem::file_size(segment_file(dir, 0)) == HEADER_SIZE + 3 * FRAME_SIZE);

    // A batch is a plain append, so a single append continues straight after it.
    log.append(make_assertion(4, 4));
    assert(log.read_all().size() == 4);

    std::filesystem::remove_all(dir);
}

void assertion_log_append_batch_is_a_no_op_for_an_empty_batch() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_append_batch_empty_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 100);

    log.append_batch({});

    // No header-only segment left behind: an empty batch must not make the log look non-empty.
    assert(!std::filesystem::exists(segment_file(dir, 0)));
    assert(log.read_all().empty());
    assert(log.record_count_hint() == 0);

    std::filesystem::remove_all(dir);
}

void assertion_log_append_batch_rolls_segments_and_keeps_them_exactly_full() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_append_batch_rollover_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 2);

    // One record first, so the batch starts mid-segment and every subsequent boundary lands inside
    // it -- the case where append_batch must split its writes across three segment files.
    log.append(make_assertion(1, 1));

    std::vector<Assertion> batch;
    for (AssertionId id = 2; id <= 6; ++id) {
        batch.push_back(make_assertion(id, id));
    }
    log.append_batch(batch);

    // The invariant read_after's id arithmetic depends on: every non-active segment holds exactly
    // max_records_per_segment complete records, mid-batch boundaries included.
    assert(std::filesystem::file_size(segment_file(dir, 0)) == HEADER_SIZE + 2 * FRAME_SIZE);
    assert(std::filesystem::file_size(segment_file(dir, 1)) == HEADER_SIZE + 2 * FRAME_SIZE);
    assert(std::filesystem::file_size(segment_file(dir, 2)) == HEADER_SIZE + 2 * FRAME_SIZE);
    assert(!std::filesystem::exists(segment_file(dir, 3)));

    auto all = log.read_all();
    assert(all.size() == 6);
    for (size_t i = 0; i < all.size(); ++i) {
        assert(all[i].id == static_cast<AssertionId>(i + 1));
    }

    assert(log.record_count_hint() == 6);

    // read_after's whole-segment skipping still lands on the right records after a batch wrote
    // across those boundaries.
    auto tail = log.read_after(3);
    assert(tail.size() == 3);
    assert(tail[0].id == 4 && tail[1].id == 5 && tail[2].id == 6);

    std::filesystem::remove_all(dir);
}

void assertion_log_read_all_spans_multiple_segments_in_order() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_read_all_multi_segment_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 2);
    for (AssertionId id = 1; id <= 5; ++id) {
        log.append(make_assertion(id, id));
    }

    auto all = log.read_all();

    assert(all.size() == 5);
    for (AssertionId id = 1; id <= 5; ++id) {
        assert(all[id - 1].id == id);
    }

    std::filesystem::remove_all(dir);
}

void assertion_log_read_after_skips_whole_segments_before_the_seek_point() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_read_after_skip_segments_log";
    std::filesystem::remove_all(dir);

    // Capacity 2: segments hold [1,2], [3,4], [5,6].
    AssertionLog log(dir, 2);
    for (AssertionId id = 1; id <= 6; ++id) {
        log.append(make_assertion(id, id));
    }

    auto tail = log.read_after(4);

    assert(tail.size() == 2);
    assert(tail[0].id == 5);
    assert(tail[1].id == 6);

    std::filesystem::remove_all(dir);
}

void assertion_log_read_after_seeks_within_the_straddling_segment() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_read_after_straddle_log";
    std::filesystem::remove_all(dir);

    // Capacity 2: segments hold [1,2], [3,4], [5,6]. id 3 falls inside the second segment.
    AssertionLog log(dir, 2);
    for (AssertionId id = 1; id <= 6; ++id) {
        log.append(make_assertion(id, id));
    }

    auto tail = log.read_after(3);

    assert(tail.size() == 3);
    assert(tail[0].id == 4);
    assert(tail[1].id == 5);
    assert(tail[2].id == 6);

    std::filesystem::remove_all(dir);
}

void assertion_log_record_count_hint_spans_multiple_segments() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_record_count_hint_multi_segment_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 2);
    for (AssertionId id = 1; id <= 5; ++id) {
        log.append(make_assertion(id, id));
    }

    assert(log.record_count_hint() == 5);

    std::filesystem::remove_all(dir);
}

void assertion_log_tail_checksum_mismatch_in_the_active_segment_is_tolerated() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_active_segment_tail_mismatch_log";
    std::filesystem::remove_all(dir);

    // Capacity 2: segment 0 holds [1,2] (full, non-active); segment 1 holds [3] (active).
    AssertionLog log(dir, 2);
    log.append(make_assertion(1, 1));
    log.append(make_assertion(2, 2));
    log.append(make_assertion(3, 3));

    {
        // Flip a byte inside the active segment's only record; nothing follows it anywhere,
        // so it must be silently dropped.
        std::fstream io(segment_file(dir, 1), std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    auto all = log.read_all();

    assert(all.size() == 2);
    assert(all[0].id == 1 && all[1].id == 2);

    std::filesystem::remove_all(dir);
}

void assertion_log_checksum_mismatch_in_a_non_active_segment_throws() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_non_active_segment_mismatch_log";
    std::filesystem::remove_all(dir);

    // Capacity 2: segment 0 holds [1,2] (full, non-active once segment 1 exists); segment 1 holds [3].
    AssertionLog log(dir, 2);
    log.append(make_assertion(1, 1));
    log.append(make_assertion(2, 2));
    log.append(make_assertion(3, 3));

    {
        // Flip a byte inside segment 0's second (last-in-file) record. More segments exist
        // after segment 0, so this can't be an ordinary crash artifact -- unambiguous
        // corruption, must throw even though nothing follows within segment 0's own file.
        std::fstream io(segment_file(dir, 0), std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(8 + FRAME_SIZE + sizeof(uint32_t));
        char byte = 0;
        io.read(&byte, 1);
        io.seekp(8 + FRAME_SIZE + sizeof(uint32_t));
        char flipped = static_cast<char>(~byte);
        io.write(&flipped, 1);
    }

    bool threw = false;
    try {
        log.read_all();
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove_all(dir);
}

void assertion_log_archive_segments_before_moves_only_fully_rolled_segments() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_archive_segments_log";
    std::filesystem::remove_all(dir);

    // Capacity 2: segments hold [1,2] (index 0), [3,4] (index 1), [5] (index 2, active).
    AssertionLog log(dir, 2);
    for (AssertionId id = 1; id <= 5; ++id) {
        log.append(make_assertion(id, id));
    }

    // Archiving before id 3 only reaches into segment 0's range ([1,2]): segment 0 is entirely
    // before it, segment 1 ([3,4]) is not, and the active segment (2) must never move regardless.
    log.archive_segments_before(3);

    assert(!std::filesystem::exists(segment_file(dir, 0)));
    assert(std::filesystem::exists(segment_file(dir / "archive", 0)));
    assert(std::filesystem::exists(segment_file(dir, 1)));
    assert(std::filesystem::exists(segment_file(dir, 2)));

    // Archiving again before a higher id now also reaches segment 1, but still never the active
    // segment, and is a no-op for the already-archived segment 0.
    log.archive_segments_before(5);

    assert(std::filesystem::exists(segment_file(dir / "archive", 0)));
    assert(!std::filesystem::exists(segment_file(dir, 1)));
    assert(std::filesystem::exists(segment_file(dir / "archive", 1)));
    assert(std::filesystem::exists(segment_file(dir, 2)));

    std::filesystem::remove_all(dir);
}

void assertion_log_archived_segments_remain_readable_via_read_all() {
    auto dir = std::filesystem::temp_directory_path() / "kernel_archived_segments_readable_log";
    std::filesystem::remove_all(dir);

    AssertionLog log(dir, 2);
    for (AssertionId id = 1; id <= 5; ++id) {
        log.append(make_assertion(id, id));
    }

    log.archive_segments_before(5);

    auto all = log.read_all();
    assert(all.size() == 5);
    for (AssertionId id = 1; id <= 5; ++id) {
        assert(all[id - 1].id == id);
    }

    auto tail = log.read_after(2);
    assert(tail.size() == 3);
    assert(tail[0].id == 3 && tail[1].id == 4 && tail[2].id == 5);

    assert(log.record_count_hint() == 5);

    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    assertion_log_appends_and_reads_assertions();
    assertion_log_returns_empty_when_missing();
    assertion_log_rejects_invalid_record_size();
    assertion_log_rejects_missing_or_invalid_header();
    assertion_log_recovers_partial_header_as_empty_log();
    assertion_log_recovers_tail_checksum_mismatch_as_torn_write();
    assertion_log_rejects_checksum_mismatch_when_followed_by_more_data();
    assertion_log_ignores_incomplete_trailing_record();
    assertion_log_read_after_zero_behaves_like_read_all();
    assertion_log_read_after_returns_only_records_committed_after_the_given_id();
    assertion_log_read_after_beyond_available_records_returns_empty();
    assertion_log_read_after_recovers_tail_checksum_mismatch_as_torn_write();
    assertion_log_record_count_hint_matches_appended_record_count();
    assertion_log_rolls_over_to_a_new_segment_when_capacity_is_reached();
    assertion_log_append_batch_writes_every_record_in_order();
    assertion_log_append_batch_is_a_no_op_for_an_empty_batch();
    assertion_log_append_batch_rolls_segments_and_keeps_them_exactly_full();
    assertion_log_read_all_spans_multiple_segments_in_order();
    assertion_log_read_after_skips_whole_segments_before_the_seek_point();
    assertion_log_read_after_seeks_within_the_straddling_segment();
    assertion_log_record_count_hint_spans_multiple_segments();
    assertion_log_tail_checksum_mismatch_in_the_active_segment_is_tolerated();
    assertion_log_checksum_mismatch_in_a_non_active_segment_throws();
    assertion_log_archive_segments_before_moves_only_fully_rolled_segments();
    assertion_log_archived_segments_remain_readable_via_read_all();

    std::cout << "All assertion_log tests passed.\n";
}
