#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "kernel/ids.hpp"

namespace knk {

struct PredicateCatalogRecord {
    PredicateId id;
    std::string name;
};

// Durable log of PredicateId <-> name mappings. Authoritative, like EntityCatalogLog -- see that
// class's comment for why there is no overwrite_all/self-heal here.
class PredicateCatalogLog {
  public:
    explicit PredicateCatalogLog(std::filesystem::path path);

    void append(const PredicateCatalogRecord &record);

    std::vector<PredicateCatalogRecord> read_all() const;

  private:
    std::filesystem::path path_;
};

} // namespace knk
