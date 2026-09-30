#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include "kernel/query_engine.hpp"
#include "kernel/query_planner.hpp"

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

// Compares one field against the filter's operand **without materializing a Value for the row**.
//
// The earlier form built a Value per row per comparison and, for ObjectValue, copied the catalog's
// std::string into an optional. Phase 11 measured per-row filter evaluation at ~7 ns, most of it exactly
// that; a comparison is a few instructions, so the allocation dominated the work it was wrapping.
//
// Semantics are unchanged, deliberately down to the awkward corner: status compares by *name*, so
// ordered comparisons on status stay lexicographic. A string_view makes that allocation-free without
// changing what it means.
int compare_ints(int64_t left, int64_t right) { return left < right ? -1 : (left == right ? 0 : 1); }

bool evaluate_comparison(const Filter &filter, const Assertion &assertion, const Catalog &catalog) {
    const Value &operand = filter.operand;

    switch (filter.field) {
    case FilterField::Subject:
        return apply_op(filter.op, compare_ints(static_cast<int64_t>(assertion.subject), operand.int64_value));
    case FilterField::Predicate:
        return apply_op(filter.op, compare_ints(static_cast<int64_t>(assertion.predicate), operand.int64_value));
    case FilterField::Object:
        return apply_op(filter.op, compare_ints(static_cast<int64_t>(assertion.object), operand.int64_value));
    case FilterField::Confidence: {
        double left = assertion.confidence;
        double right = operand.double_value;
        return apply_op(filter.op, left < right ? -1 : (left == right ? 0 : 1));
    }
    case FilterField::ValidFrom:
        return apply_op(filter.op, compare_ints(assertion.valid_from, operand.timestamp_value));
    case FilterField::ValidTo:
        return apply_op(filter.op, compare_ints(assertion.valid_to, operand.timestamp_value));
    case FilterField::ObservedAt:
        return apply_op(filter.op, compare_ints(assertion.observed_at, operand.timestamp_value));
    case FilterField::Status: {
        std::string_view left(status_name(assertion.status));
        std::string_view right(operand.text);
        return apply_op(filter.op, left < right ? -1 : (left == right ? 0 : 1));
    }
    case FilterField::ObjectValue: {
        // The one field that has to consult the catalog, and the one that can legitimately be absent or
        // of another kind -- heterogeneous data, not a malformed query, so it simply does not match.
        const Value *value = catalog.find_entity_value(assertion.object);
        if (value == nullptr || value->kind != operand.kind) {
            return false;
        }
        return apply_op(filter.op, compare_same_kind(*value, operand));
    }
    }

    return false;
}

