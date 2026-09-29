#pragma once

// Batch-at-a-time selection over the columnar store (Phase 15).
//
// The row path evaluates every predicate of a query against one assertion, then moves to the next
// assertion: one pass, but each step touches an 88-byte record to read one or two fields of it, and the
// per-predicate branches are re-taken for every row. This does the transpose -- one pass per predicate
// over one contiguous column, each writing into a byte mask -- so every loop is branch-free, touches
// only the bytes its predicate needs, and is shaped for the compiler to auto-vectorize.
//
// Only the predicates that can be answered from columns live here: the id selectors, the valid-time
// point, the observed-time window, open-endedness, and the status set. A Query::filter can consult the
// catalog and has arbitrary boolean structure, so it stays a per-row test applied to the *survivors* --
// which is the point of doing the cheap, vectorizable predicates first.
//
// Effective status is passed in rather than read from the store, because the columns are a verbatim
// projection of the log: a superseded row's status byte still says Active (see column_store.hpp). The
// kernel keeps the replayed, effective status in a parallel byte array for exactly this.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "kernel/column_store.hpp"
#include "kernel/ids.hpp"
#include "kernel/query.hpp"

namespace knk {

// True when `columns` and `effective_status` describe the same rows the caller is querying, which is the
// precondition for using them at all. A mismatch is not an error -- a read-only open against a root with
// no columns is perfectly ordinary -- it just means the row path answers instead.
bool columns_usable(const ColumnSpans &columns, std::span<const uint8_t> effective_status, size_t row_count);

// Appends the indices of rows passing the query's columnar predicates, in ascending order. `subject` and
// `object` are already resolved through merge redirects by the caller.
void vectorized_select(const Query &query, const ColumnSpans &columns, std::span<const uint8_t> effective_status,
                       std::optional<EntityId> subject, std::optional<EntityId> object,
                       std::vector<uint32_t> &selection);

} // namespace knk
