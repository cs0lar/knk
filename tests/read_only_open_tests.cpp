// Phase 13: read-only opens, so an analytics process can read a live storage root.
//
// The two properties that matter are "a reader coexists with the writer" and "a reader writes
// nothing", and the second is the one worth testing paranoidly rather than trusting: several of these
// tests fingerprint every byte under the storage root before and after a reader opens, queries and
// aggregates, and require it unchanged.
//
// Two behaviors here are deliberate and were *not* what the Phase 13 plan in AGENTS.md first said:
// a reader takes no lock at all (a shared flock on the writer's own lock file could never be acquired
// while the writer holds it exclusively), and a reader meeting stale or corrupt derived state rebuilds
// it in memory and keeps serving rather than failing, because the log is the source of truth and
// repairing the files would be a write.

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/aggregate.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/mcp_tools.hpp"
#include "kernel/query.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

constexpr EntityId ALICE = 1;
constexpr EntityId ACME = 100;
constexpr EntityId BETA = 200;
constexpr PredicateId WORKS_AT = 10;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("read_only_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

bool throws(const std::function<void()> &call) {
    try {
        call();
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

// Every file under the root, by relative path, with its exact bytes -- the strongest available check
// that a reader left the store alone. Content rather than mtime, so a harmless stat cannot fail it and
// a single flipped byte cannot pass.
std::map<std::string, std::string> fingerprint(const std::filesystem::path &root) {
    std::map<std::string, std::string> files;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        std::ifstream in(entry.path(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        files[std::filesystem::relative(entry.path(), root).string()] = std::move(bytes);
    }

    return files;
}

AssertionId seed_writer(KnowledgeKernel &writer) {
    writer.commit(ALICE, WORKS_AT, ACME, 0, 1000, 100, 0.9);
    return writer.commit(ALICE, WORKS_AT, BETA, 1000, OPEN_ENDED, 200, 0.95);
}

void read_only_kernel_opens_alongside_a_live_writer() {
    auto root = test_root("opens_alongside_a_live_writer");

    KnowledgeKernel writer(StorageConfig{root});
    AssertionId latest = seed_writer(writer);

    // The whole point of the phase: this open succeeds while the writer is very much alive and holding
    // the writer lock. Before Phase 13 it threw a single-writer violation.
    KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);
    assert(reader.mode() == OpenMode::ReadOnly);
    assert(writer.mode() == OpenMode::ReadWrite);

    assert(reader.get(latest).has_value());
    assert(reader.current(ALICE).size() == 1);
    assert(reader.current(ALICE)[0].object == BETA);

    // The writer is unaffected and keeps committing with the reader attached.
    AssertionId after = writer.commit(BETA, WORKS_AT, ACME, 0, OPEN_ENDED, 300, 0.8);
    assert(writer.get(after).has_value());

    // A second *writer* is still refused: single-writer enforcement is unchanged, which is the
    // invariant that had to survive this phase.
    assert(throws([&] { KnowledgeKernel second_writer(StorageConfig{root}); }));

    cleanup(root);
}

void read_only_kernel_is_a_snapshot_as_of_its_own_open() {
    auto root = test_root("snapshot_as_of_open");

    KnowledgeKernel writer(StorageConfig{root});
    seed_writer(writer);

    KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);
    size_t seen_at_open = reader.changes_since(0).size();

    writer.commit(BETA, WORKS_AT, ACME, 0, OPEN_ENDED, 300, 0.8);
    writer.commit(BETA, WORKS_AT, BETA, 0, OPEN_ENDED, 400, 0.8);

    // Replay happens once, in the constructor, so a read-only kernel does not grow a live view of the
    // log. This is a real limitation rather than an oversight, and it is what the reopen below is for.
    assert(reader.changes_since(0).size() == seen_at_open);

    KnowledgeKernel reopened(StorageConfig{root}, OpenMode::ReadOnly);
    assert(reopened.changes_since(0).size() == seen_at_open + 2);

    cleanup(root);
}

void read_only_kernel_rejects_every_mutating_method() {
    auto root = test_root("rejects_every_mutating_method");

    AssertionId existing = 0;
    {
        KnowledgeKernel writer(StorageConfig{root});
        existing = seed_writer(writer);
    }

    KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);

    assert(throws([&] { reader.commit(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 0, 1.0); }));
    assert(throws([&] { reader.commit_by_name("Alice", "works_at", Value::of_text("Acme"), 0, OPEN_ENDED, 0, 1.0); }));
    assert(throws([&] { reader.commit_batch({{ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 0, 1.0}}); }));
    assert(throws(
        [&] { reader.commit_batch_by_name({{"Alice", "works_at", Value::of_text("Acme"), 0, OPEN_ENDED, 0, 1.0}}); }));
    assert(throws([&] { reader.commit_retraction(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 0, 1.0, existing); }));
    assert(throws([&] { reader.commit_superseding(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 0, 1.0, existing); }));
    assert(throws([&] { reader.commit_hypothesis(ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 0, 1.0, ALICE, 0, "guess"); }));
    assert(throws([&] { reader.write_snapshot(); }));
    assert(throws([&] { reader.intern_entity("Nobody"); }));
    assert(throws([&] { reader.intern_value(Value::of_int64(7)); }));
    assert(throws([&] { reader.intern_predicate("nothing"); }));
    assert(throws([&] {
        std::vector<std::byte> content{std::byte{'x'}};
        reader.intern_document(content);
    }));
    assert(throws([&] { reader.record_provenance(existing, ALICE, 0, "manual"); }));
    assert(throws([&] { reader.record_provenance_batch({{existing, ALICE, 0, "manual"}}); }));
    assert(throws([&] { reader.merge_entities(ALICE, BETA, 0); }));
    assert(throws([&] { reader.archive_segments_before(existing); }));

    // The command layer is no back door: execute() dispatches to the same guarded methods.
    assert(throws([&] { reader.execute(CommitCommand{ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 0, 1.0}); }));

    // Reads still work after all those rejections -- nothing was left half-mutated.
    assert(reader.current(ALICE).size() == 1);

    cleanup(root);
}

void read_only_open_leaves_every_byte_untouched() {
    auto root = test_root("leaves_every_byte_untouched");

    {
        KnowledgeKernel writer(StorageConfig{root});
        seed_writer(writer);
        writer.commit_by_name("Carol", "salary", Value::of_int64(150000), 0, OPEN_ENDED, 300, 0.9);
        writer.write_snapshot(); // so a snapshot file exists to be left alone too
    }

    auto before = fingerprint(root);
    assert(!before.empty());

    {
        KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);

        // Exercise the read surface the phases before this one added, since any of them could in
        // principle touch disk: row queries, filters, name resolution, and an aggregate.
        Query query;
        query.statuses = {AssertionStatus::Active};
        query.resolve_names = true;
        assert(!reader.query(query).assertions.empty());

        Query filtered;
        filtered.filter = Filter::compare(FilterField::Confidence, CompareOp::Gte, Value::of_double(0.9));
        assert(!reader.query(filtered).assertions.empty());

        AggregateQuery aggregate;
        aggregate.group_by = {{GroupField::Predicate, 0}};
        aggregate.aggregations = {{AggregateFunction::Count, AggregateTarget::ObjectValue},
                                  {AggregateFunction::Avg, AggregateTarget::ObjectValue}};
        assert(!reader.aggregate(aggregate).groups.empty());

        assert(!reader.changes_since(0).empty());
        assert(reader.entity_name_batch({ALICE}).size() == 1);
    }

    auto after = fingerprint(root);

    // Byte-for-byte, and the same set of files: a reader adds nothing either.
    assert(before == after);

    cleanup(root);
}

