#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/assertion_log.hpp"
#include "kernel/column_store.hpp"
#include "kernel/current_index_log.hpp"
#include "kernel/entity_catalog_log.hpp"
#include "kernel/entity_merge_log.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_checkpoint.hpp"
#include "kernel/observed_time_index_log.hpp"
#include "kernel/payload_store.hpp"
#include "kernel/predicate_catalog_log.hpp"
#include "kernel/provenance_log.hpp"
#include "kernel/snapshot_store.hpp"
#include "kernel/storage_config.hpp"
#include "kernel/storage_lock.hpp"
#include "kernel/subject_index_log.hpp"
#include "kernel/time.hpp"
#include "kernel/value.hpp"

namespace knk {

class StorageEngine {
  public:
    // A ReadOnly engine creates nothing and writes nothing: no lock file, no directories, and every
    // mutating method below throws. See OpenMode in storage_config.hpp for why a reader takes no lock.
    // Throws if a ReadOnly open names a root that does not exist -- there is nothing to read, and
    // creating it would be a write.
    explicit StorageEngine(StorageConfig config, OpenMode mode = OpenMode::ReadWrite);

    OpenMode mode() const;

    void append_assertion(const Assertion &assertion);

    // The batch counterparts of the four append_* methods below: each collapses what would be one
    // fsync per record into one per underlying log, which is the whole point of a batch commit (see
    // KnowledgeKernel::commit_batch). Coordination only -- like every other method here, these know
    // nothing about what the records mean.
    void append_assertions(std::span<const Assertion> assertions);

    // The columnar projection of the assertion log (Phase 14). Maintained in lockstep by
    // append_assertion/append_assertions above, which is why no commit path has to remember to do it,
    // and rebuilt from the log on open when it cannot be trusted. Purely derived: a caller that gets
    // empty spans back reads the log instead.
    size_t column_row_count() const;

    bool verify_columns() const;

    ColumnSpans map_columns();

    std::vector<Assertion> load_assertions() const;

    std::vector<Assertion> load_assertions_after(AssertionId last_seen_id) const;

    AssertionId assertion_log_record_count_hint() const;

    void archive_segments_before(AssertionId assertion_id);

    void append_observed_time_entry(EntityId subject, Timestamp observed_at, AssertionId id);

    void append_observed_time_entries(std::span<const ObservedTimeIndexRecord> records);

    std::vector<ObservedTimeIndexRecord> load_observed_time_index() const;

    void rewrite_observed_time_index(const std::vector<ObservedTimeIndexRecord> &records);

    void append_subject_entry(EntityId subject, AssertionId id);

    void append_subject_entries(std::span<const SubjectIndexRecord> records);

    std::vector<SubjectIndexRecord> load_subject_index() const;

    void rewrite_subject_index(const std::vector<SubjectIndexRecord> &records);

    void append_current_index_entry(EntityId subject, PredicateId predicate, AssertionId id, bool active);

    void append_current_index_entries(std::span<const CurrentIndexRecord> records);

    std::vector<CurrentIndexRecord> load_current_index() const;

    void rewrite_current_index(const std::vector<CurrentIndexRecord> &records);

    void write_checkpoint(AssertionId last_fully_indexed_id);

    AssertionId load_checkpoint() const;

    void write_snapshot(AssertionId last_snapshotted_id, const std::vector<Assertion> &assertions);

    std::optional<SnapshotData> load_snapshot() const;

    void append_entity_catalog_entry(EntityId id, const Value &value);

    std::vector<EntityCatalogRecord> load_entity_catalog() const;

    void append_predicate_catalog_entry(PredicateId id, const std::string &name);

    std::vector<PredicateCatalogRecord> load_predicate_catalog() const;

    void append_entity_merge_entry(EntityId absorbed, EntityId surviving, Timestamp merged_at);

    std::vector<EntityMergeRecord> load_entity_merges() const;

    void write_payload(EntityId id, std::span<const std::byte> content);

    std::optional<std::vector<std::byte>> load_payload(EntityId id) const;

    std::vector<EntityId> existing_payload_ids() const;

    void append_provenance_entry(AssertionId assertion_id, EntityId source, Timestamp recorded_at,
                                 const std::string &method);

    void append_provenance_entries(std::span<const ProvenanceRecord> records);

    std::vector<ProvenanceRecord> load_provenance() const;

    const StorageConfig &config() const;

  private:
    // Throws when this engine was opened ReadOnly, naming the operation. The layer that owns the files
    // is the layer that refuses to write them; KnowledgeKernel guards its own public methods as well,
    // so a rejected call never half-mutates in-memory state first.
    void require_writable(const char *operation) const;

    // Brings the columnar store in line with the assertion log: rebuild when it cannot be verified,
    // append the missing tail when it merely lags. Never called for a ReadOnly open, because both of
    // those are writes.
    void ensure_columns_current();

    StorageConfig config_;
    OpenMode mode_;
    // Declared before every log member so it is constructed first and released last, enforcing the
    // single-writer model for the object's entire lifetime -- see kernel/storage_lock.hpp. Empty for a
    // ReadOnly open, which takes no lock; that absence is what lets readers coexist with the writer.
    std::optional<StorageLock> storage_lock_;
    AssertionLog assertion_log_;
    ObservedTimeIndexLog observed_time_index_log_;
    SubjectIndexLog subject_index_log_;
    CurrentIndexLog current_index_log_;
    IndexCheckpoint checkpoint_;
    SnapshotStore snapshot_store_;
    EntityCatalogLog entity_catalog_log_;
    PredicateCatalogLog predicate_catalog_log_;
    EntityMergeLog entity_merge_log_;
    PayloadStore payload_store_;
    ProvenanceLog provenance_log_;

    // Declared last among the durable members: it is rebuilt from assertion_log_ during construction, so
    // that one must already exist.
    ColumnStore column_store_;
};

} // namespace knk