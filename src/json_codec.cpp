#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "kernel/json_codec.hpp"

namespace knk {

namespace {

constexpr std::string_view BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::array<int8_t, 256> make_base64_decode_table() {
    std::array<int8_t, 256> table;
    table.fill(-1);
    for (size_t i = 0; i < BASE64_ALPHABET.size(); ++i) {
        table[static_cast<unsigned char>(BASE64_ALPHABET[i])] = static_cast<int8_t>(i);
    }
    return table;
}

} // namespace

nlohmann::json status_to_json(AssertionStatus status) { return status_name(status); }

AssertionStatus status_from_json(const nlohmann::json &json) {
    std::string name = json.get<std::string>();

    // The names themselves live in status.hpp, shared with the query IR's Status filter.
    auto status = status_from_name(name);
    if (!status.has_value()) {
        throw std::runtime_error("unknown AssertionStatus: " + name);
    }

    return *status;
}

nlohmann::json assertion_to_json(const Assertion &assertion) {
    nlohmann::json json;
    json["id"] = assertion.id;
    json["subject"] = assertion.subject;
    json["predicate"] = assertion.predicate;
    json["object"] = assertion.object;
    json["valid_from"] = assertion.valid_from;
    json["valid_to"] = assertion.valid_to;
    json["observed_at"] = assertion.observed_at;
    json["confidence"] = assertion.confidence;
    json["status"] = status_to_json(assertion.status);
    json["supersedes_id"] = assertion.supersedes_id;
    json["retracts_id"] = assertion.retracts_id;
    return json;
}

Assertion assertion_from_json(const nlohmann::json &json) {
    Assertion assertion;
    assertion.id = json.at("id").get<AssertionId>();
    assertion.subject = json.at("subject").get<EntityId>();
    assertion.predicate = json.at("predicate").get<PredicateId>();
    assertion.object = json.at("object").get<EntityId>();
    assertion.valid_from = json.at("valid_from").get<Timestamp>();
    assertion.valid_to = json.at("valid_to").get<Timestamp>();
    assertion.observed_at = json.at("observed_at").get<Timestamp>();
    assertion.confidence = json.at("confidence").get<double>();
    assertion.status = status_from_json(json.at("status"));
    assertion.supersedes_id = json.value("supersedes_id", AssertionId{0});
    assertion.retracts_id = json.value("retracts_id", AssertionId{0});
    return assertion;
}

nlohmann::json value_to_json(const Value &value) {
    nlohmann::json json;

    switch (value.kind) {
    case ValueKind::Text:
        json["kind"] = "text";
        json["value"] = value.text;
        break;
    case ValueKind::Int64:
        json["kind"] = "int64";
        json["value"] = value.int64_value;
        break;
    case ValueKind::Double:
        json["kind"] = "double";
        json["value"] = value.double_value;
        break;
    case ValueKind::Bool:
        json["kind"] = "bool";
        json["value"] = value.bool_value;
        break;
    case ValueKind::Timestamp:
        json["kind"] = "timestamp";
        json["value"] = value.timestamp_value;
        break;
    }

    return json;
}

Value value_from_json(const nlohmann::json &json) {
    std::string kind = json.at("kind").get<std::string>();

    if (kind == "text") {
        return Value::of_text(json.at("value").get<std::string>());
    }
    if (kind == "int64") {
        return Value::of_int64(json.at("value").get<int64_t>());
    }
    if (kind == "double") {
        return Value::of_double(json.at("value").get<double>());
    }
    if (kind == "bool") {
        return Value::of_bool(json.at("value").get<bool>());
    }
    if (kind == "timestamp") {
        return Value::of_timestamp(json.at("value").get<Timestamp>());
    }

    throw std::runtime_error("unknown Value kind: " + kind);
}

nlohmann::json provenance_record_to_json(const ProvenanceRecord &record) {
    nlohmann::json json;
    json["assertion_id"] = record.assertion_id;
    json["source"] = record.source;
    json["recorded_at"] = record.recorded_at;
    json["method"] = record.method;
    return json;
}

ProvenanceRecord provenance_record_from_json(const nlohmann::json &json) {
    ProvenanceRecord record;
    record.assertion_id = json.at("assertion_id").get<AssertionId>();
    record.source = json.at("source").get<EntityId>();
    record.recorded_at = json.at("recorded_at").get<Timestamp>();
    record.method = json.at("method").get<std::string>();
    return record;
}

std::string base64_encode(const std::vector<std::byte> &bytes) {
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= bytes.size()) {
        uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                         static_cast<uint32_t>(bytes[i + 2]);
        result += BASE64_ALPHABET[(chunk >> 18) & 0x3F];
        result += BASE64_ALPHABET[(chunk >> 12) & 0x3F];
        result += BASE64_ALPHABET[(chunk >> 6) & 0x3F];
        result += BASE64_ALPHABET[chunk & 0x3F];
        i += 3;
    }

