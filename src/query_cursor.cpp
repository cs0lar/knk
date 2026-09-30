#include <array>
#include <charconv>
#include <stdexcept>
#include <vector>

#include "kernel/query_cursor.hpp"

namespace knk {

namespace {

constexpr const char *CURSOR_PREFIX = "knkc";

const char *order_token(QueryOrder order) {
    switch (order) {
    case QueryOrder::ValidFrom:
        return "valid_from";
    case QueryOrder::ObservedAt:
        return "observed_at";
    case QueryOrder::AssertionId:
        break;
    }
    return "id";
}

bool order_from_token(const std::string &token, QueryOrder &order) {
    if (token == "id") {
        order = QueryOrder::AssertionId;
    } else if (token == "valid_from") {
        order = QueryOrder::ValidFrom;
    } else if (token == "observed_at") {
        order = QueryOrder::ObservedAt;
    } else {
        return false;
    }
    return true;
}

// from_chars rather than stoll: it does not throw, does not consult the locale, and rejects trailing
// junk, which for a token that arrives from outside is the point.
template <typename T> bool parse_integer(const std::string &text, T &out) {
    if (text.empty()) {
        return false;
    }
    const char *begin = text.data();
    const char *end = begin + text.size();
    auto result = std::from_chars(begin, end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

std::vector<std::string> split_fields(const std::string &token) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        size_t separator = token.find(':', start);
        if (separator == std::string::npos) {
            fields.push_back(token.substr(start));
            return fields;
        }
        fields.push_back(token.substr(start, separator - start));
        start = separator + 1;
    }
}

} // namespace

int64_t cursor_key(const Assertion &assertion, QueryOrder order) {
    switch (order) {
    case QueryOrder::ValidFrom:
        return assertion.valid_from;
    case QueryOrder::ObservedAt:
        return assertion.observed_at;
    case QueryOrder::AssertionId:
        break;
    }
    return static_cast<int64_t>(assertion.id);
}

std::string encode_query_cursor(const QueryCursor &cursor) {
    return std::string(CURSOR_PREFIX) + std::to_string(QUERY_CURSOR_VERSION) + ":" + order_token(cursor.order) + ":" +
           (cursor.newest_first ? "desc" : "asc") + ":" + std::to_string(cursor.key) + ":" + std::to_string(cursor.id);
}

QueryCursor decode_query_cursor(const std::string &token) {
    auto reject = [&token]() -> QueryCursor { throw std::runtime_error("malformed query cursor: '" + token + "'"); };

    std::vector<std::string> fields = split_fields(token);
    if (fields.size() != 5) {
        return reject();
    }

    const std::string &version_field = fields[0];
    std::string prefix(CURSOR_PREFIX);
    if (version_field.size() <= prefix.size() || version_field.compare(0, prefix.size(), prefix) != 0) {
        return reject();
    }

    uint32_t version = 0;
    if (!parse_integer(version_field.substr(prefix.size()), version)) {
        return reject();
    }
    if (version != QUERY_CURSOR_VERSION) {
        throw std::runtime_error("unsupported query cursor version: " + std::to_string(version));
    }

    QueryCursor cursor;
    if (!order_from_token(fields[1], cursor.order)) {
        return reject();
    }

    if (fields[2] == "desc") {
        cursor.newest_first = true;
    } else if (fields[2] == "asc") {
        cursor.newest_first = false;
    } else {
        return reject();
    }

    if (!parse_integer(fields[3], cursor.key) || !parse_integer(fields[4], cursor.id)) {
        return reject();
    }

    // Id 0 is never assigned, so a token naming it was not produced by encode_query_cursor.
    if (cursor.id == 0) {
        return reject();
    }

    return cursor;
}

bool after_query_cursor(const QueryCursor &cursor, const Assertion &assertion) {
    int64_t key = cursor_key(assertion, cursor.order);
    if (key != cursor.key) {
        return cursor.newest_first ? key < cursor.key : key > cursor.key;
    }

    // The tie-break that makes the order total, and it has to match ordered_before's: same field, same
    // direction. A cursor that disagreed with the sort would drop or repeat exactly the tied rows.
    return cursor.newest_first ? assertion.id < cursor.id : assertion.id > cursor.id;
}

} // namespace knk
