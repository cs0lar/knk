// Phase 14: the columnar projection store.
//
// Nothing queries columns yet -- that is Phase 15 -- so what has to be nailed down here is that the
// store is *maintained correctly*: every commit path keeps it in lockstep with the assertion log, a
// store that cannot be trusted is rebuilt rather than believed, and a store that merely lags gets the
// missing tail. The strongest tests compare the columns field-by-field against the log they are derived
// from, since that is the only definition of correct they have.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "kernel/assertion_log.hpp"
#include "kernel/column_store.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/storage_config.hpp"
#include "kernel/storage_engine.hpp"

using namespace knk;

namespace {

constexpr EntityId ALICE = 1;
constexpr EntityId BOB = 2;
constexpr EntityId ACME = 100;
constexpr EntityId BETA = 200;
constexpr PredicateId WORKS_AT = 10;
constexpr PredicateId LIVES_IN = 20;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("column_store_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

Assertion make_assertion(AssertionId id, EntityId subject, AssertionStatus status = AssertionStatus::Active) {
    return Assertion{id,
                     subject,
                     WORKS_AT,
                     ACME,
                     static_cast<Timestamp>(id) * 10,
                     OPEN_ENDED,
                     static_cast<Timestamp>(id) * 100,
                     0.5 + static_cast<double>(id) / 100.0,
                     status};
}

// The only definition of "the columns are right": they agree, field by field, with the assertions the
// log holds. Used through the public kernel/engine surface, so it also covers the lockstep maintenance.
void assert_columns_match(const std::filesystem::path &root, const std::vector<Assertion> &expected) {
    StorageEngine engine(StorageConfig{root});

    assert(engine.verify_columns());
    assert(engine.column_row_count() == expected.size());

    auto columns = engine.map_columns();
    if (expected.empty()) {
        assert(columns.empty());
        return;
    }

    assert(columns.subject.size() == expected.size());

    for (size_t i = 0; i < expected.size(); ++i) {
        assert(columns.subject[i] == expected[i].subject);
        assert(columns.predicate[i] == expected[i].predicate);
        assert(columns.object[i] == expected[i].object);
        assert(columns.valid_from[i] == expected[i].valid_from);
        assert(columns.valid_to[i] == expected[i].valid_to);
        assert(columns.observed_at[i] == expected[i].observed_at);
        assert(columns.confidence[i] == expected[i].confidence);
        assert(columns.status[i] == static_cast<uint8_t>(expected[i].status));
        assert(columns.supersedes_id[i] == expected[i].supersedes_id);
        assert(columns.retracts_id[i] == expected[i].retracts_id);
    }
}

// The log's raw records, which is what the columns project. Deliberately *not* the kernel's replayed
// view: a superseded row's record still says Active, because append-only storage never rewrote it, and an
// earlier version of this helper compared against replayed state and failed for exactly that reason.
std::vector<Assertion> log_records(const std::filesystem::path &root) {
    StorageEngine engine(StorageConfig{root}, OpenMode::ReadOnly);
    return engine.load_assertions();
}

bool throws(const std::function<void()> &call) {
    try {
        call();
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

// --- ColumnStore on its own -------------------------------------------------------

void column_store_round_trips_every_field() {
    auto root = test_root("round_trips_every_field");
    std::filesystem::create_directories(root);

    std::vector<Assertion> rows{make_assertion(1, ALICE), make_assertion(2, BOB, AssertionStatus::Superseded),
                                make_assertion(3, ALICE, AssertionStatus::Hypothesis)};

    {
        ColumnStore store(root / "columns");
        store.append(rows);
        assert(store.row_count() == 3);
        assert(store.verify());

        auto columns = store.map();
        assert(columns.subject.size() == 3);
        assert(columns.subject[1] == BOB);
        assert(columns.valid_from[2] == 30);
        assert(columns.observed_at[0] == 100);
        assert(columns.status[1] == static_cast<uint8_t>(AssertionStatus::Superseded));
        assert(columns.status[2] == static_cast<uint8_t>(AssertionStatus::Hypothesis));
        assert(columns.confidence[0] == rows[0].confidence);
    }

    // Reopening reads the manifest rather than the columns, so the row count survives without a scan.
    ColumnStore reopened(root / "columns");
    assert(reopened.row_count() == 3);
    assert(reopened.verify());

    cleanup(root);
}

void column_store_appends_incrementally_with_the_same_checksums() {
    auto root = test_root("appends_incrementally");
    std::filesystem::create_directories(root);

    std::vector<Assertion> rows{make_assertion(1, ALICE), make_assertion(2, BOB), make_assertion(3, ALICE)};

    {
        ColumnStore one_shot(root / "one_shot");
        one_shot.append(rows);
    }

    {
        ColumnStore piecemeal(root / "piecemeal");
        for (const auto &row : rows) {
            piecemeal.append(std::span<const Assertion>(&row, 1));
        }
        assert(piecemeal.row_count() == 3);
        assert(piecemeal.verify());
    }

    // The incremental checksum has to produce exactly what a single pass would, so three appends of one
    // row must leave byte-identical files to one append of three. This is what makes an append O(appended)
    // rather than O(store) without weakening the integrity check.
    for (const char *name : {"manifest", "subject.col", "confidence.col", "status.col"}) {
        std::ifstream a(root / "one_shot" / name, std::ios::binary);
        std::ifstream b(root / "piecemeal" / name, std::ios::binary);
        std::string bytes_a((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
        std::string bytes_b((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
        assert(!bytes_a.empty());
        assert(bytes_a == bytes_b);
    }

    cleanup(root);
}

void column_store_detects_every_kind_of_damage() {
    auto root = test_root("detects_damage");

    auto fresh = [&root](const char *name) {
        auto directory = root / name;
        std::filesystem::remove_all(directory);
        ColumnStore store(directory);
        store.append(std::vector<Assertion>{make_assertion(1, ALICE), make_assertion(2, BOB)});
        return directory;
    };

    // A flipped payload byte: the case that fixed-stride columns cannot catch per row, and the reason the
    // manifest carries a checksum per column at all.
    {
        auto directory = fresh("flipped_byte");
        {
            std::fstream io(directory / "subject.col", std::ios::binary | std::ios::in | std::ios::out);
            io.seekp(16); // past the header, into the first element
            char byte = 0;
            io.read(&byte, 1);
            io.seekp(16);
            char flipped = static_cast<char>(~byte);
            io.write(&flipped, 1);
        }
        ColumnStore store(directory);
        assert(!store.verify());
    }

    // A truncated column: caught by the size check before any payload is read.
    {
        auto directory = fresh("truncated");
        std::filesystem::resize_file(directory / "object.col", 16 + sizeof(EntityId));
        ColumnStore store(directory);
        assert(!store.verify());
    }

    // A torn append, leaving a partial element.
    {
        auto directory = fresh("partial_element");
        auto path = directory / "valid_from.col";
        std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3);
        ColumnStore store(directory);
        assert(!store.verify());
    }

    // No manifest: the columns may be perfect, but nothing says how many rows they should hold.
    {
        auto directory = fresh("no_manifest");
        std::filesystem::remove(directory / "manifest");
        ColumnStore store(directory);
        assert(store.row_count() == 0);
        assert(!store.verify());
    }

    // A scribbled manifest is rejected by its own checksum rather than read as a row count.
    {
        auto directory = fresh("bad_manifest");
        {
            std::fstream io(directory / "manifest", std::ios::binary | std::ios::in | std::ios::out);
            io.seekp(8);
            uint64_t lie = 9999;
            io.write(reinterpret_cast<const char *>(&lie), sizeof(lie));
        }
        ColumnStore store(directory);
        assert(store.row_count() == 0);
        assert(!store.verify());
    }

    // A missing column file entirely.
    {
        auto directory = fresh("missing_column");
        std::filesystem::remove(directory / "status.col");
        ColumnStore store(directory);
        assert(!store.verify());
    }

    cleanup(root);
}

void column_store_overwrite_all_replaces_and_empty_is_a_known_empty() {
    auto root = test_root("overwrite_all");
    std::filesystem::create_directories(root);

    ColumnStore store(root / "columns");
    store.append(std::vector<Assertion>{make_assertion(1, ALICE), make_assertion(2, BOB), make_assertion(3, ALICE)});
    assert(store.row_count() == 3);

    store.overwrite_all(std::vector<Assertion>{make_assertion(1, BOB)});
    assert(store.row_count() == 1);
    assert(store.verify());
    assert(store.map().subject[0] == BOB);

    // An empty store is a *known* empty store, not an absent one: verify passes and row_count is 0, so a
    // caller can tell "rebuilt, nothing in the log" from "never built".
    store.overwrite_all({});
    assert(store.row_count() == 0);
    assert(store.verify());
    assert(store.map().empty());

    cleanup(root);
}

void column_store_move_transfers_ownership() {
    auto root = test_root("move_transfers_ownership");
    std::filesystem::create_directories(root);

    ColumnStore source(root / "columns");
    source.append(std::vector<Assertion>{make_assertion(1, ALICE), make_assertion(2, BOB)});
    auto mapped = source.map();
    assert(mapped.subject.size() == 2);

    // Movable specifically so KnowledgeKernel stays movable (it is returned by value in the benchmarks);
    // exactly one of the two objects may own the mappings afterwards.
    ColumnStore moved(std::move(source));
    assert(moved.row_count() == 2);
    assert(moved.verify());
    assert(moved.map().subject[1] == BOB);

    cleanup(root);
}

// --- maintenance through the kernel ----------------------------------------------

void every_commit_path_keeps_the_columns_in_lockstep() {
    auto root = test_root("every_commit_path_in_lockstep");

    {
        KnowledgeKernel kernel(StorageConfig{root});

        AssertionId first = kernel.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 100, 0.9);
        kernel.commit_superseding(ALICE, WORKS_AT, BETA, 0, OPEN_ENDED, 200, 0.95, first);
        AssertionId retractable = kernel.commit(BOB, LIVES_IN, ACME, 0, OPEN_ENDED, 300, 0.8);
        kernel.commit_retraction(BOB, LIVES_IN, ACME, 0, OPEN_ENDED, 400, 1.0, retractable);
        kernel.commit_hypothesis(ALICE, LIVES_IN, BETA, 0, OPEN_ENDED, 500, 0.5, ALICE, 500, "guess");

        // Batch commits go through the same StorageEngine entry point, which is why they need no separate
        // bookkeeping -- but that is exactly the kind of claim worth testing rather than asserting.
        kernel.commit_batch({{BOB, WORKS_AT, ACME, 0, OPEN_ENDED, 600, 0.7},
                             {BOB, WORKS_AT, BETA, 0, OPEN_ENDED, 700, 0.6},
                             {ALICE, LIVES_IN, ACME, 0, OPEN_ENDED, 800, 0.55}});
        kernel.commit_batch_by_name({{"Carol", "salary", Value::of_int64(150000), 0, OPEN_ENDED, 900, 0.9}});
    }

    auto expected = log_records(root);
    assert(expected.size() == 9); // 5 singles (one of them a retraction record) + 3 + 1 batched
    assert_columns_match(root, expected);

    cleanup(root);
}

void the_status_column_holds_the_appended_status_not_the_effective_one() {
    auto root = test_root("status_column_is_the_appended_status");

    AssertionId first = 0;
    AssertionId second = 0;
    {
        KnowledgeKernel kernel(StorageConfig{root});
        first = kernel.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 100, 0.9);
        second = kernel.commit_superseding(ALICE, WORKS_AT, BETA, 0, OPEN_ENDED, 200, 0.95, first);

        // The kernel reports the *effective* status, derived by replay.
        assert(kernel.get(first)->status == AssertionStatus::Superseded);
        assert(kernel.get(second)->status == AssertionStatus::Active);
    }

    StorageEngine engine(StorageConfig{root}, OpenMode::ReadOnly);
    auto columns = engine.map_columns();
    assert(columns.subject.size() == 2);

    // The column says what was appended, because append-only storage never rewrote the row: Active.
    // A Phase 15 scan filtering on this byte alone would wrongly call row 1 current -- which is why the
    // link columns are part of the projection.
    assert(columns.status[0] == static_cast<uint8_t>(AssertionStatus::Active));
    assert(columns.status[1] == static_cast<uint8_t>(AssertionStatus::Active));

    // Effective status is derivable from the columns alone: row 1 is superseded precisely because a later
    // row points at it. This is the derivation replay performs and the one Phase 19 will restrict by id.
    assert(columns.supersedes_id[0] == 0);
    assert(columns.supersedes_id[1] == first);
    assert(columns.retracts_id[0] == 0);
    assert(columns.retracts_id[1] == 0);

    cleanup(root);
}

void columns_survive_a_restart_and_keep_matching() {
    auto root = test_root("survive_a_restart");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 100, 0.9);
    }

    {
        // A second session appends to an existing store rather than rebuilding it.
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(BOB, WORKS_AT, BETA, 0, OPEN_ENDED, 200, 0.8);
    }

    assert_columns_match(root, log_records(root));

    cleanup(root);
}

void a_corrupt_column_is_rebuilt_on_the_next_read_write_open() {
    auto root = test_root("corrupt_column_is_rebuilt");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 100, 0.9);
        kernel.commit(BOB, WORKS_AT, BETA, 0, OPEN_ENDED, 200, 0.8);
    }

    StorageConfig config{root};
    auto expected = log_records(root);

    // Damage a column, then open read-write: derived state gets the Phase 3 index treatment -- discard and
    // rebuild, never fatal.
    {
        std::fstream io(config.column_directory() / "subject.col", std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(16);
        uint64_t nonsense = 123456789;
        io.write(reinterpret_cast<const char *>(&nonsense), sizeof(nonsense));
    }

    {
        ColumnStore damaged(config.column_directory());
        assert(!damaged.verify());
    }

    {
        KnowledgeKernel healed(StorageConfig{root}); // opening is enough; no explicit repair call
        assert(healed.current(ALICE).size() == 1);
    }

    assert_columns_match(root, expected);

    cleanup(root);
}

void columns_that_lag_the_log_get_only_the_missing_tail() {
    auto root = test_root("lagging_columns_get_the_tail");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 100, 0.9);
    }

