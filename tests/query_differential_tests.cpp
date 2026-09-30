// Phase 11's correctness gate: randomized queries, each answered three ways, all three required to
// agree.
//
//  1. The engine with index selection on.
//  2. The engine with Query::force_scan, so no index is consulted.
//  3. A brute-force evaluator written here, independently of the engine, over a plain vector.
//
// (1) vs (2) pins index selection to changing cost and never results -- the property that lets later
// phases plan aggressively. (1) vs (3) pins the *semantics*, because the reference implementation
// shares no code with the thing it checks. Parity against the existing query methods (Phase 10, in
// query_engine_tests.cpp) anchors what those semantics are supposed to be in the first place.
//
// The seed is fixed so a failure is reproducible, and printed on failure along with the offending
// query so it can be replayed.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "kernel/aggregate.hpp"
#include "kernel/knowledge_kernel.hpp"
#include "kernel/query.hpp"
#include "kernel/storage_config.hpp"

using namespace knk;

namespace {

constexpr uint32_t SEED = 20260928;
constexpr size_t QUERY_COUNT = 2000;

constexpr size_t SUBJECT_COUNT = 12;
constexpr size_t PREDICATE_COUNT = 3;

// --- the corpus ------------------------------------------------------------------

struct Corpus {
    std::vector<EntityId> subjects;
    std::vector<PredicateId> predicates;
    std::vector<EntityId> objects; // a mix of named entities and typed literals
    std::vector<Timestamp> timestamps;
    std::vector<Assertion> assertions; // what the brute-force evaluator reads
};

// Deliberately heterogeneous: text-valued objects next to int64 and double literals (so ObjectValue
// comparisons meet rows they cannot match), closed and open-ended intervals, every status, and
// observed times that are not in commit order (so the observed-time index's sorted insert matters).
Corpus populate(KnowledgeKernel &kernel) {
    Corpus corpus;

    for (size_t i = 0; i < SUBJECT_COUNT; ++i) {
        corpus.subjects.push_back(kernel.intern_entity("subject-" + std::to_string(i)));
    }

    for (size_t i = 0; i < PREDICATE_COUNT; ++i) {
        corpus.predicates.push_back(kernel.intern_predicate("predicate-" + std::to_string(i)));
    }

    corpus.objects.push_back(kernel.intern_entity("Acme"));
    corpus.objects.push_back(kernel.intern_entity("Beta"));
    corpus.objects.push_back(kernel.intern_value(Value::of_int64(42)));
    corpus.objects.push_back(kernel.intern_value(Value::of_int64(7)));
    corpus.objects.push_back(kernel.intern_value(Value::of_double(2.5)));
    corpus.objects.push_back(kernel.intern_value(Value::of_bool(true)));
    // An object id with nothing interned against it at all, so ObjectValue filters meet an absent value.
    corpus.objects.push_back(999'999);

    corpus.timestamps = {0, 1'000, 2'000, 3'000, 4'000, 5'000};

    std::mt19937 rng(SEED);
    auto pick = [&rng](const auto &pool) { return pool[rng() % pool.size()]; };

    std::vector<AssertionId> live;

    for (size_t i = 0; i < 160; ++i) {
        EntityId subject = pick(corpus.subjects);
        PredicateId predicate = pick(corpus.predicates);
        EntityId object = pick(corpus.objects);
        Timestamp valid_from = pick(corpus.timestamps);
        // Drawn from the same small pool the generated queries use, so that valid_at lands *exactly* on
        // an interval's end often rather than by luck. Mutation testing caught this: with valid_to as a
        // random offset, flipping the exclusive end (`point < valid_to`) to inclusive slipped past 2,000
        // randomized queries untouched, because no query timestamp ever coincided with a row's end.
        Timestamp valid_to = OPEN_ENDED;
        if (rng() % 2 != 0) {
            Timestamp candidate = pick(corpus.timestamps);
            valid_to = candidate > valid_from ? candidate : valid_from + 1'000;
        }
        Timestamp observed_at = pick(corpus.timestamps);
        double confidence = static_cast<double>(rng() % 101) / 100.0;

        if (rng() % 7 == 0) {
            kernel.commit_hypothesis(subject, predicate, object, valid_from, valid_to, observed_at, confidence,
                                     corpus.subjects.front(), observed_at, "guess");
            continue;
        }

        AssertionId id = kernel.commit(subject, predicate, object, valid_from, valid_to, observed_at, confidence);
        live.push_back(id);

        // Supersede or retract some of what exists, so Superseded/Retracted/Retraction rows appear.
        if (live.size() > 3 && rng() % 5 == 0) {
            AssertionId target = live[rng() % (live.size() - 1)];
            auto existing = kernel.get(target);
            if (existing.has_value() && existing->status == AssertionStatus::Active) {
                if (rng() % 2 == 0) {
                    live.push_back(kernel.commit_superseding(existing->subject, existing->predicate,
                                                             pick(corpus.objects), existing->valid_from, OPEN_ENDED,
                                                             observed_at, confidence, target));
                } else {
                    kernel.commit_retraction(existing->subject, existing->predicate, existing->object,
                                             existing->valid_from, existing->valid_to, observed_at, confidence, target);
                }
            }
        }
    }

    // A merge, so resolution is exercised by queries naming the absorbed id.
    kernel.merge_entities(corpus.subjects.front(), 123'456, 9'000);
    corpus.subjects.push_back(123'456);

    // Snapshot every assertion for the reference evaluator.
    Query everything;
    for (const auto &assertion : kernel.query(everything).assertions) {
        corpus.assertions.push_back(assertion);
    }

    return corpus;
}

// --- the reference evaluator (no engine code) ------------------------------------

int reference_compare(const Value &left, const Value &right) {
    switch (left.kind) {
    case ValueKind::Text:
        return left.text < right.text ? -1 : (left.text == right.text ? 0 : 1);
    case ValueKind::Int64:
        return left.int64_value < right.int64_value ? -1 : (left.int64_value == right.int64_value ? 0 : 1);
    case ValueKind::Double:
        return left.double_value < right.double_value ? -1 : (left.double_value == right.double_value ? 0 : 1);
    case ValueKind::Bool:
        return left.bool_value == right.bool_value ? 0 : (right.bool_value ? -1 : 1);
    case ValueKind::Timestamp:
        return left.timestamp_value < right.timestamp_value ? -1
                                                            : (left.timestamp_value == right.timestamp_value ? 0 : 1);
    }
    return 0;
}

bool reference_op(CompareOp op, int ordering) {
    if (op == CompareOp::Eq) {
        return ordering == 0;
    }
    if (op == CompareOp::Ne) {
        return ordering != 0;
    }
    if (op == CompareOp::Lt) {
        return ordering < 0;
    }
    if (op == CompareOp::Lte) {
        return ordering <= 0;
    }
    if (op == CompareOp::Gt) {
        return ordering > 0;
    }
    return ordering >= 0;
}

bool reference_filter(const Filter &filter, const Assertion &a, const KnowledgeKernel &kernel) {
    if (filter.kind == FilterKind::And) {
        for (const auto &child : filter.children) {
            if (!reference_filter(child, a, kernel)) {
                return false;
            }
        }
        return true;
    }

    if (filter.kind == FilterKind::Or) {
        for (const auto &child : filter.children) {
            if (reference_filter(child, a, kernel)) {
                return true;
            }
        }
        return false;
    }

    if (filter.kind == FilterKind::Not) {
        return !reference_filter(filter.children.front(), a, kernel);
    }

    std::optional<Value> value;
    switch (filter.field) {
    case FilterField::Subject:
        value = Value::of_int64(static_cast<int64_t>(a.subject));
        break;
    case FilterField::Predicate:
        value = Value::of_int64(static_cast<int64_t>(a.predicate));
        break;
    case FilterField::Object:
        value = Value::of_int64(static_cast<int64_t>(a.object));
        break;
    case FilterField::ObjectValue:
        value = kernel.entity_value(a.object);
        break;
    case FilterField::Confidence:
        value = Value::of_double(a.confidence);
        break;
    case FilterField::ValidFrom:
        value = Value::of_timestamp(a.valid_from);
        break;
    case FilterField::ValidTo:
        value = Value::of_timestamp(a.valid_to);
        break;
    case FilterField::ObservedAt:
        value = Value::of_timestamp(a.observed_at);
        break;
    case FilterField::Status:
        value = Value::of_text(status_name(a.status));
        break;
    }

    if (!value.has_value() || value->kind != filter.operand.kind) {
        return false;
    }

    return reference_op(filter.op, reference_compare(*value, filter.operand));
}

// The row-selection half of the reference, shared by the row and aggregate references below: an
// aggregate must fold exactly the rows a query would have returned.
std::vector<Assertion> reference_matching_rows(const Query &query, const Corpus &corpus,
                                               const KnowledgeKernel &kernel) {
    std::optional<EntityId> subject;
    if (query.subject.has_value()) {
        subject = kernel.resolve_entity(*query.subject);
    }

    std::optional<EntityId> object;
    if (query.object.has_value()) {
        object = kernel.resolve_entity(*query.object);
    }

    std::vector<Assertion> matched;

    for (const auto &a : corpus.assertions) {
        if (subject.has_value() && a.subject != *subject) {
            continue;
        }
        if (query.predicate.has_value() && a.predicate != *query.predicate) {
            continue;
        }
        if (object.has_value() && a.object != *object) {
            continue;
        }
        if (!query.statuses.empty() &&
            std::find(query.statuses.begin(), query.statuses.end(), a.status) == query.statuses.end()) {
            continue;
        }
        if (query.open_ended_only && a.valid_to != OPEN_ENDED) {
            continue;
        }
        if (query.valid_at.has_value()) {
            bool covers = a.valid_from <= *query.valid_at && (a.valid_to == OPEN_ENDED || *query.valid_at < a.valid_to);
            if (!covers) {
                continue;
            }
        }
        if (query.observed_from.has_value() && a.observed_at < *query.observed_from) {
            continue;
        }
        if (query.observed_to.has_value() && a.observed_at > *query.observed_to) {
            continue;
        }
        if (query.filter.has_value() && !reference_filter(*query.filter, a, kernel)) {
            continue;
        }

        matched.push_back(a);
    }

    return matched;
}

std::vector<AssertionId> reference_answer(const Query &query, const Corpus &corpus, const KnowledgeKernel &kernel,
                                          bool &truncated) {
    std::vector<Assertion> matched = reference_matching_rows(query, corpus, kernel);

    std::stable_sort(matched.begin(), matched.end(), [&query](const Assertion &x, const Assertion &y) {
        auto key = [&query](const Assertion &a) -> Timestamp {
            if (query.order == QueryOrder::ValidFrom) {
                return a.valid_from;
            }
            if (query.order == QueryOrder::ObservedAt) {
                return a.observed_at;
            }
            return 0;
        };

        const Assertion &first = query.newest_first ? y : x;
        const Assertion &second = query.newest_first ? x : y;

        if (query.order != QueryOrder::AssertionId && key(first) != key(second)) {
            return key(first) < key(second);
        }
        return first.id < second.id;
    });

    size_t limit = query.limit == 0 ? MAX_QUERY_RESULT : std::min(query.limit, MAX_QUERY_RESULT);
    truncated = matched.size() > query.offset + limit;

    std::vector<AssertionId> ids;
    for (size_t i = query.offset; i < matched.size() && ids.size() < limit; ++i) {
        ids.push_back(matched[i].id);
    }

    return ids;
}

// --- the generator ---------------------------------------------------------------

struct Generator {
    std::mt19937 rng;
    const Corpus &corpus;

    size_t roll(size_t n) { return rng() % n; }

    Filter random_leaf() {
        switch (roll(9)) {
        case 0:
            return Filter::compare(
                FilterField::Subject, random_op(),
                Value::of_int64(static_cast<int64_t>(corpus.subjects[roll(corpus.subjects.size())])));
        case 1:
            return Filter::compare(
                FilterField::Predicate, random_op(),
                Value::of_int64(static_cast<int64_t>(corpus.predicates[roll(corpus.predicates.size())])));
        case 2:
            return Filter::compare(FilterField::Object, random_op(),
                                   Value::of_int64(static_cast<int64_t>(corpus.objects[roll(corpus.objects.size())])));
        case 3:
            return Filter::compare(FilterField::Confidence, random_op(),
                                   Value::of_double(static_cast<double>(roll(101)) / 100.0));
        case 4:
            return Filter::compare(FilterField::ValidFrom, random_op(),
                                   Value::of_timestamp(corpus.timestamps[roll(corpus.timestamps.size())]));
        case 5:
            return Filter::compare(FilterField::ValidTo, random_op(),
                                   Value::of_timestamp(corpus.timestamps[roll(corpus.timestamps.size())]));
        case 6:
            return Filter::compare(FilterField::ObservedAt, random_op(),
                                   Value::of_timestamp(corpus.timestamps[roll(corpus.timestamps.size())]));
        case 7:
            return Filter::compare(FilterField::Status, random_op(), Value::of_text(random_status_name()));
        default:
            // Mixed operand kinds on purpose: many of these cannot match a given row, which is exactly
            // the heterogeneous-data case that must be "no match" rather than an error.
            switch (roll(3)) {
            case 0:
                return Filter::compare(FilterField::ObjectValue, random_op(), Value::of_text("Acme"));
            case 1:
                return Filter::compare(FilterField::ObjectValue, random_op(),
                                       Value::of_int64(static_cast<int64_t>(roll(50))));
            default:
                return Filter::compare(FilterField::ObjectValue, random_op(),
                                       Value::of_double(static_cast<double>(roll(50)) / 10.0));
            }
        }
    }

    CompareOp random_op() {
        switch (roll(6)) {
        case 0:
            return CompareOp::Eq;
        case 1:
            return CompareOp::Ne;
        case 2:
            return CompareOp::Lt;
        case 3:
            return CompareOp::Lte;
        case 4:
            return CompareOp::Gt;
        default:
            return CompareOp::Gte;
        }
    }

    std::string random_status_name() {
        switch (roll(5)) {
        case 0:
            return "Active";
        case 1:
            return "Superseded";
        case 2:
            return "Retracted";
        case 3:
            return "Retraction";
        default:
            return "Hypothesis";
        }
    }

    Filter random_filter(size_t depth) {
        if (depth >= 3 || roll(3) == 0) {
            return random_leaf();
        }

        switch (roll(3)) {
        case 0:
            return Filter::all_of({random_filter(depth + 1), random_filter(depth + 1)});
        case 1:
            return Filter::any_of({random_filter(depth + 1), random_filter(depth + 1)});
        default:
            return Filter::negate(random_filter(depth + 1));
        }
    }

    AssertionStatus random_status() {
        switch (roll(5)) {
        case 0:
            return AssertionStatus::Active;
        case 1:
            return AssertionStatus::Superseded;
        case 2:
            return AssertionStatus::Retracted;
        case 3:
            return AssertionStatus::Retraction;
        default:
            return AssertionStatus::Hypothesis;
        }
    }

    AggregateFunction random_function() {
        switch (roll(6)) {
        case 0:
            return AggregateFunction::Count;
        case 1:
            return AggregateFunction::CountDistinct;
        case 2:
            return AggregateFunction::Sum;
        case 3:
            return AggregateFunction::Min;
        case 4:
            return AggregateFunction::Max;
        default:
            return AggregateFunction::Avg;
        }
    }

    AggregateTarget random_target() {
        switch (roll(8)) {
        case 0:
            return AggregateTarget::ObjectValue;
        case 1:
            return AggregateTarget::Confidence;
        case 2:
            return AggregateTarget::ValidFrom;
        case 3:
            return AggregateTarget::ValidTo;
        case 4:
            return AggregateTarget::ObservedAt;
        case 5:
            return AggregateTarget::Subject;
        case 6:
            return AggregateTarget::Predicate;
        default:
            return AggregateTarget::Object;
        }
    }

    GroupBy random_group_by() {
        GroupBy group;
        switch (roll(6)) {
        case 0:
            group.field = GroupField::Subject;
            break;
        case 1:
            group.field = GroupField::Predicate;
            break;
        case 2:
            group.field = GroupField::Object;
            break;
        case 3:
            group.field = GroupField::Status;
            break;
        case 4:
            group.field = GroupField::ValidFromBucket;
            group.bucket_width = static_cast<Timestamp>(1 + roll(3)) * 1000;
            break;
        default:
            group.field = GroupField::ObservedAtBucket;
            group.bucket_width = static_cast<Timestamp>(1 + roll(3)) * 1000;
            break;
        }
        return group;
    }

    AggregateQuery random_aggregate() {
        AggregateQuery aggregate;

        aggregate.selection = random_query();
        // Row-shaping fields are rejected by an aggregate, so clear what random_query set.
        aggregate.selection.limit = 0;
        aggregate.selection.offset = 0;
        aggregate.selection.order = QueryOrder::AssertionId;
        aggregate.selection.newest_first = false;
        aggregate.selection.resolve_names = false;

        size_t group_count = roll(3); // 0, 1 or 2 group-by fields
        for (size_t i = 0; i < group_count; ++i) {
            aggregate.group_by.push_back(random_group_by());
        }

        size_t aggregation_count = 1 + roll(3);
        for (size_t i = 0; i < aggregation_count; ++i) {
            aggregate.aggregations.push_back({random_function(), random_target()});
        }

        return aggregate;
    }

    Query random_query() {
        Query query;

        if (roll(3) == 0) {
            query.subject = corpus.subjects[roll(corpus.subjects.size())];
        } else if (roll(8) == 0) {
            query.subject = 777'777; // never interned
        }

        if (roll(3) == 0) {
            query.predicate = corpus.predicates[roll(corpus.predicates.size())];
        }

        if (roll(4) == 0) {
            query.object = corpus.objects[roll(corpus.objects.size())];
        }

        if (roll(3) == 0) {
            query.valid_at = corpus.timestamps[roll(corpus.timestamps.size())];
        }

        if (roll(4) == 0) {
            query.observed_from = corpus.timestamps[roll(corpus.timestamps.size())];
        }

        if (roll(3) == 0) {
            query.observed_to = corpus.timestamps[roll(corpus.timestamps.size())];
        }

        query.open_ended_only = roll(4) == 0;

        if (roll(2) == 0) {
            size_t count = 1 + roll(3);
            for (size_t i = 0; i < count; ++i) {
                AssertionStatus status = random_status();
                if (std::find(query.statuses.begin(), query.statuses.end(), status) == query.statuses.end()) {
                    query.statuses.push_back(status);
                }
            }
        }

        if (roll(2) == 0) {
            query.filter = random_filter(1);
        }

        query.resolve_names = roll(4) == 0;

        switch (roll(3)) {
        case 0:
            query.order = QueryOrder::AssertionId;
            break;
        case 1:
            query.order = QueryOrder::ValidFrom;
            break;
        default:
            query.order = QueryOrder::ObservedAt;
            break;
        }

        query.newest_first = roll(2) == 0;
        query.limit = roll(13);
        query.offset = roll(7);

        return query;
    }
};

// --- the aggregate reference -----------------------------------------------------

Timestamp reference_bucket(Timestamp value, Timestamp width) {
    Timestamp quotient = value / width;
    if (value % width != 0 && value < 0) {
        --quotient;
    }
    return quotient * width;
}

Value reference_group_key(const Assertion &a, const GroupBy &group_by) {
    switch (group_by.field) {
    case GroupField::Subject:
        return Value::of_int64(static_cast<int64_t>(a.subject));
    case GroupField::Predicate:
        return Value::of_int64(static_cast<int64_t>(a.predicate));
    case GroupField::Object:
        return Value::of_int64(static_cast<int64_t>(a.object));
    case GroupField::Status:
        return Value::of_text(status_name(a.status));
    case GroupField::ValidFromBucket:
        return Value::of_timestamp(reference_bucket(a.valid_from, group_by.bucket_width));
    case GroupField::ObservedAtBucket:
        return Value::of_timestamp(reference_bucket(a.observed_at, group_by.bucket_width));
    }
    return Value::of_int64(0);
}

std::optional<Value> reference_target(const Assertion &a, AggregateTarget target, const KnowledgeKernel &kernel) {
    switch (target) {
    case AggregateTarget::ObjectValue:
        return kernel.entity_value(a.object);
    case AggregateTarget::Confidence:
        return Value::of_double(a.confidence);
    case AggregateTarget::ValidFrom:
        return Value::of_timestamp(a.valid_from);
    case AggregateTarget::ValidTo:
        return Value::of_timestamp(a.valid_to);
    case AggregateTarget::ObservedAt:
        return Value::of_timestamp(a.observed_at);
    case AggregateTarget::Subject:
        return Value::of_int64(static_cast<int64_t>(a.subject));
    case AggregateTarget::Predicate:
        return Value::of_int64(static_cast<int64_t>(a.predicate));
    case AggregateTarget::Object:
        return Value::of_int64(static_cast<int64_t>(a.object));
    }
    return std::nullopt;
}

std::optional<double> reference_number(const Assertion &a, AggregateTarget target, const KnowledgeKernel &kernel) {
    auto value = reference_target(a, target, kernel);
    if (!value.has_value()) {
        return std::nullopt;
    }
    if (value->kind == ValueKind::Int64) {
        return static_cast<double>(value->int64_value);
    }
    if (value->kind == ValueKind::Double) {
        return value->double_value;
    }
    if (value->kind == ValueKind::Timestamp) {
        return static_cast<double>(value->timestamp_value);
    }
    return std::nullopt;
}

bool reference_value_less(const Value &left, const Value &right) {
    if (left.kind != right.kind) {
        return static_cast<uint8_t>(left.kind) < static_cast<uint8_t>(right.kind);
    }
    return reference_compare(left, right) < 0;
}

struct ReferenceKeyLess {
    bool operator()(const std::vector<Value> &left, const std::vector<Value> &right) const {
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(), reference_value_less);
    }
};

AggregateResult reference_aggregate(const AggregateQuery &query, const Corpus &corpus, const KnowledgeKernel &kernel) {
    struct State {
        int64_t rows = 0;
        std::vector<int64_t> contributing;
        std::vector<double> sums;
        std::vector<double> minima;
        std::vector<double> maxima;
        std::vector<std::vector<Value>> seen; // distinct values, kept as a list for independence
    };

    std::map<std::vector<Value>, State, ReferenceKeyLess> groups;

    for (const auto &a : reference_matching_rows(query.selection, corpus, kernel)) {
        std::vector<Value> key;
        for (const auto &group_by : query.group_by) {
            key.push_back(reference_group_key(a, group_by));
        }

        auto &state = groups[key];
        if (state.contributing.empty()) {
            state.contributing.assign(query.aggregations.size(), 0);
            state.sums.assign(query.aggregations.size(), 0.0);
            state.minima.assign(query.aggregations.size(), 0.0);
            state.maxima.assign(query.aggregations.size(), 0.0);
            state.seen.assign(query.aggregations.size(), {});
        }
        ++state.rows;

        for (size_t i = 0; i < query.aggregations.size(); ++i) {
            const auto &aggregation = query.aggregations[i];

            if (aggregation.function == AggregateFunction::Count) {
                ++state.contributing[i];
                continue;
            }

            if (aggregation.function == AggregateFunction::CountDistinct) {
                auto value = reference_target(a, aggregation.target, kernel);
                if (value.has_value() &&
                    std::find(state.seen[i].begin(), state.seen[i].end(), *value) == state.seen[i].end()) {
                    state.seen[i].push_back(*value);
                }
                continue;
            }

            auto number = reference_number(a, aggregation.target, kernel);
            if (!number.has_value()) {
                continue;
            }
            if (state.contributing[i] == 0) {
                state.minima[i] = *number;
                state.maxima[i] = *number;
            } else {
                state.minima[i] = std::min(state.minima[i], *number);
                state.maxima[i] = std::max(state.maxima[i], *number);
            }
            state.sums[i] += *number;
            ++state.contributing[i];
        }
    }

    AggregateResult result;
    for (const auto &[key, state] : groups) {
        AggregateGroup group;
        group.key = key;
        group.row_count = state.rows;

        for (size_t i = 0; i < query.aggregations.size(); ++i) {
            AggregateCell cell;
            switch (query.aggregations[i].function) {
            case AggregateFunction::Count:
                cell.count = state.contributing[i];
                break;
            case AggregateFunction::CountDistinct:
                cell.count = static_cast<int64_t>(state.seen[i].size());
                break;
            case AggregateFunction::Sum:
                if (state.contributing[i] > 0) {
                    cell.number = state.sums[i];
                }
                break;
            case AggregateFunction::Min:
                if (state.contributing[i] > 0) {
                    cell.number = state.minima[i];
                }
                break;
            case AggregateFunction::Max:
                if (state.contributing[i] > 0) {
                    cell.number = state.maxima[i];
                }
                break;
            case AggregateFunction::Avg:
                if (state.contributing[i] > 0) {
                    cell.number = state.sums[i] / static_cast<double>(state.contributing[i]);
                }
                break;
            }
            group.values.push_back(cell);
        }

        result.groups.push_back(std::move(group));
    }

    return result;
}

std::vector<AssertionId> ids_of(const std::vector<Assertion> &assertions) {
    std::vector<AssertionId> ids;
    for (const auto &assertion : assertions) {
        ids.push_back(assertion.id);
    }
    return ids;
}

void randomized_queries_agree_across_index_scan_and_reference() {
    auto root = std::filesystem::temp_directory_path() / "query_differential_tests";
    std::filesystem::remove_all(root);

    KnowledgeKernel kernel(StorageConfig{root});
    Corpus corpus = populate(kernel);

    // The corpus has to be rich enough for the comparison to mean something.
    assert(corpus.assertions.size() > 120);

    Generator generator{std::mt19937(SEED), corpus};

    size_t non_empty = 0;

    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        Query query = generator.random_query();

        // Index selection and vectorized scanning are independent choices about *how* a query runs, so
        // every generated query is answered every way: through an index, through the columnar scan, and
        // through the row scan -- then all three against the reference.
        Query scan = query;
        scan.force_scan = true;

        Query rows_only = query;
        rows_only.force_scan = true;
        rows_only.force_row_scan = true;

        auto indexed_result = kernel.query(query);
        auto scanned_result = kernel.query(scan);
        auto row_result = kernel.query(rows_only);

        bool reference_truncated = false;
        auto expected = reference_answer(query, corpus, kernel, reference_truncated);

        auto indexed = ids_of(indexed_result.assertions);
        auto scanned = ids_of(scanned_result.assertions);
        auto row_scanned = ids_of(row_result.assertions);

        if (indexed != scanned || indexed != row_scanned || indexed != expected ||
            indexed_result.truncated != reference_truncated || row_result.truncated != reference_truncated) {
            // Ids, not just counts: a mutation test showed counts can agree while the rows differ, which
            // made the failure output actively misleading.
            auto show = [](const char *label, const std::vector<AssertionId> &ids, bool truncated) {
                std::cerr << "  " << label << " truncated=" << truncated << " ids=[";
                for (size_t n = 0; n < ids.size(); ++n) {
                    std::cerr << (n == 0 ? "" : ",") << ids[n];
                }
                std::cerr << "]\n";
            };

            std::cerr << "differential mismatch at query " << i << " (seed " << SEED << ")\n";
            show("indexed   ", indexed, indexed_result.truncated);
            show("columnar  ", scanned, scanned_result.truncated);
            show("row scan  ", row_scanned, row_result.truncated);
            show("reference ", expected, reference_truncated);
            assert(false && "randomized query disagreed across evaluation paths");
        }

        // resolve_names must not change which rows come back, only what rides along with them.
        if (query.resolve_names) {
            assert(indexed_result.names.size() == indexed_result.assertions.size());
        } else {
            assert(indexed_result.names.empty());
        }

        if (!indexed.empty()) {
            ++non_empty;
        }

        // Paging with a cursor (Phase 18) must produce exactly the rows the unpaged query produced, in
        // the same order -- against generated filters and orderings, not just the hand-written cases in
        // query_surface_tests.cpp. Run on a slice of the queries because each one costs several passes,
        // and only on untruncated ones, where "every row" is a knowable answer.
        if (i % 7 == 0 && !indexed_result.truncated && !indexed.empty() && query.offset == 0) {
            Query paged = query;
            paged.limit = 3;

            std::vector<AssertionId> walked;
            std::string cursor;
            for (size_t page = 0; page <= indexed.size(); ++page) {
                paged.cursor = cursor;
                QueryResult result = kernel.query(paged);
                for (const auto &assertion : result.assertions) {
                    walked.push_back(assertion.id);
                }
                if (result.next_cursor.empty()) {
                    break;
                }
                cursor = result.next_cursor;
            }

            if (walked != indexed) {
                std::cerr << "cursor walk disagreed with the unpaged answer at query " << i << " (seed " << SEED
                          << ")\n";
                assert(false && "cursor walk did not reproduce the unpaged result");
            }
        }
    }

    // Guards the generator itself: a corpus or generator that mostly produces empty answers would make
    // the whole suite vacuous while still passing.
    assert(non_empty > QUERY_COUNT / 10);

    std::filesystem::remove_all(root);
}

void randomized_aggregates_agree_across_index_scan_and_reference() {
    auto root = std::filesystem::temp_directory_path() / "query_differential_aggregate_tests";
    std::filesystem::remove_all(root);

    KnowledgeKernel kernel(StorageConfig{root});
    Corpus corpus = populate(kernel);

    Generator generator{std::mt19937(SEED + 1), corpus};

    size_t non_empty = 0;

    for (size_t i = 0; i < QUERY_COUNT; ++i) {
        AggregateQuery query = generator.random_aggregate();

        AggregateQuery scan = query;
        scan.selection.force_scan = true;

        AggregateQuery rows_only = query;
        rows_only.selection.force_scan = true;
        rows_only.selection.force_row_scan = true;

        auto indexed = kernel.aggregate(query);
        auto scanned = kernel.aggregate(scan);
        auto row_scanned = kernel.aggregate(rows_only);
        auto expected = reference_aggregate(query, corpus, kernel);

        auto same = [](const AggregateResult &left, const AggregateResult &right) {
            if (left.groups.size() != right.groups.size()) {
                return false;
            }
            for (size_t g = 0; g < left.groups.size(); ++g) {
                if (left.groups[g].key != right.groups[g].key ||
                    left.groups[g].row_count != right.groups[g].row_count ||
                    left.groups[g].values.size() != right.groups[g].values.size()) {
                    return false;
                }
                for (size_t c = 0; c < left.groups[g].values.size(); ++c) {
                    const auto &a = left.groups[g].values[c];
                    const auto &b = right.groups[g].values[c];
                    if (a.count != b.count || a.number.has_value() != b.number.has_value()) {
                        return false;
                    }
                    // Sums accumulate in the same order on both sides, so equality is exact; the
                    // tolerance is only insurance against a future reordering.
                    if (a.number.has_value() && std::abs(*a.number - *b.number) > 1e-9) {
                        return false;
                    }
                }
            }
            return true;
        };

        if (!same(indexed, scanned) || !same(indexed, row_scanned) || !same(indexed, expected)) {
            std::cerr << "aggregate mismatch at query " << i << " (seed " << SEED + 1 << ")\n"
                      << "  indexed   " << indexed.groups.size() << " groups\n"
                      << "  columnar  " << scanned.groups.size() << " groups\n"
                      << "  row scan  " << row_scanned.groups.size() << " groups\n"
                      << "  reference " << expected.groups.size() << " groups\n";
            assert(false && "randomized aggregate disagreed across evaluation paths");
        }

        if (!indexed.groups.empty()) {
            ++non_empty;
        }
    }

    assert(non_empty > QUERY_COUNT / 10);

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    randomized_queries_agree_across_index_scan_and_reference();
    randomized_aggregates_agree_across_index_scan_and_reference();

    std::cout << "All query_differential tests passed.\n";
    return 0;
}
