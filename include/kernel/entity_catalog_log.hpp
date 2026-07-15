#pragma once

#include <filesystem>
#include <vector>

#include "kernel/ids.hpp"
#include "kernel/value.hpp"

namespace knk {

struct EntityCatalogRecord {
    EntityId id;
    Value value;
};

// Durable log of EntityId <-> Value mappings. Unlike the Phase 3 index logs, this log is
// authoritative, not a derived/rebuildable index -- assertions.log never stores what an EntityId
// means, so there is nothing to replay this mapping from. There is deliberately no overwrite_all:
// non-tail corruption here is fatal (see read_all), matching AssertionLog's philosophy rather than
// the self-healing index logs.
class EntityCatalogLog {
  public:
    explicit EntityCatalogLog(std::filesystem::path path);

    void append(const EntityCatalogRecord &record);

    std::vector<EntityCatalogRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