    size_t remaining = bytes.size() - i;
    if (remaining == 1) {
        uint32_t chunk = static_cast<uint32_t>(bytes[i]) << 16;
        result += BASE64_ALPHABET[(chunk >> 18) & 0x3F];
        result += BASE64_ALPHABET[(chunk >> 12) & 0x3F];
        result += "==";
    } else if (remaining == 2) {
        uint32_t chunk = (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
        result += BASE64_ALPHABET[(chunk >> 18) & 0x3F];
        result += BASE64_ALPHABET[(chunk >> 12) & 0x3F];
        result += BASE64_ALPHABET[(chunk >> 6) & 0x3F];
        result += "=";
    }

    return result;
}

std::vector<std::byte> base64_decode(const std::string &encoded) {
    static const std::array<int8_t, 256> decode_table = make_base64_decode_table();

    std::vector<std::byte> result;
    result.reserve((encoded.size() / 4) * 3);

    uint32_t buffer = 0;
    int bits_collected = 0;

    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }

        int8_t decoded = decode_table[static_cast<unsigned char>(c)];
        if (decoded < 0) {
            throw std::runtime_error("invalid base64 character");
        }

        buffer = (buffer << 6) | static_cast<uint32_t>(decoded);
        bits_collected += 6;

        if (bits_collected >= 8) {
            bits_collected -= 8;
            result.push_back(static_cast<std::byte>((buffer >> bits_collected) & 0xFF));
        }
    }

    return result;
}