    StorageConfig config{root};

    // Simulate a crash between the assertion-log append and the column append: add a record to the log
    // directly, leaving the columns one row behind -- the state the ordering in append_assertion makes
    // possible on purpose.
    {
        AssertionLog log(config.segment_directory(), config.max_records_per_segment);
        log.append(Assertion{2, BOB, WORKS_AT, BETA, 0, OPEN_ENDED, 200, 0.8, AssertionStatus::Active});
    }

    {
        ColumnStore lagging(config.column_directory());
        assert(lagging.row_count() == 1);
        assert(lagging.verify()); // not damaged, just behind
    }

    {
        KnowledgeKernel kernel(StorageConfig{root});
        assert(kernel.get(2).has_value());
    }

    assert_columns_match(root, log_records(root));

    cleanup(root);
}

void a_root_without_columns_is_migrated_on_open() {
    auto root = test_root("root_without_columns_is_migrated");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 100, 0.9);
        kernel.commit(BOB, WORKS_AT, BETA, 0, OPEN_ENDED, 200, 0.8);
    }

    StorageConfig config{root};
    auto expected = log_records(root);

    // A root written before this phase existed has no columns/ at all. Opening it read-write builds them
    // from the log -- a one-time O(log) migration, not an error.
    std::filesystem::remove_all(config.column_directory());
    assert(!std::filesystem::exists(config.column_directory()));

    {
        KnowledgeKernel migrated(StorageConfig{root});
        assert(migrated.current(ALICE).size() == 1);
    }

    assert(std::filesystem::exists(config.column_directory()));
    assert_columns_match(root, expected);

    cleanup(root);
}

