#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "kernel/ids.hpp"

namespace knk {

// Durable one-file-per-payload store for arbitrary-size byte content addressed by EntityId, used
// for content that doesn't fit the Catalog's small fixed-ish framed-log records (whole documents,
// arbitrary blobs). Like EntityCatalogLog/PredicateCatalogLog, this is authoritative, not a
// derived/rebuildable index -- assertions.log never stores payload content, so there is nothing to
// replay this from. Unlike the append-only logs, each payload file is always written in full via
// write_file_atomically (never appended to), so a present file is guaranteed either fully-formed or
// entirely absent; any anomaly found in a present file is therefore genuine corruption, not an
// ordinary crash-mid-append artifact, and read() always throws rather than tail-tolerating it.
class PayloadStore {
  public:
    explicit PayloadStore(std::filesystem::path directory);

    void write(EntityId id, std::span<const std::byte> content);

    // Returns nullopt only if no payload was ever written for `id`. Throws std::runtime_error if a
    // payload file exists but is corrupt (bad header, length/content mismatch, checksum mismatch).
    std::optional<std::vector<std::byte>> read(EntityId id) const;

    // Lists the EntityIds of every payload currently on disk, discovered by scanning `directory`'s
    // filenames -- there is no separate manifest, matching the segmented assertion log's precedent
    // of deriving everything from directory contents.
    std::vector<EntityId> existing_ids() const;

  private:
    std::filesystem::path directory_;

    std::filesystem::path path_for(EntityId id) const;
};

} // namespace knk
