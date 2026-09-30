#pragma once

// Writing a query result to disk as columns, for results too large to hand back as JSON-RPC text
// (Phase 17). The MCP tool returns a descriptor -- where the files are, what is in them -- instead of
// rows, and the caller reads the bytes directly.
//
// **Format decision, and it reverses the recommendation in #57.** That plan proposed hand-written Arrow
// IPC, on the grounds of interoperability without a dependency. Implementing it surfaced the fact the
// plan had glossed over: Arrow IPC's metadata is a *FlatBuffer*, not a header, so "write the spec by
// hand" means implementing FlatBuffer encoding -- vtables, offsets, alignment -- with no Arrow
// implementation available to check the result against. Neither this environment nor CI (ubuntu-latest,
// C++ only) can install pyarrow, so the only test possible would be a hand-written reader agreeing with
// the hand-written writer, which proves nothing about whether DuckDB or Polars can read the file. An
// interoperability claim that cannot be tested is one a user discovers is false.
//
// So the format here is knk's own, and deliberately the dullest thing that works: one file per column,
// fixed-width native-endian values, no headers, everything described in `descriptor.json`. A reader is
// about twenty lines in any language -- `tools/read_spill.py` is the reference one -- and converting to
// Arrow is a few lines *on the consumer's side*, where a real Arrow implementation exists and is tested.
// Vendoring Arrow C++ stays rejected on size alone.
//
// The status written is the **effective** one, not the appended one the columnar store holds: a spill is a
// query result, so it must say what a query says. That falls out of reading rows -- assertions_ carries
// replayed status -- which is why this takes rows rather than columns.
//
// Spills are never written under the storage root. A read-only kernel is exactly the analytics case, and
// it must write nothing to the root it opened; the spill directory is the caller's, somewhere it owns.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/catalog.hpp"
#include "kernel/ids.hpp"

namespace knk {

// Ceiling on rows one spill may write. The point of spilling is to escape the JSON row ceiling, so this
// is generous -- but an accidentally unbounded spill filling a disk is a real failure mode, and every
// other caller-facing bound in the kernel exists for the same reason.
constexpr size_t MAX_SPILL_ROWS = 10'000'000;

struct SpillColumn {
    std::string name;
    std::string file;

    // "uint64", "int64", "double" or "uint8" -- named as a consumer would spell them, so a descriptor can
    // be turned into numpy dtypes or Arrow types without a lookup table.
    std::string type;

    size_t bytes_per_value = 0;
};

struct SpillDescriptor {
    std::string token;
    std::filesystem::path directory;
    size_t rows = 0;
    std::vector<SpillColumn> columns;

    // Distinct ids appearing in the result, mapped to their catalog names/values. O(distinct ids) rather
    // than O(rows), and it exists because a column of ids is useless to an analytics consumer without it
    // -- the alternative was making them join back through entity_name_batch a page at a time.
    std::string dictionary_file;
};

// Writes the rows `selection` names (indices into `assertions`, in result order) as columns under
// `directory / token`, with a descriptor and a dictionary. Throws if the token already exists -- silently
// overwriting a result someone may be reading is worse than refusing -- or if the selection exceeds
// MAX_SPILL_ROWS.
SpillDescriptor write_spill(const std::filesystem::path &directory, const std::string &token,
                            std::span<const uint32_t> selection, std::span<const Assertion> assertions,
                            const Catalog &catalog);

// Removes a spill directory. Nothing sweeps automatically: the kernel never reads a wall clock (so a TTL
// is not available to it), and deleting a result someone is still reading would be worse than leaving a
// file behind. Lifecycle is the caller's, which is why the directory is theirs too.
void drop_spill(const std::filesystem::path &directory, const std::string &token);

} // namespace knk
