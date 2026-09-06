#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/time.hpp"

namespace knk {

struct ProvenanceRecord {
    AssertionId assertion_id;
    EntityId source;
    Timestamp recorded_at;
    std::string method;
};

// Durable side-log of provenance for committed assertions: which source (an EntityId interned in
// Catalog exactly like any other entity) produced a given assertion, and by what method. Keyed by
// AssertionId rather than being a field on Assertion, so the raw-struct on-disk assertion format is
// untouched. Authoritative, like the catalog logs -- assertions.log never encodes provenance, so
// there is nothing to rebuild this from. There is deliberately no overwrite_all: non-tail corruption
// here is fatal (see read_all), matching AssertionLog's philosophy rather than the self-healing
// index logs.
class ProvenanceLog {
  public:
    explicit ProvenanceLog(std::filesystem::path path);

    void append(const ProvenanceRecord &record);

    // Appends every record with one file open and one fsync, instead of one of each per record --
    // the provenance half of a batch commit's cost (see KnowledgeKernel::record_provenance_batch).
    // Records are written in the order given; a crash before the fsync returns can leave any prefix
    // of them durable, which read_all already handles as an ordinary torn trailing frame. An empty
    // span writes nothing at all, rather than creating a header-only file.
    void append_batch(std::span<const ProvenanceRecord> records);

    std::vector<ProvenanceRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
