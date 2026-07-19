#pragma once

#include <filesystem>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/time.hpp"

namespace knk {

struct EntityMergeRecord {
    EntityId absorbed;
    EntityId surviving;
    Timestamp merged_at;
};

// Durable, append-only log of entity-merge redirects backing Catalog::resolve/
// KnowledgeKernel::merge_entities. Like EntityCatalogLog/PredicateCatalogLog/ProvenanceLog, this log
// is authoritative, not a derived/rebuildable index -- assertions.log never records that two
// EntityIds were merged, so there is nothing to replay this mapping from. There is deliberately no
// overwrite_all: non-tail corruption here is fatal (see read_all), matching AssertionLog's philosophy
// rather than the self-healing Phase 3 index logs. Fixed-size record, same framing style as
// ObservedTimeIndexLog.
class EntityMergeLog {
  public:
    explicit EntityMergeLog(std::filesystem::path path);

    void append(const EntityMergeRecord &record);

    std::vector<EntityMergeRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