nlohmann::json kernel_result_to_json(const KernelResult &result) {
    return std::visit(
        [](const auto &value) -> nlohmann::json {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr;
            } else if constexpr (std::is_same_v<T, AssertionId>) {
                return value;
            } else if constexpr (std::is_same_v<T, std::optional<Assertion>>) {
                return value.has_value() ? assertion_to_json(*value) : nlohmann::json(nullptr);
            } else if constexpr (std::is_same_v<T, std::vector<Assertion>>) {
                nlohmann::json array = nlohmann::json::array();
                for (const auto &assertion : value) {
                    array.push_back(assertion_to_json(assertion));
                }
                return array;
            } else if constexpr (std::is_same_v<T, std::vector<std::pair<Assertion, Assertion>>>) {
                nlohmann::json array = nlohmann::json::array();
                for (const auto &[a, b] : value) {
                    array.push_back(nlohmann::json{{"a", assertion_to_json(a)}, {"b", assertion_to_json(b)}});
                }
                return array;
            } else if constexpr (std::is_same_v<T, std::optional<AssertionId>>) {
                return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
            } else if constexpr (std::is_same_v<T, std::optional<std::string>>) {
                return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
            } else if constexpr (std::is_same_v<T, std::optional<Value>>) {
                return value.has_value() ? value_to_json(*value) : nlohmann::json(nullptr);
            } else if constexpr (std::is_same_v<T, std::optional<std::vector<std::byte>>>) {
                return value.has_value() ? nlohmann::json(base64_encode(*value)) : nlohmann::json(nullptr);
            } else if constexpr (std::is_same_v<T, std::optional<ProvenanceRecord>>) {
                return value.has_value() ? provenance_record_to_json(*value) : nlohmann::json(nullptr);
            } else if constexpr (std::is_same_v<T, std::vector<EntityId>>) {
                nlohmann::json array = nlohmann::json::array();
                for (EntityId id : value) {
                    array.push_back(id);
                }
                return array;
            } else if constexpr (std::is_same_v<T, SpillDescriptor>) {
                nlohmann::json columns = nlohmann::json::array();
                for (const auto &column : value.columns) {
                    columns.push_back(nlohmann::json{{"name", column.name},
                                                     {"file", column.file},
                                                     {"type", column.type},
                                                     {"bytes_per_value", column.bytes_per_value}});
                }

                // The same shape written to descriptor.json in the spill directory, so a caller can work
                // from either the tool response or the file on disk without a second vocabulary.
                return nlohmann::json{{"token", value.token},
                                      {"directory", value.directory.string()},
                                      {"rows", value.rows},
                                      {"columns", columns},
                                      {"dictionary", value.dictionary_file}};
            } else if constexpr (std::is_same_v<T, QueryPlan>) {
                nlohmann::json considered = nlohmann::json::array();
                for (const auto &option : value.considered) {
                    nlohmann::json entry{{"source", plan_source_name(option.source)}};
                    if (option.rejected_because.empty()) {
                        entry["rows"] = option.rows;
                        entry["cost"] = option.cost;
                    } else {
                        // Rejected options carry the reason instead of numbers: "why not" is the half of
                        // an explanation that tells a caller what to change about the query.
                        entry["rejected_because"] = option.rejected_because;
                    }
                    considered.push_back(entry);
                }

                return nlohmann::json{{"chosen", plan_source_name(value.chosen)},
                                      {"estimated_rows", value.estimated_rows},
                                      {"estimated_cost", value.estimated_cost},
                                      {"filter_evaluated_per_row", value.filter_evaluated_per_row},
                                      {"total_rows", value.total_rows},
                                      {"distinct_subjects", value.distinct_subjects},
                                      {"considered", considered}};
            } else if constexpr (std::is_same_v<T, AggregateResult>) {
                nlohmann::json groups = nlohmann::json::array();
                for (const auto &group : value.groups) {
                    nlohmann::json key = nlohmann::json::array();
                    for (const auto &part : group.key) {
                        key.push_back(value_to_json(part));
                    }

                    nlohmann::json cells = nlohmann::json::array();
                    for (const auto &cell : group.values) {
                        // Counts are integers; everything else is a number that may be null when no row
                        // in the group contributed one. Exactly one of the two is ever set.
                        if (cell.count.has_value()) {
                            cells.push_back(nlohmann::json(*cell.count));
                        } else if (cell.number.has_value()) {
                            cells.push_back(nlohmann::json(*cell.number));
                        } else {
                            cells.push_back(nlohmann::json(nullptr));
                        }
                    }

                    groups.push_back(nlohmann::json{{"key", key}, {"row_count", group.row_count}, {"values", cells}});
                }

                return nlohmann::json{{"groups", groups}};
            } else if constexpr (std::is_same_v<T, QueryResult>) {
                nlohmann::json assertions = nlohmann::json::array();
                for (const auto &assertion : value.assertions) {
                    assertions.push_back(assertion_to_json(assertion));
                }
                // truncated travels alongside the rows so a caller can tell "that was all of them" from
                // "here are the first N" without re-counting.
                nlohmann::json result{{"assertions", assertions}, {"truncated", value.truncated}};

                // Only when there is a next page. Present-and-a-token versus absent is the whole paging
                // protocol: a caller feeds it back as `cursor` and stops when it stops arriving.
                if (!value.next_cursor.empty()) {
                    result["next_cursor"] = value.next_cursor;
                }

                // Only present when the query asked for resolution, and then parallel to the rows: a
                // caller zips the two, and an absent name is null in its slot rather than missing.
                if (!value.names.empty()) {
                    nlohmann::json names = nlohmann::json::array();
                    for (const auto &resolved : value.names) {
                        names.push_back(nlohmann::json{
                            {"subject_name", resolved.subject_name.has_value() ? nlohmann::json(*resolved.subject_name)
                                                                               : nlohmann::json(nullptr)},
                            {"predicate_name", resolved.predicate_name.has_value()
                                                   ? nlohmann::json(*resolved.predicate_name)
                                                   : nlohmann::json(nullptr)},
                            {"object_value", resolved.object_value.has_value() ? value_to_json(*resolved.object_value)
                                                                               : nlohmann::json(nullptr)}});
                    }
                    result["names"] = names;
                }

                return result;
            } else if constexpr (std::is_same_v<T, std::vector<PredicateSummary>>) {
                nlohmann::json array = nlohmann::json::array();
                for (const auto &summary : value) {
                    array.push_back(nlohmann::json{
                        {"id", summary.id}, {"name", summary.name}, {"current_rows", summary.current_rows}});
                }
                return array;
            } else if constexpr (std::is_same_v<T, CorpusSummary>) {
                // Keyed by status *name*, not by the on-disk byte: the byte is a storage detail, and a
                // caller reading this is about to write those names into a query's `statuses`.
                nlohmann::json statuses = nlohmann::json::object();
                for (const auto &[status, count] : value.status_counts) {
                    statuses[status_name(status)] = count;
                }

                auto span = [](const std::optional<Timestamp> &bound) {
                    return bound.has_value() ? nlohmann::json(*bound) : nlohmann::json(nullptr);
                };

                return nlohmann::json{{"assertion_count", value.assertion_count},
                                      {"entity_count", value.entity_count},
                                      {"predicate_count", value.predicate_count},
                                      {"distinct_subjects", value.distinct_subjects},
                                      {"distinct_current_objects", value.distinct_current_objects},
                                      {"status_counts", statuses},
                                      {"min_observed_at", span(value.min_observed_at)},
                                      {"max_observed_at", span(value.max_observed_at)},
                                      {"min_valid_from", span(value.min_valid_from)},
                                      {"max_valid_from", span(value.max_valid_from)}};
            } else if constexpr (std::is_same_v<T, std::vector<std::optional<std::string>>>) {
                // One slot per id, in input order: a slot the single resolver would answer null for is
                // null here too, so a caller can zip ids with answers without matching them up.
                nlohmann::json array = nlohmann::json::array();
                for (const auto &slot : value) {
                    array.push_back(slot.has_value() ? nlohmann::json(*slot) : nlohmann::json(nullptr));
                }
                return array;
            } else if constexpr (std::is_same_v<T, std::vector<std::optional<Value>>>) {
                nlohmann::json array = nlohmann::json::array();
                for (const auto &slot : value) {
                    array.push_back(slot.has_value() ? value_to_json(*slot) : nlohmann::json(nullptr));
                }
                return array;
            } else if constexpr (std::is_same_v<T, std::vector<std::optional<ProvenanceRecord>>>) {
                nlohmann::json array = nlohmann::json::array();
                for (const auto &slot : value) {
                    array.push_back(slot.has_value() ? provenance_record_to_json(*slot) : nlohmann::json(nullptr));
                }
                return array;
            } else {
                static_assert(!sizeof(T *), "unhandled KernelResult alternative");
            }
        },
        result);
}

} // namespace knk
