#pragma once

// Keyset pagination tokens (Phase 18; see AGENTS.md's "Query Engine" section).
//
// Every QueryOrder breaks ties on AssertionId, so (order key, id) is a *total* order over a result: no
// two rows compare equal. That is the whole reason a cursor can be exact -- "resume strictly after this
// (key, id)" names one position, so paging cannot repeat a row or skip one, which offset-based paging
// cannot promise once rows are being committed underneath it.
//
// The token is opaque by contract: callers pass back what QueryResult::next_cursor gave them and never
// construct one. It is plain text rather than an encoded blob only so a failing page is debuggable from
// a log line -- the version prefix is what lets the encoding change without a caller noticing.

#include <cstdint>
#include <string>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/query.hpp"
#include "kernel/time.hpp"

namespace knk {

// Bumped if the token's meaning changes. A token from another version is rejected, never guessed at --
// the same rule QUERY_IR_VERSION follows, for the same reason.
constexpr uint32_t QUERY_CURSOR_VERSION = 1;

struct QueryCursor {
    // The ordering the token was produced under. Carried, not assumed: resuming an ascending walk with a
    // descending cursor would silently return the wrong window, so the mismatch is rejected instead.
    QueryOrder order = QueryOrder::AssertionId;
    bool newest_first = false;

    int64_t key = 0; // the ordering field's value on the last returned row
    AssertionId id = 0;
};

// The value `order` sorts on. int64 covers all three: two are Timestamps, and an AssertionId large enough
// to overflow one would need more rows than MAX_SPILL_ROWS allows a thousand times over.
int64_t cursor_key(const Assertion &assertion, QueryOrder order);

std::string encode_query_cursor(const QueryCursor &cursor);

// Throws std::runtime_error if the token is malformed or carries an unknown version. A caller-supplied
// token is caller-supplied input, so it is validated like one.
QueryCursor decode_query_cursor(const std::string &token);

// True when `assertion` falls strictly after `cursor` in the given ordering, i.e. belongs to a later page.
bool after_query_cursor(const QueryCursor &cursor, const Assertion &assertion);

} // namespace knk