void a_read_only_open_neither_builds_nor_repairs_columns() {
    auto root = test_root("read_only_neither_builds_nor_repairs");

    {
        KnowledgeKernel kernel(StorageConfig{root});
        kernel.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 100, 0.9);
    }

    StorageConfig config{root};

    // Columns removed entirely: a read-only open must still serve queries (the log is the source of
    // truth) and must not build what it found missing, because building is a write.
    std::filesystem::remove_all(config.column_directory());

    {
        KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);
        assert(reader.current(ALICE).size() == 1);
    }

    assert(!std::filesystem::exists(config.column_directory()));

    // And a read-only StorageEngine refuses the write paths that maintain them.
    StorageEngine engine(StorageConfig{root}, OpenMode::ReadOnly);
    assert(engine.column_row_count() == 0);
    assert(engine.map_columns().empty());
    Assertion assertion = make_assertion(99, ALICE);
    assert(throws([&] { engine.append_assertion(assertion); }));

    cleanup(root);
}

} // namespace

int main() {
    column_store_round_trips_every_field();
    column_store_appends_incrementally_with_the_same_checksums();
    column_store_detects_every_kind_of_damage();
    column_store_overwrite_all_replaces_and_empty_is_a_known_empty();
    column_store_move_transfers_ownership();
    every_commit_path_keeps_the_columns_in_lockstep();
    the_status_column_holds_the_appended_status_not_the_effective_one();
    columns_survive_a_restart_and_keep_matching();
    a_corrupt_column_is_rebuilt_on_the_next_read_write_open();
    columns_that_lag_the_log_get_only_the_missing_tail();
    a_root_without_columns_is_migrated_on_open();
    a_read_only_open_neither_builds_nor_repairs_columns();

    std::cout << "All column_store tests passed.\n";
    return 0;
}