void read_only_open_does_not_heal_stale_derived_state() {
    auto root = test_root("does_not_heal_stale_derived_state");

    {
        KnowledgeKernel writer(StorageConfig{root});
        seed_writer(writer);
    }

    StorageConfig config{root};

    // Deleting the checkpoint is exactly the "indexes cannot be trusted" trigger the recovery path
    // uses, and it is the state a reader will genuinely meet while a writer is mid-commit.
    std::filesystem::remove(config.checkpoint_path());
    assert(!std::filesystem::exists(config.checkpoint_path()));

    auto before = fingerprint(root);

    {
        KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);

        // Correct answers, from the log, without healing anything: the rebuild happened in memory.
        assert(reader.current(ALICE).size() == 1);
        assert(reader.current(ALICE)[0].object == BETA);
        assert(reader.assertions_for_subject(ALICE).size() == 2);
    }

    assert(fingerprint(root) == before);
    assert(!std::filesystem::exists(config.checkpoint_path())); // still not healed

    // A writer, by contrast, repairs it -- which is how a root in this state gets fixed.
    {
        KnowledgeKernel writer(StorageConfig{root});
        assert(writer.current(ALICE).size() == 1);
    }
    assert(std::filesystem::exists(config.checkpoint_path()));

    cleanup(root);
}

