#pragma once

// As-of reconstruction (Phase 19; see AGENTS.md's "Current Roadmap").
//
// `known_at` answers "which facts had we observed by time T" but filters on status **now**, so once a
// correction lands it can no longer tell you what you believed then. "What did our numbers look like at
// close" has been documented as a non-answer in docs/query_semantics.md since Phase 10. This is the
// answer.
//
// The log already holds everything needed, because `supersedes_id`/`retracts_id` live on the *later*
// record: an assertion was Active as of commit N unless some record at or before N superseded or
// retracted it. Rather than scan for that record per query, the kernel keeps the reverse link -- for each
// row, the id of the record that closed it -- so reconstruction is O(1) per row and needs no durable
// state of its own. Like every other in-memory structure, it is rebuilt by replaying the log.
//
// Memory is 17 bytes per assertion (one status byte, two ids) against ~88 for the Assertion itself. That
// is the price of answering as-of queries at all; it buys exactly the ids a scan would otherwise have to
// go looking for.

#include <cstdint>
#include <span>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/ids.hpp"
#include "kernel/status.hpp"
#include "kernel/time.hpp"

namespace knk {

// Parallel to KnowledgeKernel::assertions_, indexed by row (id - 1).
struct StatusHistory {
    // The status the record was *appended* with -- Active, Hypothesis, or Retraction. Superseded and
    // Retracted are never appended; they are only ever applied to an earlier record by a later one, which
    // is why they can be reconstructed and this cannot. Needed separately from the effective status
    // because a superseded *hypothesis* reads as Superseded now, and "Active" would be the wrong thing to
    // reconstruct it back to.
    std::vector<uint8_t> appended;

    // Id of the record that superseded / retracted this row, or 0 for neither. A row can be both over its
    // lifetime, which is why these are two fields and not one: as of a commit between the two events, the
    // row is whatever the earlier event made it.
    std::vector<AssertionId> superseded_by;
    std::vector<AssertionId> retracted_by;

    size_t size() const { return appended.size(); }

    void clear() {
        appended.clear();
        superseded_by.clear();
        retracted_by.clear();
    }

    void push(AssertionStatus status) {
        appended.push_back(static_cast<uint8_t>(status));
        superseded_by.push_back(0);
        retracted_by.push_back(0);
    }

    // Status of `row` as of commit `commit`, i.e. considering only records with id <= commit. The caller
    // is responsible for not asking about a row that did not exist yet (id > commit); see
    // QueryEngine, which filters those out before asking.
    AssertionStatus as_of_commit(size_t row, AssertionId commit) const {
        AssertionId superseded = superseded_by[row];
        AssertionId retracted = retracted_by[row];

        bool was_superseded = superseded != 0 && superseded <= commit;
        bool was_retracted = retracted != 0 && retracted <= commit;

        if (!was_superseded && !was_retracted) {
            return static_cast<AssertionStatus>(appended[row]);
        }

        // Later event wins, which is exactly what replaying the log in order would have produced.
        return superseded > retracted ? AssertionStatus::Superseded : AssertionStatus::Retracted;
    }

    // Status of `row` as of observed time `observed`: the correction counts only if the kernel had
    // observed it by then. `assertions` is needed to read the closing record's own observed_at -- stored
    // nowhere here, because that would be sixteen more bytes per row to avoid a lookup that only happens
    // for rows something actually closed.
    //
    // observed_at is caller-supplied and need not increase with commit id, so a backdated correction can
    // close a row "before" the row itself was observed. That follows from what observed_at means and is
    // the same latitude known_at already gives; see docs/query_semantics.md.
    AssertionStatus as_of_observed(size_t row, Timestamp observed, std::span<const Assertion> assertions) const {
        AssertionId superseded = superseded_by[row];
        AssertionId retracted = retracted_by[row];

        bool was_superseded = superseded != 0 && assertions[superseded - 1].observed_at <= observed;
        bool was_retracted = retracted != 0 && assertions[retracted - 1].observed_at <= observed;

        if (!was_superseded && !was_retracted) {
            return static_cast<AssertionStatus>(appended[row]);
        }

        if (was_superseded && was_retracted) {
            // Both already observed: the one observed later is what we believed, falling back to commit
            // order when they were observed at the same moment.
            Timestamp superseded_at = assertions[superseded - 1].observed_at;
            Timestamp retracted_at = assertions[retracted - 1].observed_at;
            if (superseded_at != retracted_at) {
                return superseded_at > retracted_at ? AssertionStatus::Superseded : AssertionStatus::Retracted;
            }
            return superseded > retracted ? AssertionStatus::Superseded : AssertionStatus::Retracted;
        }

        return was_superseded ? AssertionStatus::Superseded : AssertionStatus::Retracted;
    }
};

} // namespace knk
