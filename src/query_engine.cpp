#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

#include "kernel/query_engine.hpp"

namespace knk {

namespace {

// --- filter validation -----------------------------------------------------------
//
// Structural problems throw; data-dependent ones do not. A filter whose operand kind does not match
// its field is a caller mistake that can never match anything, so saying so beats returning an empty
// result the caller then has to explain.

void require_operand_kind(const Filter &filter, ValueKind expected, const char *field_name) {
    if (filter.operand.kind != expected) {
        throw std::runtime_error(std::string("filter on ") + field_name + " has an operand of the wrong kind");
    }
}

void validate(const Filter &filter, size_t depth) {
    if (depth > MAX_FILTER_DEPTH) {
        throw std::runtime_error("filter nested deeper than MAX_FILTER_DEPTH");
    }

    switch (filter.kind) {
    case FilterKind::Comparison:
        switch (filter.field) {
        case FilterField::Subject:
        case FilterField::Predicate:
        case FilterField::Object:
            require_operand_kind(filter, ValueKind::Int64, "an id field");
            break;
        case FilterField::ObjectValue:
            break; // any kind: the row's own value kind decides whether it can match
        case FilterField::Confidence:
            require_operand_kind(filter, ValueKind::Double, "confidence");
            break;
        case FilterField::ValidFrom:
        case FilterField::ValidTo:
        case FilterField::ObservedAt:
            require_operand_kind(filter, ValueKind::Timestamp, "a timestamp field");
            break;
        case FilterField::Status:
            require_operand_kind(filter, ValueKind::Text, "status");
            if (!status_from_name(filter.operand.text).has_value()) {
                throw std::runtime_error("unknown status name in filter: " + filter.operand.text);
            }
            break;
        }
        break;

    case FilterKind::And:
    case FilterKind::Or:
        // Rejected rather than folded to true/false: an empty conjunction almost always means the
        // caller built the tree wrong, and a query engine guessing which identity was intended is worse
        // than one that says so.
        if (filter.children.empty()) {
            throw std::runtime_error("and/or filter needs at least one child");
        }
        for (const auto &child : filter.children) {
            validate(child, depth + 1);
        }
        break;

    case FilterKind::Not:
        if (filter.children.size() != 1) {
            throw std::runtime_error("not filter needs exactly one child");
        }
        validate(filter.children.front(), depth + 1);
        break;
    }
}

// --- filter evaluation -----------------------------------------------------------

// -1 / 0 / 1, comparing the one field that `kind` makes meaningful. Both values are known to share a
// kind by the time this is called.
int compare_same_kind(const Value &left, const Value &right) {
    switch (left.kind) {
    case ValueKind::Text:
        return left.text < right.text ? -1 : (left.text == right.text ? 0 : 1);
    case ValueKind::Int64:
        return left.int64_value < right.int64_value ? -1 : (left.int64_value == right.int64_value ? 0 : 1);
    case ValueKind::Double:
        return left.double_value < right.double_value ? -1 : (left.double_value == right.double_value ? 0 : 1);
    case ValueKind::Bool:
        return left.bool_value == right.bool_value ? 0 : (right.bool_value ? -1 : 1); // false < true
    case ValueKind::Timestamp:
        return left.timestamp_value < right.timestamp_value ? -1
                                                            : (left.timestamp_value == right.timestamp_value ? 0 : 1);
    }

    return 0;
}

bool apply_op(CompareOp op, int ordering) {
    switch (op) {
    case CompareOp::Eq:
        return ordering == 0;
    case CompareOp::Ne:
        return ordering != 0;
    case CompareOp::Lt:
        return ordering < 0;
    case CompareOp::Lte:
        return ordering <= 0;
    case CompareOp::Gt:
        return ordering > 0;
    case CompareOp::Gte:
        return ordering >= 0;
    }

    return false;
}

// The row's value for a field, as a Value so one comparison path serves every field. ObjectValue is the
// only one that can be absent: an object id with nothing interned against it (a document id, or an id
// that was never interned at all) simply has no value to compare.
std::optional<Value> field_value(const Assertion &assertion, FilterField field, const Catalog &catalog) {
    switch (field) {
    case FilterField::Subject:
        return Value::of_int64(static_cast<int64_t>(assertion.subject));
    case FilterField::Predicate:
        return Value::of_int64(static_cast<int64_t>(assertion.predicate));
    case FilterField::Object:
        return Value::of_int64(static_cast<int64_t>(assertion.object));
    case FilterField::ObjectValue:
        return catalog.entity_value(assertion.object);
    case FilterField::Confidence:
        return Value::of_double(assertion.confidence);
    case FilterField::ValidFrom:
        return Value::of_timestamp(assertion.valid_from);
    case FilterField::ValidTo:
        return Value::of_timestamp(assertion.valid_to);
    case FilterField::ObservedAt:
        return Value::of_timestamp(assertion.observed_at);
    case FilterField::Status:
        return Value::of_text(status_name(assertion.status));
    }

    return std::nullopt;
}

bool evaluate(const Filter &filter, const Assertion &assertion, const Catalog &catalog) {
    switch (filter.kind) {
    case FilterKind::Comparison: {
        auto value = field_value(assertion, filter.field, catalog);

        // A kind mismatch is heterogeneous data, not a malformed query: objects across the kernel are
        // a mix of named entities and typed literals, so "object value > 100" simply does not match a
        // row whose object is text.
        if (!value.has_value() || value->kind != filter.operand.kind) {
            return false;
        }

        return apply_op(filter.op, compare_same_kind(*value, filter.operand));
    }

    case FilterKind::And:
        for (const auto &child : filter.children) {
            if (!evaluate(child, assertion, catalog)) {
                return false;
            }
        }
        return true;

    case FilterKind::Or:
        for (const auto &child : filter.children) {
            if (evaluate(child, assertion, catalog)) {
                return true;
            }
        }
        return false;

    case FilterKind::Not:
        return !evaluate(filter.children.front(), assertion, catalog);
    }

    return false;
}

// --- candidate selection ---------------------------------------------------------

// True when the query asks for exactly what the current-state indexes store: Active status and an
// open-ended valid interval, i.e. is_current_assertion's definition. Only then may those indexes be
// used as a candidate source, because they deliberately hold nothing else -- an assertion drops out of
// them the moment it is superseded or retracted, so a query wanting any other status would silently
// lose rows.
bool is_current_shaped(const Query &query) {
    return query.open_ended_only && query.statuses.size() == 1 && query.statuses.front() == AssertionStatus::Active;
}

// nullopt means "scan everything"; an empty vector means "this index genuinely holds nothing for these
// arguments", which is a real (empty) answer rather than a reason to fall back. Phase 10 conflated the
// two and rescanned the whole log whenever an index came back empty.
//
// Every rule here is chosen to be result-preserving by construction rather than by benchmark: an index
// is used only where it provably contains every row the query could match. Cost-based selection over
// real statistics is Phase 16.
std::optional<std::vector<AssertionId>> select_candidates(const Query &query, std::optional<EntityId> subject,
                                                          std::optional<EntityId> object,
                                                          const IndexManager &index_manager) {
    if (query.force_scan) {
        return std::nullopt;
    }

    if (subject.has_value()) {
        // observed_before returns the subject's assertions with observed_at <= t, which is a prefix of
        // assertions_for_subject -- never larger, often much smaller, and a row outside it could not
        // have matched observed_to anyway.
        if (query.observed_to.has_value()) {
            return index_manager.observed_before(*subject, *query.observed_to);
        }

        return index_manager.assertions_for_subject(*subject);
    }

    if (is_current_shaped(query)) {
        std::optional<std::vector<AssertionId>> by_object;
        std::optional<std::vector<AssertionId>> by_predicate;

        if (object.has_value()) {
            by_object = index_manager.current_assertions_by_object(*object);
        }

        if (query.predicate.has_value()) {
            by_predicate = index_manager.current_assertions_by_predicate(*query.predicate);
        }

        // Both apply: take the smaller bucket. The other selector still filters every row, so this only
        // changes how many rows are examined.
        if (by_object.has_value() && by_predicate.has_value()) {
            return by_object->size() <= by_predicate->size() ? by_object : by_predicate;
        }

        if (by_object.has_value()) {
            return by_object;
        }

        if (by_predicate.has_value()) {
            return by_predicate;
        }
    }

    return std::nullopt;
}

bool status_allowed(const Query &query, AssertionStatus status) {
    if (query.statuses.empty()) {
        return true; // no status filter, the audit-shaped reads' behavior
    }

    return std::find(query.statuses.begin(), query.statuses.end(), status) != query.statuses.end();
}

// Inclusive start, exclusive end, with OPEN_ENDED meaning "no end" -- the same arithmetic valid_at()
// applies, kept in one place so the two cannot drift apart.
bool valid_at_covers(const Assertion &assertion, Timestamp valid_time) {
    bool starts_before_or_at = assertion.valid_from <= valid_time;
    bool ends_after = assertion.valid_to == OPEN_ENDED || valid_time < assertion.valid_to;
    return starts_before_or_at && ends_after;
}

bool matches(const Query &query, const Assertion &assertion, std::optional<EntityId> subject,
             std::optional<EntityId> object, const Catalog &catalog) {
    // The resolved query argument is compared against the raw stored id, which is what the existing
    // methods do: merge_entities never rewrites assertions_, it redirects at the query boundary only.
    if (subject.has_value() && assertion.subject != *subject) {
        return false;
    }

    if (query.predicate.has_value() && assertion.predicate != *query.predicate) {
        return false;
    }

    if (object.has_value() && assertion.object != *object) {
        return false;
    }

    if (!status_allowed(query, assertion.status)) {
        return false;
    }

    if (query.open_ended_only && assertion.valid_to != OPEN_ENDED) {
        return false;
    }

    if (query.valid_at.has_value() && !valid_at_covers(assertion, *query.valid_at)) {
        return false;
    }

    if (query.observed_from.has_value() && assertion.observed_at < *query.observed_from) {
        return false;
    }

    if (query.observed_to.has_value() && assertion.observed_at > *query.observed_to) {
        return false;
    }

    // Cheapest first: the filter tree is the only test that can hit the catalog, so it runs last.
    if (query.filter.has_value() && !evaluate(*query.filter, assertion, catalog)) {
        return false;
    }

    return true;
}

// Total order: the requested key first, then AssertionId as tie-break, so every query has exactly one
// answer. newest_first reverses the whole comparison, tie-break included, matching changes_since.
bool ordered_before(const Assertion &a, const Assertion &b, QueryOrder order) {
    switch (order) {
    case QueryOrder::ValidFrom:
        if (a.valid_from != b.valid_from) {
            return a.valid_from < b.valid_from;
        }
        break;
    case QueryOrder::ObservedAt:
        if (a.observed_at != b.observed_at) {
            return a.observed_at < b.observed_at;
        }
        break;
    case QueryOrder::AssertionId:
        break;
    }

    return a.id < b.id;
}

const Assertion *find_by_id(const std::vector<Assertion> &assertions, AssertionId id) {
    if (id == 0 || id > assertions.size()) {
        return nullptr;
    }

    // assertions_ is dense and id-ordered (ids start at 1 and every commit appends exactly one), the
    // same indexing KnowledgeKernel::get relies on.
    return &assertions[id - 1];
}

} // namespace

QueryResult QueryEngine::execute(const Query &query, const std::vector<Assertion> &assertions,
                                 const IndexManager &index_manager, const Catalog &catalog) const {
    if (query.ir_version != QUERY_IR_VERSION) {
        throw std::runtime_error("unsupported query IR version");
    }

    if (query.filter.has_value()) {
        validate(*query.filter, 1);
    }

    // Resolved once, then compared against raw stored ids -- see matches().
    std::optional<EntityId> subject;
    if (query.subject.has_value()) {
        subject = catalog.resolve(*query.subject);
    }

    std::optional<EntityId> object;
    if (query.object.has_value()) {
        object = catalog.resolve(*query.object);
    }

    std::vector<Assertion> matched;

    auto candidates = select_candidates(query, subject, object, index_manager);
    if (candidates.has_value()) {
        for (AssertionId id : *candidates) {
            const Assertion *assertion = find_by_id(assertions, id);
            if (assertion != nullptr && matches(query, *assertion, subject, object, catalog)) {
                matched.push_back(*assertion);
            }
        }
    } else {
        for (const auto &assertion : assertions) {
            if (matches(query, assertion, subject, object, catalog)) {
                matched.push_back(assertion);
            }
        }
    }

    std::sort(matched.begin(), matched.end(), [&query](const Assertion &a, const Assertion &b) {
        return query.newest_first ? ordered_before(b, a, query.order) : ordered_before(a, b, query.order);
    });

    size_t limit = query.limit == 0 ? MAX_QUERY_RESULT : std::min(query.limit, MAX_QUERY_RESULT);

    QueryResult result;
    result.truncated = matched.size() > query.offset + limit;

    if (query.offset >= matched.size()) {
        return result; // offset past the end is an empty page, not an error
    }

    auto begin = matched.begin() + static_cast<std::ptrdiff_t>(query.offset);
    auto end = matched.size() - query.offset > limit ? begin + static_cast<std::ptrdiff_t>(limit) : matched.end();

    result.assertions.assign(begin, end);

    // Only the returned page is resolved, never the whole match set: a 10,000-row match paged three at
    // a time costs three lookups per field, not 10,000.
    if (query.resolve_names) {
        result.names.reserve(result.assertions.size());

        for (const auto &assertion : result.assertions) {
            ResolvedNames names;

            auto subject_value = catalog.entity_value(assertion.subject);
            if (subject_value.has_value() && subject_value->kind == ValueKind::Text) {
                names.subject_name = subject_value->text;
            }

            names.predicate_name = catalog.predicate_name(assertion.predicate);
            names.object_value = catalog.entity_value(assertion.object);

            result.names.push_back(std::move(names));
        }
    }

    return result;
}

} // namespace knk
