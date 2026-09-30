// Phase 17: spilling a query result to columns on disk.
//
// The format's whole claim is that it is trivial to read, so these tests read it the way a consumer would
// -- open a file, interpret fixed-width native-endian values, consult the descriptor -- and compare
// against what the same query returns in memory. Nothing here uses the writer's own code to check the
// writer, which is what makes the round trip meaningful.

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "kernel/knowledge_kernel.hpp"
#include "kernel/spill.hpp"
#include "kernel/storage_config.hpp"

#include <nlohmann/json.hpp>

using namespace knk;

namespace {

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("spill_" + name);
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

// A consumer's reader, deliberately naive: read the whole file, walk it in fixed-width steps. If this
// needed anything cleverer, the format would have failed at its one job.
template <typename T> std::vector<T> read_column(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    assert(bytes.size() % sizeof(T) == 0);

    std::vector<T> values(bytes.size() / sizeof(T));
    if (!values.empty()) {
        std::memcpy(values.data(), bytes.data(), bytes.size());
    }
    return values;
}

nlohmann::json read_json(const std::filesystem::path &path) {
    std::ifstream in(path);
    nlohmann::json json;
    in >> json;
    return json;
}

struct Fixture {
    EntityId alice;
    EntityId bob;
    EntityId eng;
    PredicateId salary;
    PredicateId dept;
};

Fixture seed(KnowledgeKernel &kernel) {
    Fixture f;
    f.alice = kernel.intern_entity("Alice");
    f.bob = kernel.intern_entity("Bob");
    f.eng = kernel.intern_entity("eng");
    f.salary = kernel.intern_predicate("salary");
    f.dept = kernel.intern_predicate("dept");

    kernel.commit(f.alice, f.salary, kernel.intern_value(Value::of_int64(120000)), 0, OPEN_ENDED, 100, 0.95);
    kernel.commit(f.bob, f.salary, kernel.intern_value(Value::of_int64(90000)), 0, OPEN_ENDED, 200, 0.60);
    kernel.commit(f.alice, f.dept, f.eng, 0, OPEN_ENDED, 100, 1.0);

    return f;
}

void spill_columns_match_the_query_row_for_row() {
    auto root = test_root("columns_match_the_query");
    auto spills = root / "spills";

    KnowledgeKernel kernel(StorageConfig{root / "store"});
    seed(kernel);

    Query query;
    query.order = QueryOrder::ObservedAt;

    auto expected = kernel.query(query).assertions;
    auto descriptor = kernel.spill_query(query, spills);

    assert(descriptor.rows == expected.size());
    assert(descriptor.columns.size() == 11);

    auto ids = read_column<AssertionId>(descriptor.directory / "id.col");
    auto subjects = read_column<EntityId>(descriptor.directory / "subject.col");
    auto predicates = read_column<PredicateId>(descriptor.directory / "predicate.col");
    auto objects = read_column<EntityId>(descriptor.directory / "object.col");
    auto valid_from = read_column<Timestamp>(descriptor.directory / "valid_from.col");
    auto valid_to = read_column<Timestamp>(descriptor.directory / "valid_to.col");
    auto observed_at = read_column<Timestamp>(descriptor.directory / "observed_at.col");
    auto confidence = read_column<double>(descriptor.directory / "confidence.col");
    auto status = read_column<uint8_t>(descriptor.directory / "status.col");
    auto supersedes = read_column<AssertionId>(descriptor.directory / "supersedes_id.col");
    auto retracts = read_column<AssertionId>(descriptor.directory / "retracts_id.col");

    assert(ids.size() == expected.size());

    for (size_t i = 0; i < expected.size(); ++i) {
        assert(ids[i] == expected[i].id);
        assert(subjects[i] == expected[i].subject);
        assert(predicates[i] == expected[i].predicate);
        assert(objects[i] == expected[i].object);
        assert(valid_from[i] == expected[i].valid_from);
        assert(valid_to[i] == expected[i].valid_to);
        assert(observed_at[i] == expected[i].observed_at);
        assert(confidence[i] == expected[i].confidence);
        assert(status[i] == static_cast<uint8_t>(expected[i].status));
        assert(supersedes[i] == expected[i].supersedes_id);
        assert(retracts[i] == expected[i].retracts_id);
    }

    // The descriptor on disk says the same thing the call returned, so a consumer can work from either.
    auto on_disk = read_json(descriptor.directory / "descriptor.json");
    assert(on_disk.at("format") == "knk-columnar-result");
    assert(on_disk.at("rows") == descriptor.rows);
    assert(on_disk.at("token") == descriptor.token);
    assert(on_disk.at("columns").size() == descriptor.columns.size());
    assert(on_disk.at("byte_order") == "little");

    cleanup(root);
}

void the_spilled_status_is_the_effective_one() {
    auto root = test_root("spilled_status_is_effective");
    auto spills = root / "spills";

    KnowledgeKernel kernel(StorageConfig{root / "store"});
    auto f = seed(kernel);

    AssertionId superseded = kernel.commit(f.bob, f.dept, f.eng, 0, OPEN_ENDED, 300, 0.5);
    kernel.commit_superseding(f.bob, f.dept, f.alice, 0, OPEN_ENDED, 400, 0.9, superseded);

    Query only_superseded;
    only_superseded.statuses = {AssertionStatus::Superseded};

    auto descriptor = kernel.spill_query(only_superseded, spills);
    assert(descriptor.rows == 1);

    // A spill is a query result, so it must say what a query says -- Superseded. The columnar *store*
    // holds the appended status for the same row (still Active), which is a different thing on purpose;
    // see column_store.hpp.
    auto status = read_column<uint8_t>(descriptor.directory / "status.col");
    auto ids = read_column<AssertionId>(descriptor.directory / "id.col");
    assert(ids[0] == superseded);
    assert(status[0] == static_cast<uint8_t>(AssertionStatus::Superseded));

    cleanup(root);
}

void the_dictionary_covers_the_ids_present_in_every_value_kind() {
    auto root = test_root("dictionary_covers_every_kind");
    auto spills = root / "spills";

    KnowledgeKernel kernel(StorageConfig{root / "store"});
    EntityId subject = kernel.intern_entity("Subject");
    PredicateId predicate = kernel.intern_predicate("has");

    // One object of each Value kind, so the dictionary's coverage of the tagged union is exercised rather
    // than assumed.
    std::vector<Value> values{Value::of_text("text-object"), Value::of_int64(-42), Value::of_double(2.5),
                              Value::of_bool(true), Value::of_timestamp(1719792000)};

    for (const auto &value : values) {
        kernel.commit(subject, predicate, kernel.intern_value(value), 0, OPEN_ENDED, 0, 1.0);
    }

    auto descriptor = kernel.spill_query(Query{}, spills);
    assert(descriptor.rows == values.size());

    auto dictionary = read_json(descriptor.directory / descriptor.dictionary_file);
    const auto &entities = dictionary.at("entities");

    // The subject and every object appear, each as a tagged value a consumer can interpret without knk.
    assert(entities.contains(std::to_string(subject)));
    assert(entities.at(std::to_string(subject)).at("kind") == "text");
    assert(entities.at(std::to_string(subject)).at("value") == "Subject");

    std::map<std::string, bool> kinds_seen;
    for (const auto &[id, entry] : entities.items()) {
        kinds_seen[entry.at("kind").get<std::string>()] = true;
    }
    for (const char *kind : {"text", "int64", "double", "bool", "timestamp"}) {
        assert(kinds_seen[kind]);
    }

    assert(dictionary.at("predicates").at(std::to_string(predicate)) == "has");

    // O(distinct), not O(rows): only what the result refers to.
    assert(entities.size() == values.size() + 1); // the five objects plus the subject

    cleanup(root);
}

void tokens_are_generated_without_collisions_and_never_overwritten() {
    auto root = test_root("tokens_never_overwritten");
    auto spills = root / "spills";

    KnowledgeKernel kernel(StorageConfig{root / "store"});
    seed(kernel);

    auto first = kernel.spill_query(Query{}, spills);
    auto second = kernel.spill_query(Query{}, spills);
    assert(first.token != second.token);
    assert(std::filesystem::exists(first.directory));
    assert(std::filesystem::exists(second.directory));

    // A generated token skips what is already on disk, so a restarted process cannot land on an old
    // result -- simulated here by creating the next name by hand.
    std::filesystem::create_directories(spills / "result-3");
    auto third = kernel.spill_query(Query{}, spills);
    assert(third.token != "result-3");

    // An explicit token that already exists is refused rather than overwritten: someone may be reading it.
    assert(throws([&] { kernel.spill_query(Query{}, spills, first.token); }));

    // And dropping it makes the name reusable.
    drop_spill(spills, first.token);
    assert(!std::filesystem::exists(first.directory));
    auto reused = kernel.spill_query(Query{}, spills, first.token);
    assert(reused.token == first.token);

    cleanup(root);
}

void invalid_tokens_are_refused() {
    auto root = test_root("invalid_tokens_refused");
    auto spills = root / "spills";

    KnowledgeKernel kernel(StorageConfig{root / "store"});
    seed(kernel);

    // A token becomes a directory name, so anything that could escape the spill directory is refused.
    for (const char *token : {"..", ".", "sub/dir", "../escape"}) {
        assert(throws([&] { kernel.spill_query(Query{}, spills, token); }));
    }

    // An *empty* token is not invalid at this layer -- it is how a caller asks for a generated name, which
    // is what the defaulted argument means. The writer below it still refuses one, so the two layers do
    // not disagree about what empty means.
    assert(!throws([&] { kernel.spill_query(Query{}, spills, ""); }));
    assert(throws([&] { write_spill(spills, "", {}, {}, Catalog{}); }));

    assert(throws([&] { drop_spill(spills, ".."); }));

    cleanup(root);
}

void spilling_honours_order_limit_and_offset() {
    auto root = test_root("honours_order_limit_offset");
    auto spills = root / "spills";

    KnowledgeKernel kernel(StorageConfig{root / "store"});
    seed(kernel);

    Query ordered;
    ordered.order = QueryOrder::ObservedAt;
    ordered.newest_first = true;

    auto all = kernel.spill_query(ordered, spills, "all");
    auto all_ids = read_column<AssertionId>(all.directory / "id.col");
    assert(all_ids.size() == 3);

    Query paged = ordered;
    paged.limit = 1;
    paged.offset = 1;

    auto page = kernel.spill_query(paged, spills, "page");
    auto page_ids = read_column<AssertionId>(page.directory / "id.col");
    assert(page_ids.size() == 1);
    assert(page_ids[0] == all_ids[1]); // the same row the equivalent page of a query would return

    cleanup(root);
}

void an_empty_result_is_a_valid_empty_spill() {
    auto root = test_root("empty_result_is_valid");
    auto spills = root / "spills";

    KnowledgeKernel kernel(StorageConfig{root / "store"});
    seed(kernel);

    Query nothing;
    nothing.subject = 999999; // never interned

    auto descriptor = kernel.spill_query(nothing, spills);
    assert(descriptor.rows == 0);

    // Present and empty, rather than absent: a consumer reading the descriptor should not have to handle
    // missing files as a special case.
    for (const auto &column : descriptor.columns) {
        assert(std::filesystem::exists(descriptor.directory / column.file));
        assert(std::filesystem::file_size(descriptor.directory / column.file) == 0);
    }
    assert(read_json(descriptor.directory / "descriptor.json").at("rows") == 0);
    assert(read_json(descriptor.directory / descriptor.dictionary_file).at("entities").empty());

    cleanup(root);
}

void a_read_only_kernel_can_spill_without_touching_the_root() {
    auto root = test_root("read_only_can_spill");
    auto store = root / "store";
    auto spills = root / "spills";

    {
        KnowledgeKernel writer(StorageConfig{store});
        seed(writer);
    }

    // Fingerprint every byte of the storage root, because this is the case the phase exists for: an
    // analytics reader producing result files while writing nothing to the store it opened.
    std::map<std::string, std::string> before;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(store)) {
        if (entry.is_regular_file()) {
            std::ifstream in(entry.path(), std::ios::binary);
            before[std::filesystem::relative(entry.path(), store).string()] =
                std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
    }

    {
        KnowledgeKernel reader(StorageConfig{store}, OpenMode::ReadOnly);
        auto descriptor = reader.spill_query(Query{}, spills);
        assert(descriptor.rows == 3);
        assert(read_column<AssertionId>(descriptor.directory / "id.col").size() == 3);
    }

    std::map<std::string, std::string> after;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(store)) {
        if (entry.is_regular_file()) {
            std::ifstream in(entry.path(), std::ios::binary);
            after[std::filesystem::relative(entry.path(), store).string()] =
                std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
    }

    assert(before == after);

    cleanup(root);
}

void spilling_escapes_the_json_row_ceiling() {
    auto root = test_root("escapes_the_json_ceiling");
    auto spills = root / "spills";

    KnowledgeKernel kernel(StorageConfig{root / "store"});

    // More rows than a query may return as JSON, which is the reason spilling exists.
    const size_t rows = MAX_QUERY_RESULT + 500;
    for (size_t written = 0; written < rows; written += 10'000) {
        std::vector<PendingAssertion> batch;
        size_t count = std::min<size_t>(10'000, rows - written);
        for (size_t i = 0; i < count; ++i) {
            batch.push_back({static_cast<EntityId>(written + i + 1), 1, 2, 0, OPEN_ENDED,
                             static_cast<Timestamp>(written + i), 1.0});
        }
        kernel.commit_batch(batch);
    }

    Query everything;
    auto paged = kernel.query(everything);
    assert(paged.assertions.size() == MAX_QUERY_RESULT);
    assert(paged.truncated); // the JSON path caps and says so

    auto descriptor = kernel.spill_query(everything, spills);
    assert(descriptor.rows == rows); // the spill does not
    assert(read_column<AssertionId>(descriptor.directory / "id.col").size() == rows);

    cleanup(root);
}

} // namespace

int main() {
    spill_columns_match_the_query_row_for_row();
    the_spilled_status_is_the_effective_one();
    the_dictionary_covers_the_ids_present_in_every_value_kind();
    tokens_are_generated_without_collisions_and_never_overwritten();
    invalid_tokens_are_refused();
    spilling_honours_order_limit_and_offset();
    an_empty_result_is_a_valid_empty_spill();
    a_read_only_kernel_can_spill_without_touching_the_root();
    spilling_escapes_the_json_row_ceiling();

    std::cout << "All spill tests passed.\n";
    return 0;
}
