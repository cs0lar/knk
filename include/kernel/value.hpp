#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "kernel/time.hpp"

namespace knk {

enum class ValueKind : uint8_t { Text, Int64, Double, Bool, Timestamp };

// A typed literal that can be interned into an EntityId by Catalog, distinct from a named entity
// only in that nobody assigned it a label -- "Alice" and the number 42 both become entities, but
// Value is how 42 (or a short sentence, boolean, or timestamp) gets one. Only the field matching
// `kind` is meaningful; the rest sit at their default value, which keeps equality/hashing simple
// (compare/hash every field) without needing a tagged union.
struct Value {
    ValueKind kind;
    std::string text;
    int64_t int64_value = 0;
    double double_value = 0.0;
    bool bool_value = false;
    Timestamp timestamp_value = 0;

    static Value of_text(std::string text) {
        Value value;
        value.kind = ValueKind::Text;
        value.text = std::move(text);
        return value;
    }

    static Value of_int64(int64_t v) {
        Value value;
        value.kind = ValueKind::Int64;
        value.int64_value = v;
        return value;
    }

    static Value of_double(double v) {
        Value value;
        value.kind = ValueKind::Double;
        value.double_value = v;
        return value;
    }

    static Value of_bool(bool v) {
        Value value;
        value.kind = ValueKind::Bool;
        value.bool_value = v;
        return value;
    }

    static Value of_timestamp(Timestamp v) {
        Value value;
        value.kind = ValueKind::Timestamp;
        value.timestamp_value = v;
        return value;
    }

    bool operator==(const Value &) const = default;
};

} // namespace knk

namespace std {

template <> struct hash<knk::Value> {
    size_t operator()(const knk::Value &value) const noexcept {
        size_t seed = std::hash<uint8_t>{}(static_cast<uint8_t>(value.kind));
        auto combine = [&seed](size_t h) { seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2); };

        combine(std::hash<std::string>{}(value.text));
        combine(std::hash<int64_t>{}(value.int64_value));
        combine(std::hash<double>{}(value.double_value));
        combine(std::hash<bool>{}(value.bool_value));
        combine(std::hash<knk::Timestamp>{}(value.timestamp_value));

        return seed;
    }
};

} // namespace std
