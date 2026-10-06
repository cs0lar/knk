#include <stdexcept>
#include <string>

#include "kernel/storage_engine.hpp"
#include "kernel/subject_index_log.hpp"

namespace knk {

StorageEngine::StorageEngine(StorageConfig config, OpenMode mode)
    : config_(std::move(config)), mode_(mode),
      assertion_log_(config_.segment_directory(), config_.max_records_per_segment),
      observed_time_index_log_(config_.observed_time_index_path()), subject_index_log_(config_.subject_index_path()),
      current_index_log_(config_.current_index_path()), checkpoint_(config_.checkpoint_path()),
      snapshot_store_(config_.snapshot_path()), entity_catalog_log_(config_.entity_catalog_path()),
      predicate_catalog_log_(config_.predicate_catalog_path()), payload_store_(config_.payload_directory()),
      provenance_log_(config_.provenance_log_path()), entity_merge_log_(config_.entity_merge_log_path()),
      column_store_(config_.column_directory()) {
    if (mode_ == OpenMode::ReadOnly) {
        // Nothing is created and no lock is taken: a reader that conjured the layout into existence
        // would be writing, and a root that does not exist has nothing to read.
        if (!std::filesystem::exists(config_.root)) {
            throw std::runtime_error("storage root '" + config_.root.string() +
                                     "' does not exist (opened read-only, so it will not be created)");
        }
        return;
    }

    // The writer lock is taken here rather than in the member initializer list so that the ReadOnly
    // branch above can skip it entirely; it still outlives every log member, since those are only ever
    // touched through this object.
    storage_lock_.emplace(config_.root);

    // config_.root itself is created by StorageLock's constructor.
    std::filesystem::create_directories(config_.index_directory());
    std::filesystem::create_directories(config_.payload_directory());
    std::filesystem::create_directories(config_.catalog_directory());
    std::filesystem::create_directories(config_.provenance_directory());

    ensure_columns_current();
}

void StorageEngine::ensure_columns_current() {
    // Derived state, so the recovery rules are the Phase 3 index rules rather than the log's: never
    // fatal, always rebuildable. A store that cannot be verified is rebuilt wholesale, which is far
    // easier to reason about than repairing columns that disagree with each other; a store that merely
    // lags the log (the crash-between-appends case) just gets the missing tail.
    if (!column_store_.verify()) {
        column_store_.overwrite_all(assertion_log_.read_all());
        return;
    }

    // row_count is also the last assertion id, since rows are dense in id order.
    auto tail = assertion_log_.read_after(static_cast<AssertionId>(column_store_.row_count()));
    if (!tail.empty()) {
        column_store_.append(tail);
    }
}

size_t StorageEngine::column_row_count() const { return column_store_.row_count(); }

bool StorageEngine::verify_columns() const { return column_store_.verify(); }

ColumnSpans StorageEngine::map_columns() const { return column_store_.map(); }

OpenMode StorageEngine::mode() const { return mode_; }

void StorageEngine::require_writable(const char *operation) const {
    if (mode_ == OpenMode::ReadOnly) {
        throw std::runtime_error(std::string("storage root '") + config_.root.string() + "' is open read-only; " +
                                 operation + " is not allowed");
    }
}

void StorageEngine::append_assertion(const Assertion &assertion) {
    require_writable("append_assertion");
    assertion_log_.append(assertion);

    // After the log, always: the log is the source of truth, and a crash between the two leaves columns
    // lagging, which the next open heals. The reverse order would put derived state ahead of it.
    column_store_.append(std::span<const Assertion>(&assertion, 1));
}

void StorageEngine::append_assertions(std::span<const Assertion> assertions) {
    require_writable("append_assertions");

    assertion_log_.append_batch(assertions);
    column_store_.append(assertions);
}

std::vector<Assertion> StorageEngine::load_assertions() const { return assertion_log_.read_all(); }

std::vector<Assertion> StorageEngine::load_assertions_after(AssertionId last_seen_id) const {
    return assertion_log_.read_after(last_seen_id);
}

AssertionId StorageEngine::assertion_log_record_count_hint() const { return assertion_log_.record_count_hint(); }

void StorageEngine::archive_segments_before(AssertionId assertion_id) {
    require_writable("archive_segments_before");

    assertion_log_.archive_segments_before(assertion_id);
}

void StorageEngine::append_observed_time_entry(EntityId subject, Timestamp observed_at, AssertionId id) {
    require_writable("append_observed_time_entry");

    observed_time_index_log_.append(ObservedTimeIndexRecord{subject, observed_at, id});
}

void StorageEngine::append_observed_time_entries(std::span<const ObservedTimeIndexRecord> records) {
    require_writable("append_observed_time_entries");

    observed_time_index_log_.append_batch(records);
}

std::vector<ObservedTimeIndexRecord> StorageEngine::load_observed_time_index() const {
    return observed_time_index_log_.read_all();
}

void StorageEngine::rewrite_observed_time_index(const std::vector<ObservedTimeIndexRecord> &records) {
    require_writable("rewrite_observed_time_index");

    observed_time_index_log_.overwrite_all(records);
}

void StorageEngine::append_subject_entry(EntityId subject, AssertionId id) {
    require_writable("append_subject_entry");

    subject_index_log_.append(SubjectIndexRecord(subject, id));
}

void StorageEngine::append_subject_entries(std::span<const SubjectIndexRecord> records) {
    require_writable("append_subject_entries");

    subject_index_log_.append_batch(records);
}

std::vector<SubjectIndexRecord> StorageEngine::load_subject_index() const { return subject_index_log_.read_all(); }

void StorageEngine::rewrite_subject_index(const std::vector<SubjectIndexRecord> &records) {
    require_writable("rewrite_subject_index");

    subject_index_log_.overwrite_all(records);
}

void StorageEngine::append_current_index_entry(EntityId subject, PredicateId predicate, AssertionId id, bool active) {
    require_writable("append_current_index_entry");

    current_index_log_.append(CurrentIndexRecord{subject, predicate, id, active});
}

void StorageEngine::append_current_index_entries(std::span<const CurrentIndexRecord> records) {
    require_writable("append_current_index_entries");

    current_index_log_.append_batch(records);
}

std::vector<CurrentIndexRecord> StorageEngine::load_current_index() const { return current_index_log_.read_all(); }

void StorageEngine::rewrite_current_index(const std::vector<CurrentIndexRecord> &records) {
    require_writable("rewrite_current_index");

    current_index_log_.overwrite_all(records);
}

void StorageEngine::write_checkpoint(AssertionId last_fully_indexed_id) {
    require_writable("write_checkpoint");
    checkpoint_.write(last_fully_indexed_id);
}

AssertionId StorageEngine::load_checkpoint() const { return checkpoint_.read(); }

void StorageEngine::write_snapshot(AssertionId last_snapshotted_id, const std::vector<Assertion> &assertions,
                                   std::span<const uint8_t> appended_status) {
    require_writable("write_snapshot");

    snapshot_store_.write(last_snapshotted_id, assertions, appended_status);
}

std::optional<SnapshotData> StorageEngine::load_snapshot() const { return snapshot_store_.read(); }

void StorageEngine::append_entity_catalog_entry(EntityId id, const Value &value) {
    require_writable("append_entity_catalog_entry");

    entity_catalog_log_.append(EntityCatalogRecord{id, value});
}

std::vector<EntityCatalogRecord> StorageEngine::load_entity_catalog() const { return entity_catalog_log_.read_all(); }

void StorageEngine::append_predicate_catalog_entry(PredicateId id, const std::string &name) {
    require_writable("append_predicate_catalog_entry");

    predicate_catalog_log_.append(PredicateCatalogRecord{id, name});
}

std::vector<PredicateCatalogRecord> StorageEngine::load_predicate_catalog() const {
    return predicate_catalog_log_.read_all();
}

void StorageEngine::write_payload(EntityId id, std::span<const std::byte> content) {
    require_writable("write_payload");

    payload_store_.write(id, content);
}

std::optional<std::vector<std::byte>> StorageEngine::load_payload(EntityId id) const { return payload_store_.read(id); }

std::vector<EntityId> StorageEngine::existing_payload_ids() const { return payload_store_.existing_ids(); }

void StorageEngine::append_provenance_entry(AssertionId assertion_id, EntityId source, Timestamp recorded_at,
                                            const std::string &method) {
    require_writable("append_provenance_entry");

    provenance_log_.append(ProvenanceRecord{assertion_id, source, recorded_at, method});
}

void StorageEngine::append_provenance_entries(std::span<const ProvenanceRecord> records) {
    require_writable("append_provenance_entries");

    provenance_log_.append_batch(records);
}

std::vector<ProvenanceRecord> StorageEngine::load_provenance() const { return provenance_log_.read_all(); }

void StorageEngine::append_entity_merge_entry(EntityId absorbed, EntityId surviving, Timestamp merged_at) {
    require_writable("append_entity_merge_entry");

    entity_merge_log_.append(EntityMergeRecord{absorbed, surviving, merged_at});
}

std::vector<EntityMergeRecord> StorageEngine::load_entity_merges() const { return entity_merge_log_.read_all(); }

const StorageConfig &StorageEngine::config() const { return config_; }

} // namespace knk