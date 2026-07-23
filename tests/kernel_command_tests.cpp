// One round-trip test per KernelCommand variant: each confirms KnowledgeKernel::execute(cmd) returns
// the same result as calling the mirrored public method directly. For query commands (no mutation)
// the direct call and the execute() call run against the same kernel. For mutating commands they run
// against two freshly-seeded twin kernels, so the two calls start from identical state and must
// return the identical value.
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "kernel/kernel_command.hpp"
#include "kernel/kernel_result.hpp"
#include "kernel/knowledge_kernel.hpp"

using namespace knk;

namespace {

constexpr EntityId ALICE = 1;
constexpr EntityId ACME = 100;
constexpr EntityId BETA = 200;
constexpr PredicateId WORKS_AT = 10;

constexpr Timestamp JAN_1_2023 = 1672531200;
constexpr Timestamp JAN_1_2024 = 1704067200;
constexpr Timestamp JUL_1_2024 = 1719792000;
constexpr Timestamp JUL_2_2024 = 1719878400;
constexpr Timestamp JUL_8_2024 = 1720450412;

std::filesystem::path test_root(const std::string &name) {
    auto path = std::filesystem::temp_directory_path() / ("kernel_command_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void cleanup(const std::filesystem::path &path) { std::filesystem::remove_all(path); }

std::vector<std::byte> bytes(const std::string &s) {
    std::vector<std::byte> b(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        b[i] = static_cast<std::byte>(s[i]);
    }
    return b;
}

std::vector<AssertionId> ids(const std::vector<Assertion> &v) {
    std::vector<AssertionId> r;
    for (const auto &a : v) {
        r.push_back(a.id);
    }
    return r;
}

std::vector<std::pair<AssertionId, AssertionId>> conflict_ids(const std::vector<std::pair<Assertion, Assertion>> &v) {
    std::vector<std::pair<AssertionId, AssertionId>> r;
    for (const auto &p : v) {
        r.emplace_back(p.first.id, p.second.id);
    }
    return r;
}

// Shared richly-populated kernel for the read-only query round-trips.
struct SeedIds {
    EntityId alice;
    EntityId acme;
    EntityId beta;
    EntityId source;
    EntityId doc;
    PredicateId works_at;
    PredicateId lives_in;
    AssertionId a1; // alice works_at acme (superseded by a2)
    AssertionId a2; // alice works_at beta (active, supersedes a1)
    AssertionId conflict_a;
    AssertionId conflict_b;
};

SeedIds seed_query_kernel(KnowledgeKernel &k) {
    SeedIds s;
    s.alice = k.intern_entity("Alice");
    s.acme = k.intern_entity("Acme");
    s.beta = k.intern_entity("Beta");
    s.source = k.intern_entity("pipeline");
    s.works_at = k.intern_predicate("works_at");
    s.lives_in = k.intern_predicate("lives_in");

    s.a1 = k.commit(s.alice, s.works_at, s.acme, JAN_1_2023, JUL_1_2024, JUL_2_2024, 0.90);
    s.a2 = k.commit_superseding(s.alice, s.works_at, s.beta, JUL_1_2024, OPEN_ENDED, JUL_2_2024, 0.95, s.a1);

    // Two overlapping active lives_in claims with different objects -> a conflict.
    s.conflict_a = k.commit(s.alice, s.lives_in, s.acme, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.50);
    s.conflict_b = k.commit(s.alice, s.lives_in, s.beta, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.50);

    k.record_provenance(s.a1, s.source, JUL_2_2024, "manual");

    s.doc = k.intern_document(bytes("hello world"));

    return s;
}

// --- Mutating command round-trips (twin kernels) ---------------------------------

void commit_command_round_trips() {
    auto root = test_root("commit_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto d = direct.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    auto v = std::get<AssertionId>(
        via.execute(CommitCommand{ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90}));

    assert(d == v);
    assert(via.get(v).has_value());

    cleanup(root);
}

void commit_by_name_command_round_trips() {
    auto root = test_root("commit_by_name_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto d =
        direct.commit_by_name("Alice", "works_at", Value::of_text("Acme"), JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    auto v = std::get<AssertionId>(via.execute(
        CommitByNameCommand{"Alice", "works_at", Value::of_text("Acme"), JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90}));

    auto direct_assertion = direct.get(d);
    auto via_assertion = via.get(v);

    assert(direct_assertion.has_value() && via_assertion.has_value());
    assert(direct_assertion->subject == via_assertion->subject);
    assert(direct_assertion->predicate == via_assertion->predicate);
    assert(direct_assertion->object == via_assertion->object);

    cleanup(root);
}

void commit_superseding_command_round_trips() {
    auto root = test_root("commit_superseding_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto base_d = direct.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    auto base_v = via.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    assert(base_d == base_v);

    auto d = direct.commit_superseding(ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95, base_d);
    auto v = std::get<AssertionId>(
        via.execute(CommitSupersedingCommand{ALICE, WORKS_AT, BETA, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.95, base_v}));

    assert(d == v);
    assert(via.get(base_v)->status == AssertionStatus::Superseded);

    cleanup(root);
}

void commit_retraction_command_round_trips() {
    auto root = test_root("commit_retraction_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto base_d = direct.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    auto base_v = via.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);

    auto d = direct.commit_retraction(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90, base_d);
    auto v = std::get<AssertionId>(
        via.execute(CommitRetractionCommand{ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90, base_v}));

    assert(d == v);
    assert(via.get(base_v)->status == AssertionStatus::Retracted);

    cleanup(root);
}

void write_snapshot_command_round_trips() {
    auto root = test_root("write_snapshot_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);

    // Void-returning command -> monostate. Executing it must not throw and must leave the kernel
    // usable (the snapshot is an optimization hint, verified in the knowledge_kernel snapshot tests).
    auto r = kernel.execute(WriteSnapshotCommand{});
    assert(std::holds_alternative<std::monostate>(r));

    cleanup(root);
}

void intern_entity_command_round_trips() {
    auto root = test_root("intern_entity_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto d = direct.intern_entity("Alice");
    auto v = std::get<AssertionId>(via.execute(InternEntityCommand{"Alice"}));

    assert(d == v);

    cleanup(root);
}

void intern_value_command_round_trips() {
    auto root = test_root("intern_value_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto d = direct.intern_value(Value::of_int64(42));
    auto v = std::get<AssertionId>(via.execute(InternValueCommand{Value::of_int64(42)}));

    assert(d == v);

    cleanup(root);
}

void intern_predicate_command_round_trips() {
    auto root = test_root("intern_predicate_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto d = direct.intern_predicate("works_at");
    auto v = std::get<AssertionId>(via.execute(InternPredicateCommand{"works_at"}));

    assert(d == v);

    cleanup(root);
}

void intern_document_command_round_trips() {
    auto root = test_root("intern_document_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto d = direct.intern_document(bytes("a document"));
    auto v = std::get<AssertionId>(via.execute(InternDocumentCommand{bytes("a document")}));

    assert(d == v);
    assert(via.document_content(v) == bytes("a document"));

    cleanup(root);
}

void record_provenance_command_round_trips() {
    auto root = test_root("record_provenance_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});

    auto source = kernel.intern_entity("pipeline");
    auto id = kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);

    auto r = kernel.execute(RecordProvenanceCommand{id, source, JUL_2_2024, "manual"});
    assert(std::holds_alternative<std::monostate>(r));

    auto p = kernel.provenance_for(id);
    assert(p.has_value());
    assert(p->source == source);
    assert(p->method == "manual");

    cleanup(root);
}

void commit_hypothesis_command_round_trips() {
    auto root = test_root("commit_hypothesis_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    auto source_d = direct.intern_entity("predictor_model");
    auto source_v = via.intern_entity("predictor_model");
    assert(source_d == source_v);

    auto d = direct.commit_hypothesis(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6, source_d,
                                      JUL_2_2024, "predicted_by_model");
    auto v = std::get<AssertionId>(via.execute(CommitHypothesisCommand{
        ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6, source_v, JUL_2_2024, "predicted_by_model"}));

    assert(d == v);
    assert(via.get(v)->status == AssertionStatus::Hypothesis);
    auto provenance = via.provenance_for(v);
    assert(provenance.has_value());
    assert(provenance->method == "predicted_by_model");

    cleanup(root);
}

void merge_entities_command_round_trips() {
    auto root = test_root("merge_entities_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct"});
    KnowledgeKernel via(StorageConfig{root / "via"});

    constexpr EntityId ALICE_DUPLICATE = 999;

    direct.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    via.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);

    direct.merge_entities(ALICE, ALICE_DUPLICATE, JUL_2_2024);
    auto r = via.execute(MergeEntitiesCommand{ALICE, ALICE_DUPLICATE, JUL_2_2024});
    assert(std::holds_alternative<std::monostate>(r));

    assert(direct.resolve_entity(ALICE_DUPLICATE) == via.resolve_entity(ALICE_DUPLICATE));
    assert(via.current(ALICE_DUPLICATE).size() == 1);

    cleanup(root);
}

void archive_segments_before_command_round_trips() {
    auto root = test_root("archive_segments_before_command_round_trips");
    KnowledgeKernel direct(StorageConfig{root / "direct", 2});
    KnowledgeKernel via(StorageConfig{root / "via", 2});

    AssertionId last_direct = 0;
    AssertionId last_via = 0;
    for (int i = 0; i < 5; ++i) {
        last_direct = direct.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
        last_via = via.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);
    }
    assert(last_direct == last_via);

    direct.archive_segments_before(last_direct);
    auto r = via.execute(ArchiveSegmentsBeforeCommand{last_via});
    assert(std::holds_alternative<std::monostate>(r));

    assert(direct.get(1).has_value());
    assert(via.get(1).has_value());
    assert(via.get(last_via).has_value());

    cleanup(root);
}

// --- Query command round-trips (same kernel, direct vs execute) ------------------

void get_command_round_trips() {
    auto root = test_root("get_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.get(s.a1);
    auto v = std::get<std::optional<Assertion>>(kernel.execute(GetCommand{s.a1}));
    assert(d.has_value() && v.has_value());
    assert(d->id == v->id);

    // An unknown id round-trips as nullopt through both paths too.
    assert(!std::get<std::optional<Assertion>>(kernel.execute(GetCommand{9999})).has_value());

    cleanup(root);
}

void assertions_for_subject_command_round_trips() {
    auto root = test_root("assertions_for_subject_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.assertions_for_subject(s.alice);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(AssertionsForSubjectCommand{s.alice}));
    assert(ids(d) == ids(v));

    cleanup(root);
}

void current_command_round_trips() {
    auto root = test_root("current_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.current(s.alice);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(CurrentCommand{s.alice}));
    assert(ids(d) == ids(v));

    cleanup(root);
}

void current_by_name_command_round_trips() {
    auto root = test_root("current_by_name_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    seed_query_kernel(kernel);

    auto d = kernel.current_by_name("Alice");
    auto v = std::get<std::vector<Assertion>>(kernel.execute(CurrentByNameCommand{"Alice"}));
    assert(ids(d) == ids(v));
    assert(!v.empty());

    // An unknown name round-trips as empty through both paths too.
    assert(kernel.current_by_name("Nobody").empty());
    assert(std::get<std::vector<Assertion>>(kernel.execute(CurrentByNameCommand{"Nobody"})).empty());

    cleanup(root);
}

void current_by_object_command_round_trips() {
    auto root = test_root("current_by_object_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.current_by_object(s.beta);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(CurrentByObjectCommand{s.beta}));
    assert(ids(d) == ids(v));
    assert(!v.empty());

    cleanup(root);
}

void current_by_predicate_command_round_trips() {
    auto root = test_root("current_by_predicate_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.current_by_predicate(s.works_at);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(CurrentByPredicateCommand{s.works_at}));
    assert(ids(d) == ids(v));
    assert(!v.empty());

    cleanup(root);
}

void valid_at_command_round_trips() {
    auto root = test_root("valid_at_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.valid_at(s.alice, JAN_1_2024);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(ValidAtCommand{s.alice, JAN_1_2024}));
    assert(ids(d) == ids(v));

    cleanup(root);
}

void known_at_command_round_trips() {
    auto root = test_root("known_at_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.known_at(s.alice, JUL_8_2024);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(KnownAtCommand{s.alice, JUL_8_2024}));
    assert(ids(d) == ids(v));

    cleanup(root);
}

void valid_at_known_at_command_round_trips() {
    auto root = test_root("valid_at_known_at_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.valid_at_known_at(s.alice, JAN_1_2024, JUL_8_2024);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(ValidAtKnownAtCommand{s.alice, JAN_1_2024, JUL_8_2024}));
    assert(ids(d) == ids(v));

    cleanup(root);
}

void valid_time_timeline_command_round_trips() {
    auto root = test_root("valid_time_timeline_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.valid_time_timeline(s.alice, s.works_at);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(ValidTimeTimelineCommand{s.alice, s.works_at}));
    assert(ids(d) == ids(v));

    cleanup(root);
}

void observed_time_timeline_command_round_trips() {
    auto root = test_root("observed_time_timeline_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.observed_time_timeline(s.alice, s.works_at);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(ObservedTimeTimelineCommand{s.alice, s.works_at}));
    assert(ids(d) == ids(v));

    cleanup(root);
}

void commit_history_command_round_trips() {
    auto root = test_root("commit_history_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.commit_history(s.alice, s.works_at);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(CommitHistoryCommand{s.alice, s.works_at}));
    assert(ids(d) == ids(v));

    cleanup(root);
}

void changes_since_command_round_trips() {
    auto root = test_root("changes_since_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    seed_query_kernel(kernel);

    auto d = kernel.changes_since(0);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(ChangesSinceCommand{0}));
    assert(ids(d) == ids(v));
    assert(!v.empty());

    cleanup(root);
}

void explain_command_round_trips() {
    auto root = test_root("explain_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.explain(s.a2);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(ExplainCommand{s.a2}));
    assert(ids(d) == ids(v));
    assert(v.size() == 2); // a2 then the a1 it superseded

    cleanup(root);
}

void find_conflicts_command_round_trips() {
    auto root = test_root("find_conflicts_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.find_conflicts(s.alice, s.lives_in);
    auto v = std::get<std::vector<std::pair<Assertion, Assertion>>>(
        kernel.execute(FindConflictsCommand{s.alice, s.lives_in}));
    assert(conflict_ids(d) == conflict_ids(v));
    assert(v.size() == 1);

    cleanup(root);
}

void find_entity_command_round_trips() {
    auto root = test_root("find_entity_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.find_entity("Alice");
    auto v = std::get<std::optional<AssertionId>>(kernel.execute(FindEntityCommand{"Alice"}));
    assert(d == v);
    assert(v == std::optional<EntityId>(s.alice));

    assert(!std::get<std::optional<AssertionId>>(kernel.execute(FindEntityCommand{"Nobody"})).has_value());

    cleanup(root);
}

void find_value_command_round_trips() {
    auto root = test_root("find_value_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    seed_query_kernel(kernel);

    auto d = kernel.find_value(Value::of_text("Alice"));
    auto v = std::get<std::optional<AssertionId>>(kernel.execute(FindValueCommand{Value::of_text("Alice")}));
    assert(d == v);

    cleanup(root);
}

void find_predicate_command_round_trips() {
    auto root = test_root("find_predicate_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.find_predicate("works_at");
    auto v = std::get<std::optional<AssertionId>>(kernel.execute(FindPredicateCommand{"works_at"}));
    assert(d == v);
    assert(v == std::optional<PredicateId>(s.works_at));

    cleanup(root);
}

void entity_name_command_round_trips() {
    auto root = test_root("entity_name_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.entity_name(s.alice);
    auto v = std::get<std::optional<std::string>>(kernel.execute(EntityNameCommand{s.alice}));
    assert(d == v);
    assert(v == std::optional<std::string>("Alice"));

    cleanup(root);
}

void entity_value_command_round_trips() {
    auto root = test_root("entity_value_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.entity_value(s.alice);
    auto v = std::get<std::optional<Value>>(kernel.execute(EntityValueCommand{s.alice}));
    assert(d == v);
    assert(v == std::optional<Value>(Value::of_text("Alice")));

    cleanup(root);
}

void predicate_name_command_round_trips() {
    auto root = test_root("predicate_name_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.predicate_name(s.works_at);
    auto v = std::get<std::optional<std::string>>(kernel.execute(PredicateNameCommand{s.works_at}));
    assert(d == v);
    assert(v == std::optional<std::string>("works_at"));

    cleanup(root);
}

void document_content_command_round_trips() {
    auto root = test_root("document_content_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.document_content(s.doc);
    auto v = std::get<std::optional<std::vector<std::byte>>>(kernel.execute(DocumentContentCommand{s.doc}));
    assert(d == v);
    assert(v == std::optional<std::vector<std::byte>>(bytes("hello world")));

    cleanup(root);
}

void provenance_for_command_round_trips() {
    auto root = test_root("provenance_for_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});
    auto s = seed_query_kernel(kernel);

    auto d = kernel.provenance_for(s.a1);
    auto v = std::get<std::optional<ProvenanceRecord>>(kernel.execute(ProvenanceForCommand{s.a1}));
    assert(d.has_value() && v.has_value());
    assert(d->assertion_id == v->assertion_id);
    assert(d->source == v->source);
    assert(d->recorded_at == v->recorded_at);
    assert(d->method == v->method);

    cleanup(root);
}

void hypotheses_for_command_round_trips() {
    auto root = test_root("hypotheses_for_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});

    auto source = kernel.intern_entity("predictor_model");
    kernel.commit_hypothesis(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.6, source, JUL_2_2024,
                             "predicted_by_model");

    auto d = kernel.hypotheses_for(ALICE);
    auto v = std::get<std::vector<Assertion>>(kernel.execute(HypothesesForCommand{ALICE}));
    assert(ids(d) == ids(v));
    assert(v.size() == 1);

    cleanup(root);
}

void neighbors_command_round_trips() {
    auto root = test_root("neighbors_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);

    auto d = kernel.neighbors(ALICE, 1);
    auto v = std::get<std::vector<EntityId>>(kernel.execute(NeighborsCommand{ALICE, 1}));
    assert(d == v);
    assert(v.size() == 1);
    assert(v[0] == ACME);

    cleanup(root);
}

void co_occurring_predicates_command_round_trips() {
    auto root = test_root("co_occurring_predicates_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});

    kernel.commit(ALICE, WORKS_AT, ACME, JAN_1_2023, OPEN_ENDED, JUL_2_2024, 0.90);

    auto d = kernel.co_occurring_predicates(ALICE);
    auto v = std::get<std::vector<EntityId>>(kernel.execute(CoOccurringPredicatesCommand{ALICE}));
    assert(d == v);
    assert(v.size() == 1);
    assert(v[0] == WORKS_AT);

    cleanup(root);
}

void resolve_entity_command_round_trips() {
    auto root = test_root("resolve_entity_command_round_trips");
    KnowledgeKernel kernel(StorageConfig{root});

    constexpr EntityId ALICE_DUPLICATE = 999;
    kernel.merge_entities(ALICE, ALICE_DUPLICATE, JUL_2_2024);

    auto d = kernel.resolve_entity(ALICE_DUPLICATE);
    auto v = std::get<AssertionId>(kernel.execute(ResolveEntityCommand{ALICE_DUPLICATE}));
    assert(d == v);
    assert(v == ALICE);

    cleanup(root);
}

} // namespace

int main() {
    commit_command_round_trips();
    commit_by_name_command_round_trips();
    commit_superseding_command_round_trips();
    commit_retraction_command_round_trips();
    write_snapshot_command_round_trips();
    intern_entity_command_round_trips();
    intern_value_command_round_trips();
    intern_predicate_command_round_trips();
    intern_document_command_round_trips();
    record_provenance_command_round_trips();
    commit_hypothesis_command_round_trips();
    merge_entities_command_round_trips();
    archive_segments_before_command_round_trips();
    get_command_round_trips();
    assertions_for_subject_command_round_trips();
    current_command_round_trips();
    current_by_name_command_round_trips();
    current_by_object_command_round_trips();
    current_by_predicate_command_round_trips();
    valid_at_command_round_trips();
    known_at_command_round_trips();
    valid_at_known_at_command_round_trips();
    valid_time_timeline_command_round_trips();
    observed_time_timeline_command_round_trips();
    commit_history_command_round_trips();
    changes_since_command_round_trips();
    explain_command_round_trips();
    find_conflicts_command_round_trips();
    find_entity_command_round_trips();
    find_value_command_round_trips();
    find_predicate_command_round_trips();
    entity_name_command_round_trips();
    entity_value_command_round_trips();
    predicate_name_command_round_trips();
    document_content_command_round_trips();
    provenance_for_command_round_trips();
    hypotheses_for_command_round_trips();
    neighbors_command_round_trips();
    co_occurring_predicates_command_round_trips();
    resolve_entity_command_round_trips();

    std::cout << "All kernel_command tests passed.\n";
}
