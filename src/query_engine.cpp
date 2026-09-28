#include <algorithm>
#include <stdexcept>

#include "kernel/query_engine.hpp"

namespace knk {

namespace {

// True when the query asks for exactly what the current-state indexes store: Active status and an
// open-ended valid interval, i.e. is_current_assertion's definition. Only then may those indexes be
// used as a candidate source, because they deliberately hold nothing else -- an assertion drops out of
// them the moment it is superseded or retracted, so a query wanting any other status would silently
// lose rows.
bool is_current_shaped(const Query &query) {
    return query.open_ended_only && query.statuses.size() == 1 && query.statuses.front() == AssertionStatus::Active;
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

} // namespace

QueryEngine::QueryEngine(const std::vector<Assertion> &assertions, const IndexManager &index_manager,
                         const Catalog &catalog)
    : assertions_(assertions), index_manager_(index_manager), catalog_(catalog) {}

const Assertion *QueryEngine::find_by_id(AssertionId id) const {
    if (id == 0 || id > assertions_.size()) {
        return nullptr;
    }

    // assertions_ is dense and id-ordered (ids start at 1 and every commit appends exactly one), the
    // same indexing KnowledgeKernel::get relies on.
    return &assertions_[id - 1];
}

std::vector<AssertionId> QueryEngine::candidate_ids(const Query &query, std::optional<EntityId> subject,
                                                    std::optional<EntityId> object) const {
    if (query.force_scan) {
        return {};
    }

    // The subject index holds every assertion for a subject regardless of status, so it is safe for any
    // query that names one.
    if (subject.has_value()) {
        return index_manager_.assertions_for_subject(*subject);
    }

    // The object and predicate indexes are current-only, hence the is_current_shaped guard.
    if (is_current_shaped(query)) {
        if (object.has_value()) {
            return index_manager_.current_assertions_by_object(*object);
        }

        if (query.predicate.has_value()) {
            return index_manager_.current_assertions_by_predicate(*query.predicate);
        }
    }

    return {};
}

bool QueryEngine::matches(const Query &query, const Assertion &assertion, std::optional<EntityId> subject,
                          std::optional<EntityId> object) const {
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

    return true;
}

QueryResult QueryEngine::execute(const Query &query) const {
    if (query.ir_version != QUERY_IR_VERSION) {
        throw std::runtime_error("unsupported query IR version");
    }

    // Resolved once, then compared against raw stored ids -- see matches().
    std::optional<EntityId> subject;
    if (query.subject.has_value()) {
        subject = catalog_.resolve(*query.subject);
    }

    std::optional<EntityId> object;
    if (query.object.has_value()) {
        object = catalog_.resolve(*query.object);
    }

    std::vector<Assertion> matched;

    auto candidates = candidate_ids(query, subject, object);
    if (candidates.empty()) {
        // Either no index applied or the index genuinely held nothing; a full scan is correct in both
        // cases, and at Phase 10's scale it is also what changes_since already does.
        for (const auto &assertion : assertions_) {
            if (matches(query, assertion, subject, object)) {
                matched.push_back(assertion);
            }
        }
    } else {
        for (AssertionId id : candidates) {
            const Assertion *assertion = find_by_id(id);
            if (assertion != nullptr && matches(query, *assertion, subject, object)) {
                matched.push_back(*assertion);
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

    return result;
}

} // namespace knk
