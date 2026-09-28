#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "kernel/json_codec.hpp"

using namespace knk;

namespace {

Assertion sample_assertion() {
    Assertion assertion;
    assertion.id = 7;
    assertion.subject = 1;
    assertion.predicate = 10;
    assertion.object = 100;
    assertion.valid_from = 1704067200;
    assertion.valid_to = 0;
    assertion.observed_at = 1719792000;
    assertion.confidence = 0.85;
    assertion.status = AssertionStatus::Active;
    assertion.supersedes_id = 3;
    assertion.retracts_id = 0;
    return assertion;
}

void assertion_round_trips_through_json() {
    auto original = sample_assertion();

    auto json = assertion_to_json(original);
    auto restored = assertion_from_json(json);

    assert(restored.id == original.id);
    assert(restored.subject == original.subject);
    assert(restored.predicate == original.predicate);
    assert(restored.object == original.object);
    assert(restored.valid_from == original.valid_from);
    assert(restored.valid_to == original.valid_to);
    assert(restored.observed_at == original.observed_at);
    assert(restored.confidence == original.confidence);
    assert(restored.status == original.status);
    assert(restored.supersedes_id == original.supersedes_id);
    assert(restored.retracts_id == original.retracts_id);
}

void every_assertion_status_round_trips_through_json() {
    for (auto status : {AssertionStatus::Active, AssertionStatus::Superseded, AssertionStatus::Retracted,
                        AssertionStatus::Retraction, AssertionStatus::Hypothesis}) {
        assert(status_from_json(status_to_json(status)) == status);
    }
}

void every_value_kind_round_trips_through_json() {
    assert(value_from_json(value_to_json(Value::of_text("hello"))) == Value::of_text("hello"));
    assert(value_from_json(value_to_json(Value::of_int64(-42))) == Value::of_int64(-42));
    assert(value_from_json(value_to_json(Value::of_double(3.5))) == Value::of_double(3.5));
    assert(value_from_json(value_to_json(Value::of_bool(true))) == Value::of_bool(true));
    assert(value_from_json(value_to_json(Value::of_timestamp(1719792000))) == Value::of_timestamp(1719792000));
}

void provenance_record_round_trips_through_json() {
    ProvenanceRecord record{7, 42, 1719792000, "daily_crm_sync"};

    auto restored = provenance_record_from_json(provenance_record_to_json(record));

    assert(restored.assertion_id == record.assertion_id);
    assert(restored.source == record.source);
    assert(restored.recorded_at == record.recorded_at);
    assert(restored.method == record.method);
}

void base64_round_trips_every_padding_case() {
    // Zero, one, two mod-3 remainder bytes exercise the "==", "=", and no-padding branches.
    std::vector<std::byte> empty;
    std::vector<std::byte> one_byte{std::byte{0x61}};
    std::vector<std::byte> two_bytes{std::byte{0x61}, std::byte{0x62}};
    std::vector<std::byte> three_bytes{std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};

    assert(base64_decode(base64_encode(empty)) == empty);
    assert(base64_decode(base64_encode(one_byte)) == one_byte);
    assert(base64_decode(base64_encode(two_bytes)) == two_bytes);
    assert(base64_decode(base64_encode(three_bytes)) == three_bytes);

    // Known vector: "Man" -> "TWFu" (classic base64 example, no padding).
    std::vector<std::byte> man{std::byte{'M'}, std::byte{'a'}, std::byte{'n'}};
    assert(base64_encode(man) == "TWFu");
}

void kernel_result_to_json_covers_every_alternative() {
    assert(kernel_result_to_json(KernelResult{std::monostate{}}).is_null());

    assert(kernel_result_to_json(KernelResult{AssertionId{5}}) == 5);

    assert(kernel_result_to_json(KernelResult{std::optional<Assertion>{std::nullopt}}).is_null());
    auto assertion_result = kernel_result_to_json(KernelResult{std::optional<Assertion>{sample_assertion()}});
    assert(assertion_result.at("id") == 7);

    auto vector_result = kernel_result_to_json(KernelResult{std::vector<Assertion>{sample_assertion()}});
    assert(vector_result.is_array());
    assert(vector_result.size() == 1);

    auto conflicts = std::vector<std::pair<Assertion, Assertion>>{{sample_assertion(), sample_assertion()}};
    auto conflicts_result = kernel_result_to_json(KernelResult{conflicts});
    assert(conflicts_result.is_array());
    assert(conflicts_result[0].contains("a"));
    assert(conflicts_result[0].contains("b"));

    assert(kernel_result_to_json(KernelResult{std::optional<AssertionId>{std::nullopt}}).is_null());
    assert(kernel_result_to_json(KernelResult{std::optional<AssertionId>{9}}) == 9);

    assert(kernel_result_to_json(KernelResult{std::optional<std::string>{std::nullopt}}).is_null());
    assert(kernel_result_to_json(KernelResult{std::optional<std::string>{"Alice"}}) == "Alice");

    assert(kernel_result_to_json(KernelResult{std::optional<Value>{std::nullopt}}).is_null());
    assert(kernel_result_to_json(KernelResult{std::optional<Value>{Value::of_int64(1)}}).at("kind") == "int64");

    assert(kernel_result_to_json(KernelResult{std::optional<std::vector<std::byte>>{std::nullopt}}).is_null());
    std::vector<std::byte> content{std::byte{'h'}, std::byte{'i'}};
    auto content_result = kernel_result_to_json(KernelResult{std::optional<std::vector<std::byte>>{content}});
    assert(content_result == "aGk=");

    assert(kernel_result_to_json(KernelResult{std::optional<ProvenanceRecord>{std::nullopt}}).is_null());
    ProvenanceRecord record{7, 42, 1719792000, "manual"};
    auto provenance_result = kernel_result_to_json(KernelResult{std::optional<ProvenanceRecord>{record}});
    assert(provenance_result.at("method") == "manual");

    auto ids_result = kernel_result_to_json(KernelResult{std::vector<EntityId>{1, 2, 3}});
    assert(ids_result.is_array());
    assert(ids_result.size() == 3);
    assert(ids_result[1] == 2);

    // Batch reads: one slot per id in input order, with a null slot kept in place rather than dropped,
    // so a caller can zip ids with answers.
    auto names_result =
        kernel_result_to_json(KernelResult{std::vector<std::optional<std::string>>{"Alice", std::nullopt, "Acme"}});
    assert(names_result.is_array());
    assert(names_result.size() == 3);
    assert(names_result[0] == "Alice");
    assert(names_result[1].is_null());
    assert(names_result[2] == "Acme");

    auto values_result =
        kernel_result_to_json(KernelResult{std::vector<std::optional<Value>>{std::nullopt, Value::of_int64(42)}});
    assert(values_result.size() == 2);
    assert(values_result[0].is_null());
    assert(values_result[1].at("kind") == "int64");

    auto provenance_batch_result =
        kernel_result_to_json(KernelResult{std::vector<std::optional<ProvenanceRecord>>{record, std::nullopt}});
    assert(provenance_batch_result.size() == 2);
    assert(provenance_batch_result[0].at("method") == "manual");
    assert(provenance_batch_result[1].is_null());

    // A query result carries its rows and the truncated flag together, so a caller can tell a complete
    // answer from a first page without re-counting.
    QueryResult query_result;
    query_result.assertions.push_back(sample_assertion());
    query_result.truncated = true;
    auto query_json = kernel_result_to_json(KernelResult{query_result});
    assert(query_json.at("assertions").is_array());
    assert(query_json.at("assertions").size() == 1);
    assert(query_json.at("assertions")[0].at("id") == 7);
    assert(query_json.at("truncated") == true);

    QueryResult empty_result;
    auto empty_json = kernel_result_to_json(KernelResult{empty_result});
    assert(empty_json.at("assertions").is_array());
    assert(empty_json.at("assertions").empty());
    assert(empty_json.at("truncated") == false);
}

} // namespace

int main() {
    assertion_round_trips_through_json();
    every_assertion_status_round_trips_through_json();
    every_value_kind_round_trips_through_json();
    provenance_record_round_trips_through_json();
    base64_round_trips_every_padding_case();
    kernel_result_to_json_covers_every_alternative();

    std::cout << "All json_codec tests passed.\n";
    return 0;
}
