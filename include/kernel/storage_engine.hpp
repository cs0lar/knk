#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/assertion_log.hpp"
#include "kernel/current_index_log.hpp"
#include "kernel/ids.hpp"
#include "kernel/index_checkpoint.hpp"
#include "kernel/observed_time_index_log.hpp"
#include "kernel/snapshot_store.hpp"
#include "kernel/storage_config.hpp"
#include "kernel/subject_index_log.hpp"
#include "kernel/time.hpp"

namespace knk {

class StorageEngine {
  public:
    explicit StorageEngine(StorageConfig config);

    void append_assertion(const Assertion &assertion);

    std::vector<Assertion> load_assertions() const;

    std::vector<Assertion> load_assertions_after(AssertionId last_seen_id) const;

    AssertionId assertion_log_record_count_hint() const;

    void append_observed_time_entry(EntityId subject, Timestamp observed_at, AssertionId id);

    std::vector<ObservedTimeIndexRecord> load_observed_time_index() const;

    void rewrite_observed_time_index(const std::vector<ObservedTimeIndexRecord> &records);

    void append_subject_entry(EntityId subject, AssertionId id);

    std::vector<SubjectIndexRecord> load_subject_index() const;

    void rewrite_subject_index(const std::vector<SubjectIndexRecord> &records);

    void append_current_index_entry(EntityId subject, PredicateId predicate, AssertionId id, bool active);

    std::vector<CurrentIndexRecord> load_current_index() const;

    void rewrite_current_index(const std::vector<CurrentIndexRecord> &records);

    void write_checkpoint(AssertionId last_fully_indexed_id);

    AssertionId load_checkpoint() const;

    void write_snapshot(AssertionId last_snapshotted_id, const std::vector<Assertion> &assertions);

    std::optional<SnapshotData> load_snapshot() const;

    const StorageConfig &config() const;

  private:
    StorageConfig config_;
    AssertionLog assertion_log_;
    ObservedTimeIndexLog observed_time_index_log_;
    SubjectIndexLog subject_index_log_;
    CurrentIndexLog current_index_log_;
    IndexCheckpoint checkpoint_;
    SnapshotStore snapshot_store_;
};

} // namespace knk