bool evaluate(const Filter &filter, const Assertion &assertion, const Catalog &catalog) {
    switch (filter.kind) {
    case FilterKind::Comparison:
        return evaluate_comparison(filter, assertion, catalog);

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

// The ids a chosen index source yields. Deliberately separate from planning: a plan can be produced --
// and explained -- without doing any of the work it describes, which is what makes explain_query cheap
// enough to call before every query.
std::vector<AssertionId> candidate_ids(PlanSource source, const Query &query, std::optional<EntityId> subject,
                                       std::optional<EntityId> object, const IndexManager &index_manager) {
    switch (source) {
    case PlanSource::SubjectIndex:
        return index_manager.assertions_for_subject(*subject);
    case PlanSource::ObservedTimeIndex:
        return index_manager.observed_before(*subject, *query.observed_to);
    case PlanSource::ObjectCurrentIndex:
        return index_manager.current_assertions_by_object(*object);
    case PlanSource::PredicateCurrentIndex:
        return index_manager.current_assertions_by_predicate(*query.predicate);
    case PlanSource::ColumnarScan:
    case PlanSource::RowScan:
        break;
    }

    return {};
}

bool is_index_source(PlanSource source) { return source != PlanSource::ColumnarScan && source != PlanSource::RowScan; }

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

const Assertion *find_by_id(std::span<const Assertion> assertions, AssertionId id) {
    if (id == 0 || id > assertions.size()) {
        return nullptr;
    }

    // assertions_ is dense and id-ordered (ids start at 1 and every commit appends exactly one), the
    // same indexing KnowledgeKernel::get relies on.
    return &assertions[id - 1];
}

// --- aggregation (Phase 12) ------------------------------------------------------

// A total order over Values: kind first, then whichever field that kind makes meaningful. Used only
// for group keys, where having *a* deterministic order is the whole point -- it is not claimed to be
// meaningful across kinds.
bool value_less(const Value &left, const Value &right) {
    if (left.kind != right.kind) {
        return static_cast<uint8_t>(left.kind) < static_cast<uint8_t>(right.kind);
    }

    return compare_same_kind(left, right) < 0;
}

struct KeyLess {
    bool operator()(const std::vector<Value> &left, const std::vector<Value> &right) const {
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(), value_less);
    }
};

// Floor division, so a bucket boundary means the same thing before and after the epoch: -1 with width
// 10 belongs to bucket -10, not 0. width is validated positive before this is called.
Timestamp floor_bucket(Timestamp value, Timestamp width) {
    Timestamp quotient = value / width;
    if (value % width != 0 && value < 0) {
        --quotient;
    }

    return quotient * width;
}

Value group_key_value(const Assertion &assertion, const GroupBy &group_by) {
    switch (group_by.field) {
    case GroupField::Subject:
        return Value::of_int64(static_cast<int64_t>(assertion.subject));
    case GroupField::Predicate:
        return Value::of_int64(static_cast<int64_t>(assertion.predicate));
    case GroupField::Object:
        return Value::of_int64(static_cast<int64_t>(assertion.object));
    case GroupField::Status:
        return Value::of_text(status_name(assertion.status));
    case GroupField::ValidFromBucket:
        return Value::of_timestamp(floor_bucket(assertion.valid_from, group_by.bucket_width));
    case GroupField::ObservedAtBucket:
        return Value::of_timestamp(floor_bucket(assertion.observed_at, group_by.bucket_width));
    }

    return Value::of_int64(0);
}

std::optional<Value> target_value(const Assertion &assertion, AggregateTarget target, const Catalog &catalog) {
    switch (target) {
    case AggregateTarget::ObjectValue:
        return catalog.entity_value(assertion.object);
    case AggregateTarget::Confidence:
        return Value::of_double(assertion.confidence);
    case AggregateTarget::ValidFrom:
        return Value::of_timestamp(assertion.valid_from);
    case AggregateTarget::ValidTo:
        return Value::of_timestamp(assertion.valid_to);
    case AggregateTarget::ObservedAt:
        return Value::of_timestamp(assertion.observed_at);
    case AggregateTarget::Subject:
        return Value::of_int64(static_cast<int64_t>(assertion.subject));
    case AggregateTarget::Predicate:
        return Value::of_int64(static_cast<int64_t>(assertion.predicate));
    case AggregateTarget::Object:
        return Value::of_int64(static_cast<int64_t>(assertion.object));
    }

    return std::nullopt;
}

// The numeric reading of a target, or nothing when the row has no number there -- a text or boolean
// object value, or an object with nothing interned against it. Such rows are *skipped* by
// sum/min/max/avg rather than counted as zero, which would quietly bias every average.
std::optional<double> target_number(const Assertion &assertion, AggregateTarget target, const Catalog &catalog) {
    auto value = target_value(assertion, target, catalog);
    if (!value.has_value()) {
        return std::nullopt;
    }

    switch (value->kind) {
    case ValueKind::Int64:
        return static_cast<double>(value->int64_value);
    case ValueKind::Double:
        return value->double_value;
    case ValueKind::Timestamp:
        return static_cast<double>(value->timestamp_value);
    case ValueKind::Text:
    case ValueKind::Bool:
        return std::nullopt;
    }

    return std::nullopt;
}

struct AggregateState {
    // For Count this counts rows; for the numeric functions it counts the rows that actually had a
    // number, which is what makes "10 rows, average over 3" reportable.
    int64_t contributing = 0;
    double sum = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    std::unordered_set<Value> distinct;
};

void validate_aggregate(const AggregateQuery &query) {
    if (query.ir_version != QUERY_IR_VERSION || query.selection.ir_version != QUERY_IR_VERSION) {
        throw std::runtime_error("unsupported query IR version");
    }

    if (query.aggregations.empty()) {
        throw std::runtime_error("aggregate needs at least one aggregation");
    }

    if (query.group_by.size() > MAX_GROUP_BY_FIELDS) {
        throw std::runtime_error("aggregate groups by more than MAX_GROUP_BY_FIELDS fields");
    }

    for (const auto &group_by : query.group_by) {
        bool is_bucket =
            group_by.field == GroupField::ValidFromBucket || group_by.field == GroupField::ObservedAtBucket;
        if (is_bucket && group_by.bucket_width <= 0) {
            throw std::runtime_error("bucket group-by needs a positive bucket_width");
        }
    }

    // Rejected rather than ignored: these describe how *rows* are returned, and an aggregate returns
    // groups. A caller who set them meant something the aggregate cannot deliver.
    const Query &selection = query.selection;
    if (selection.limit != 0 || selection.offset != 0 || selection.order != QueryOrder::AssertionId ||
        selection.newest_first || selection.resolve_names) {
        throw std::runtime_error("aggregate selection must not set limit/offset/order/newest_first/resolve_names");
    }
}

} // namespace

