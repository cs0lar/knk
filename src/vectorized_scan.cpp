#include <algorithm>

#include "kernel/status.hpp"
#include "kernel/vectorized_scan.hpp"

namespace knk {

namespace {

// Each predicate narrows a byte mask in its own pass. Written as `mask[i] = mask[i] & condition` rather
// than `if (condition) ...` on purpose: no branch per row, one column streamed per pass, which is the
// shape a compiler can turn into vector instructions. The cost is several passes over one byte per row
// instead of one pass over an 88-byte record.

void narrow_equal(std::vector<uint8_t> &mask, std::span<const EntityId> column, EntityId wanted) {
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = static_cast<uint8_t>(mask[i] & static_cast<uint8_t>(column[i] == wanted));
    }
}

void narrow_open_ended(std::vector<uint8_t> &mask, std::span<const Timestamp> valid_to) {
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = static_cast<uint8_t>(mask[i] & static_cast<uint8_t>(valid_to[i] == OPEN_ENDED));
    }
}

// valid_from <= t < valid_to, with OPEN_ENDED meaning no end -- the same arithmetic valid_at() applies,
// expressed without a branch.
void narrow_valid_at(std::vector<uint8_t> &mask, std::span<const Timestamp> valid_from,
                     std::span<const Timestamp> valid_to, Timestamp point) {
    for (size_t i = 0; i < mask.size(); ++i) {
        uint8_t starts = static_cast<uint8_t>(valid_from[i] <= point);
        uint8_t ends = static_cast<uint8_t>(valid_to[i] == OPEN_ENDED || point < valid_to[i]);
        mask[i] = static_cast<uint8_t>(mask[i] & starts & ends);
    }
}

void narrow_observed_from(std::vector<uint8_t> &mask, std::span<const Timestamp> observed_at, Timestamp bound) {
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = static_cast<uint8_t>(mask[i] & static_cast<uint8_t>(observed_at[i] >= bound));
    }
}

void narrow_observed_to(std::vector<uint8_t> &mask, std::span<const Timestamp> observed_at, Timestamp bound) {
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = static_cast<uint8_t>(mask[i] & static_cast<uint8_t>(observed_at[i] <= bound));
    }
}

// A five-entry lookup keyed by the status byte, so an arbitrary status *set* costs the same as testing
// one status: no inner loop over the requested statuses, no branch.
void narrow_status(std::vector<uint8_t> &mask, std::span<const uint8_t> effective_status,
                   const std::vector<AssertionStatus> &statuses) {
    std::array<uint8_t, 5> allowed{};
    for (AssertionStatus status : statuses) {
        auto index = static_cast<size_t>(status);
        if (index < allowed.size()) {
            allowed[index] = 1;
        }
    }

    for (size_t i = 0; i < mask.size(); ++i) {
        uint8_t status = effective_status[i];
        uint8_t ok = status < allowed.size() ? allowed[status] : 0;
        mask[i] = static_cast<uint8_t>(mask[i] & ok);
    }
}

} // namespace

bool columns_usable(const ColumnSpans &columns, std::span<const uint8_t> effective_status, size_t row_count) {
    if (columns.empty() || row_count == 0) {
        return false;
    }

    // Every span has to describe exactly the rows being queried. Anything else -- a store that lags the
    // log, a read-only open with no columns at all -- means the row path answers instead.
    return columns.subject.size() == row_count && columns.predicate.size() == row_count &&
           columns.object.size() == row_count && columns.valid_from.size() == row_count &&
           columns.valid_to.size() == row_count && columns.observed_at.size() == row_count &&
           effective_status.size() == row_count;
}

void vectorized_select(const Query &query, const ColumnSpans &columns, std::span<const uint8_t> effective_status,
                       std::optional<EntityId> subject, std::optional<EntityId> object,
                       std::vector<uint32_t> &selection) {
    const size_t rows = columns.subject.size();
    std::vector<uint8_t> mask(rows, 1);

    // Ordered cheapest-and-most-selective first where it is knowable: an id equality usually discards
    // most rows, so later passes still stream their whole column but the final compaction is short.
    if (subject.has_value()) {
        narrow_equal(mask, columns.subject, *subject);
    }

    if (query.predicate.has_value()) {
        narrow_equal(mask, columns.predicate, *query.predicate);
    }

    if (object.has_value()) {
        narrow_equal(mask, columns.object, *object);
    }

    if (!query.statuses.empty()) {
        narrow_status(mask, effective_status, query.statuses);
    }

    if (query.open_ended_only) {
        narrow_open_ended(mask, columns.valid_to);
    }

    if (query.valid_at.has_value()) {
        narrow_valid_at(mask, columns.valid_from, columns.valid_to, *query.valid_at);
    }

    if (query.observed_from.has_value()) {
        narrow_observed_from(mask, columns.observed_at, *query.observed_from);
    }

    if (query.observed_to.has_value()) {
        narrow_observed_to(mask, columns.observed_at, *query.observed_to);
    }

    for (size_t i = 0; i < rows; ++i) {
        if (mask[i] != 0) {
            selection.push_back(static_cast<uint32_t>(i));
        }
    }
}

} // namespace knk