void read_only_open_tolerates_a_corrupt_index_without_repairing_it() {
    auto root = test_root("tolerates_a_corrupt_index");

    {
        KnowledgeKernel writer(StorageConfig{root});
        seed_writer(writer);
    }

    StorageConfig config{root};

    // A valid header followed by a bad record size: corruption that makes the index log throw on load,
    // rather than a merely stale one.
    {
        std::ofstream out(config.subject_index_path(), std::ios::binary | std::ios::trunc);
        out.write("KNK1", 4);
        uint32_t version = 1;
        out.write(reinterpret_cast<const char *>(&version), sizeof(version));
        uint32_t bad_record_size = 1;
        out.write(reinterpret_cast<const char *>(&bad_record_size), sizeof(bad_record_size));
    }

    auto before = fingerprint(root);

    {
        // Deliberately not a hard failure: the log is the source of truth, so a reader that can answer
        // correctly should, and the alternative would take an analytics reader offline for a problem
        // only a writer can fix.
        KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);
        assert(reader.assertions_for_subject(ALICE).size() == 2);
        assert(reader.current(ALICE).size() == 1);
    }

    // The corrupt file is left exactly as found -- self-heal is a write.
    assert(fingerprint(root) == before);

    cleanup(root);
}

void read_only_open_drops_a_torn_trailing_record_like_recovery_does() {
    auto root = test_root("drops_a_torn_trailing_record");

    StorageConfig config{root};

    {
        KnowledgeKernel writer(StorageConfig{root});
        seed_writer(writer);
    }

    // Simulate a writer caught mid-append: a record-size prefix followed by half a payload, which is
    // what a reader can see of a commit that has not finished. A reader sees the committed prefix and
    // nothing torn.
    {
        std::ofstream out(config.segment_path(0), std::ios::binary | std::ios::app);
        uint32_t record_size = static_cast<uint32_t>(sizeof(Assertion));
        out.write(reinterpret_cast<const char *>(&record_size), sizeof(record_size));
        std::vector<char> half(sizeof(Assertion) / 2, 0);
        out.write(half.data(), static_cast<std::streamsize>(half.size()));
    }

    KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);
    assert(reader.changes_since(0).size() == 2); // the two complete records, not a third
    assert(reader.current(ALICE).size() == 1);

    cleanup(root);
}

void read_only_open_of_a_missing_root_throws_and_creates_nothing() {
    auto root = test_root("missing_root");
    assert(!std::filesystem::exists(root));

    assert(throws([&] { KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly); }));

    // Not created as a side effect: a reader that conjures the layout into existence has written.
    assert(!std::filesystem::exists(root));

    // A read-write open does create it, which is the pre-existing behavior.
    {
        KnowledgeKernel writer(StorageConfig{root});
    }
    assert(std::filesystem::exists(root));

    cleanup(root);
}

void many_read_only_kernels_coexist() {
    auto root = test_root("many_readers_coexist");

    KnowledgeKernel writer(StorageConfig{root});
    seed_writer(writer);

    KnowledgeKernel first(StorageConfig{root}, OpenMode::ReadOnly);
    KnowledgeKernel second(StorageConfig{root}, OpenMode::ReadOnly);
    KnowledgeKernel third(StorageConfig{root}, OpenMode::ReadOnly);

    assert(first.current(ALICE).size() == 1);
    assert(second.current(ALICE).size() == 1);
    assert(third.current(ALICE).size() == 1);

    // And the writer is still writing throughout.
    assert(writer.commit(BETA, WORKS_AT, ACME, 0, OPEN_ENDED, 500, 0.7) != 0);

    cleanup(root);
}