std::vector<uint32_t> QueryEngine::select_rows(const Query &query, const QuerySource &source,
                                               size_t ordered_prefix) const {
    const auto &assertions = source.assertions;
    const auto &index_manager = source.index_manager;
    const auto &catalog = source.catalog;

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

    // Row indices, not rows. Everything here works on 4-byte indices: filtering pushes them, sorting
    // swaps them, and materializing rows is the caller's business -- which is what lets a page copy a
    // handful of rows and a spill stream millions without either holding the other's cost.
    std::vector<uint32_t> matched;

    // One plan, followed -- not a plan produced for explaining and a separate set of rules for running.
    bool columns_ready = columns_usable(source.columns, source.effective_status, assertions.size());
    QueryPlan plan = plan_query(query, subject, object, index_manager, columns_ready, assertions.size());

    if (is_index_source(plan.chosen)) {
        for (AssertionId id : candidate_ids(plan.chosen, query, subject, object, index_manager)) {
            const Assertion *assertion = find_by_id(assertions, id);
            if (assertion != nullptr && matches(query, *assertion, subject, object, catalog)) {
                matched.push_back(static_cast<uint32_t>(id - 1));
            }
        }
    } else if (plan.chosen == PlanSource::ColumnarScan) {
        // The columnar path: the selectors, time windows, open-endedness and status set are answered by
        // passes over single columns, and only the survivors are touched as rows.
        vectorized_select(query, source.columns, source.effective_status, subject, object, matched);

        if (query.filter.has_value()) {
            // The filter tree stays per-row: it can consult the catalog and has arbitrary boolean shape,
            // so it is applied to what the vectorized passes left rather than to everything.
            auto surviving = matched.begin();
            for (uint32_t row : matched) {
                if (evaluate(*query.filter, assertions[row], catalog)) {
                    *surviving++ = row;
                }
            }
            matched.erase(surviving, matched.end());
        }
    } else {
        for (size_t i = 0; i < assertions.size(); ++i) {
            if (matches(query, assertions[i], subject, object, catalog)) {
                matched.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    auto ordered = [&query, &assertions](uint32_t left, uint32_t right) {
        return query.newest_first ? ordered_before(assertions[right], assertions[left], query.order)
                                  : ordered_before(assertions[left], assertions[right], query.order);
    };

    // Only as much order as the caller will use: a page discards the tail, so sorting it is work whose
    // result is thrown away. A spill asks for the whole thing ordered and gets a full sort.
    if (ordered_prefix < matched.size()) {
        std::partial_sort(matched.begin(), matched.begin() + static_cast<std::ptrdiff_t>(ordered_prefix), matched.end(),
                          ordered);
    } else {
        std::sort(matched.begin(), matched.end(), ordered);
    }

    return matched;
}

QueryResult QueryEngine::execute(const Query &query, const QuerySource &source) const {
    const auto &assertions = source.assertions;
    const auto &catalog = source.catalog;

    size_t limit = query.limit == 0 ? MAX_QUERY_RESULT : std::min(query.limit, MAX_QUERY_RESULT);

    // Saturating rather than wrapping: offset is caller-supplied and unbounded, so a wrapped offset+limit
    // would turn "a page past the end" into "a page from the beginning".
    size_t requested_end = query.offset > std::numeric_limits<size_t>::max() - limit
                               ? std::numeric_limits<size_t>::max()
                               : query.offset + limit;

    std::vector<uint32_t> matched = select_rows(query, source, requested_end);

    QueryResult result;
    result.truncated = matched.size() > requested_end;

    if (query.offset >= matched.size()) {
        return result; // offset past the end is an empty page, not an error
    }

    size_t page_end = std::min(matched.size(), requested_end);
    result.assertions.reserve(page_end - query.offset);
    for (size_t i = query.offset; i < page_end; ++i) {
        result.assertions.push_back(assertions[matched[i]]);
    }

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

QueryPlan QueryEngine::explain(const Query &query, const QuerySource &source) const {
    // Validates exactly as execute() does, so explaining a malformed query reports the same error rather
    // than describing a plan for something that would never run.
    if (query.ir_version != QUERY_IR_VERSION) {
        throw std::runtime_error("unsupported query IR version");
    }

    if (query.filter.has_value()) {
        validate(*query.filter, 1);
    }

    std::optional<EntityId> subject;
    if (query.subject.has_value()) {
        subject = source.catalog.resolve(*query.subject);
    }

    std::optional<EntityId> object;
    if (query.object.has_value()) {
        object = source.catalog.resolve(*query.object);
    }

    bool columns_ready = columns_usable(source.columns, source.effective_status, source.assertions.size());
    return plan_query(query, subject, object, source.index_manager, columns_ready, source.assertions.size());
}

AggregateResult QueryEngine::aggregate(const AggregateQuery &query, const QuerySource &source) const {
    const auto &assertions = source.assertions;
    const auto &index_manager = source.index_manager;
    const auto &catalog = source.catalog;

    validate_aggregate(query);

    const Query &selection = query.selection;
    if (selection.filter.has_value()) {
        validate(*selection.filter, 1);
    }

    std::optional<EntityId> subject;
    if (selection.subject.has_value()) {
        subject = catalog.resolve(*selection.subject);
    }

    std::optional<EntityId> object;
    if (selection.object.has_value()) {
        object = catalog.resolve(*selection.object);
    }

    size_t max_groups = query.max_groups == 0 ? MAX_GROUP_COUNT : std::min(query.max_groups, MAX_GROUP_COUNT);

    struct Group {
        int64_t row_count = 0;
        std::vector<AggregateState> states;
    };

    // An ordered map, so groups come out in key order without a separate sort, and so the group cap can
    // be enforced on insertion. Memory is bounded by the group count, not by the number of matching
    // rows -- rows are folded in and dropped, never collected.
    std::map<std::vector<Value>, Group, KeyLess> groups;

    auto fold = [&](const Assertion &assertion) {
        std::vector<Value> key;
        key.reserve(query.group_by.size());
        for (const auto &group_by : query.group_by) {
            key.push_back(group_key_value(assertion, group_by));
        }

        auto it = groups.find(key);
        if (it == groups.end()) {
            if (groups.size() >= max_groups) {
                throw std::runtime_error("aggregate produced more groups than max_groups allows");
            }

            Group fresh;
            fresh.states.resize(query.aggregations.size());
            it = groups.emplace(std::move(key), std::move(fresh)).first;
        }

        Group &group = it->second;
        ++group.row_count;

        for (size_t i = 0; i < query.aggregations.size(); ++i) {
            const Aggregation &aggregation = query.aggregations[i];
            AggregateState &state = group.states[i];

            switch (aggregation.function) {
            case AggregateFunction::Count:
                ++state.contributing;
                break;

            case AggregateFunction::CountDistinct: {
                auto value = target_value(assertion, aggregation.target, catalog);
                if (value.has_value()) {
                    state.distinct.insert(*value);
                }
                break;
            }

            case AggregateFunction::Sum:
            case AggregateFunction::Min:
            case AggregateFunction::Max:
            case AggregateFunction::Avg: {
                auto number = target_number(assertion, aggregation.target, catalog);
                if (!number.has_value()) {
                    break;
                }

                if (state.contributing == 0) {
                    state.minimum = *number;
                    state.maximum = *number;
                } else {
                    state.minimum = std::min(state.minimum, *number);
                    state.maximum = std::max(state.maximum, *number);
                }

                state.sum += *number;
                ++state.contributing;
                break;
            }
            }
        }
    };

    bool columns_ready = columns_usable(source.columns, source.effective_status, assertions.size());
    QueryPlan plan = plan_query(selection, subject, object, index_manager, columns_ready, assertions.size());

    if (is_index_source(plan.chosen)) {
        for (AssertionId id : candidate_ids(plan.chosen, selection, subject, object, index_manager)) {
            const Assertion *assertion = find_by_id(assertions, id);
            if (assertion != nullptr && matches(selection, *assertion, subject, object, catalog)) {
                fold(*assertion);
            }
        }
    } else if (plan.chosen == PlanSource::ColumnarScan) {
        std::vector<uint32_t> rows;
        vectorized_select(selection, source.columns, source.effective_status, subject, object, rows);

        for (uint32_t row : rows) {
            const Assertion &assertion = assertions[row];
            if (!selection.filter.has_value() || evaluate(*selection.filter, assertion, catalog)) {
                fold(assertion);
            }
        }
    } else {
        for (const auto &assertion : assertions) {
            if (matches(selection, assertion, subject, object, catalog)) {
                fold(assertion);
            }
        }
    }

    AggregateResult result;
    result.groups.reserve(groups.size());

    for (auto &[key, group] : groups) {
        AggregateGroup out;
        out.key = key;
        out.row_count = group.row_count;
        out.values.reserve(query.aggregations.size());

        for (size_t i = 0; i < query.aggregations.size(); ++i) {
            const Aggregation &aggregation = query.aggregations[i];
            const AggregateState &state = group.states[i];

            AggregateCell cell;
            switch (aggregation.function) {
            case AggregateFunction::Count:
                cell.count = state.contributing;
                break;
            case AggregateFunction::CountDistinct:
                cell.count = static_cast<int64_t>(state.distinct.size());
                break;
            case AggregateFunction::Sum:
                // Absent, not zero: a sum over nothing is unknown, and reporting 0.0 would be a claim
                // about data that was never there.
                if (state.contributing > 0) {
                    cell.number = state.sum;
                }
                break;
            case AggregateFunction::Min:
                if (state.contributing > 0) {
                    cell.number = state.minimum;
                }
                break;
            case AggregateFunction::Max:
                if (state.contributing > 0) {
                    cell.number = state.maximum;
                }
                break;
            case AggregateFunction::Avg:
                if (state.contributing > 0) {
                    cell.number = state.sum / static_cast<double>(state.contributing);
                }
                break;
            }

            out.values.push_back(cell);
        }

        result.groups.push_back(std::move(out));
    }

    return result;
}

} // namespace knk