void read_only_mcp_tools_report_writes_as_tool_errors() {
    auto root = test_root("mcp_tools_report_writes_as_errors");

    {
        KnowledgeKernel writer(StorageConfig{root});
        seed_writer(writer);
    }

    KnowledgeKernel reader(StorageConfig{root}, OpenMode::ReadOnly);

    // Over MCP a refused write is an ordinary tool error, not a crashed server: handle_tool_call already
    // turns an exception from execute() into is_error, which is exactly the right behavior here.
    auto commit = mcp::handle_tool_call(reader, "commit",
                                        nlohmann::json{{"subject", ALICE},
                                                       {"predicate", WORKS_AT},
                                                       {"object", ACME},
                                                       {"valid_from", 0},
                                                       {"valid_to", 0},
                                                       {"observed_at", 0},
                                                       {"confidence", 0.9}});
    assert(commit.is_error);
    assert(commit.content_text.find("read-only") != std::string::npos);

    assert(mcp::handle_tool_call(reader, "intern_entity", nlohmann::json{{"name", "X"}}).is_error);
    assert(mcp::handle_tool_call(reader, "write_snapshot", nlohmann::json::object()).is_error);

    // Reads over the same surface still work.
    auto query = mcp::handle_tool_call(reader, "query", nlohmann::json{{"subject", ALICE}});
    assert(!query.is_error);
    assert(nlohmann::json::parse(query.content_text).at("assertions").size() == 2);

    auto aggregate =
        mcp::handle_tool_call(reader, "aggregate", nlohmann::json{{"aggregations", {{{"function", "count"}}}}});
    assert(!aggregate.is_error);
    assert(nlohmann::json::parse(aggregate.content_text).at("groups")[0].at("values")[0] == 2);

    cleanup(root);
}

void storage_engine_read_only_refuses_writes_directly() {
    auto root = test_root("storage_engine_refuses_writes");

    {
        KnowledgeKernel writer(StorageConfig{root});
        seed_writer(writer);
    }

    // The layer that owns the files refuses too, not just the kernel above it -- so a future direct
    // caller cannot write through a read-only engine by bypassing KnowledgeKernel.
    StorageEngine engine(StorageConfig{root}, OpenMode::ReadOnly);
    assert(engine.mode() == OpenMode::ReadOnly);

    Assertion assertion{99, ALICE, WORKS_AT, ACME, 0, OPEN_ENDED, 0, 1.0, AssertionStatus::Active};
    assert(throws([&] { engine.append_assertion(assertion); }));
    assert(throws([&] { engine.write_checkpoint(1); }));
    assert(throws([&] { engine.write_snapshot(1, {assertion}); }));
    assert(throws([&] { engine.append_subject_entry(ALICE, 1); }));
    assert(throws([&] { engine.rewrite_subject_index({}); }));
    assert(throws([&] { engine.append_entity_catalog_entry(1, Value::of_text("x")); }));
    assert(throws([&] { engine.archive_segments_before(1); }));

    // Loads still work.
    assert(engine.load_assertions().size() == 2);

    cleanup(root);
}

} // namespace

int main() {
    read_only_kernel_opens_alongside_a_live_writer();
    read_only_kernel_is_a_snapshot_as_of_its_own_open();
    read_only_kernel_rejects_every_mutating_method();
    read_only_open_leaves_every_byte_untouched();
    read_only_open_does_not_heal_stale_derived_state();
    read_only_open_tolerates_a_corrupt_index_without_repairing_it();
    read_only_open_drops_a_torn_trailing_record_like_recovery_does();
    read_only_open_of_a_missing_root_throws_and_creates_nothing();
    many_read_only_kernels_coexist();
    read_only_mcp_tools_report_writes_as_tool_errors();
    storage_engine_read_only_refuses_writes_directly();

    std::cout << "All read_only_open tests passed.\n";
    return 0;
}
