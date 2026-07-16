#include "kernel/storage_engine.hpp"
#include "kernel/subject_index_log.hpp"

namespace knk {

StorageEngine::StorageEngine(StorageConfig config)
    : config_(std::move(config)), assertion_log_(config_.segment_directory(), config_.max_records_per_segment),
      observed_time_index_log_(config_.observed_time_index_path()), subject_index_log_(config_.subject_index_path()),
      current_index_log_(config_.current_index_path()), checkpoint_(config_.checkpoint_path()),
      snapshot_store_(config_.snapshot_path()), entity_catalog_log_(config_.entity_catalog_path()),
      predicate_catalog_log_(config_.predicate_catalog_path()), payload_store_(config_.payload_directory()) {
    std::filesystem::create_directories(config_.root);
    std::filesystem::create_directories(config_.index_directory());
    std::filesystem::create_directories(config_.payload_directory());
    std::filesystem::create_directories(config_.catalog_directory());
}

void StorageEngine::append_assertion(const Assertion &assertion) { assertion_log_.append(assertion); }

std::vector<Assertion> StorageEngine::load_assertions() const { return assertion_log_.read_all(); }

std::vector<Assertion> StorageEngine::load_assertions_after(AssertionId last_seen_id) const {
    return assertion_log_.read_after(last_seen_id);
}

AssertionId StorageEngine::assertion_log_record_count_hint() const { return assertion_log_.record_count_hint(); }

void StorageEngine::append_observed_time_entry(EntityId subject, Timestamp observed_at, AssertionId id) {
    observed_time_index_log_.append(ObservedTimeIndexRecord{subject, observed_at, id});
}

std::vector<ObservedTimeIndexRecord> StorageEngine::load_observed_time_index() const {
    return observed_time_index_log_.read_all();
}

void StorageEngine::rewrite_observed_time_index(const std::vector<ObservedTimeIndexRecord> &records) {
    observed_time_index_log_.overwrite_all(records);
}

void StorageEngine::append_subject_entry(EntityId subject, AssertionId id) {
    subject_index_log_.append(SubjectIndexRecord(subject, id));
}

std::vector<SubjectIndexRecord> StorageEngine::load_subject_index() const { return subject_index_log_.read_all(); }

void StorageEngine::rewrite_subject_index(const std::vector<SubjectIndexRecord> &records) {
    subject_index_log_.overwrite_all(records);
}

void StorageEngine::append_current_index_entry(EntityId subject, PredicateId predicate, AssertionId id, bool active) {
    current_index_log_.append(CurrentIndexRecord{subject, predicate, id, active});
}

std::vector<CurrentIndexRecord> StorageEngine::load_current_index() const { return current_index_log_.read_all(); }

void StorageEngine::rewrite_current_index(const std::vector<CurrentIndexRecord> &records) {
    current_index_log_.overwrite_all(records);
}

void StorageEngine::write_checkpoint(AssertionId last_fully_indexed_id) { checkpoint_.write(last_fully_indexed_id); }

AssertionId StorageEngine::load_checkpoint() const { return checkpoint_.read(); }

void StorageEngine::write_snapshot(AssertionId last_snapshotted_id, const std::vector<Assertion> &assertions) {
    snapshot_store_.write(last_snapshotted_id, assertions);
}

std::optional<SnapshotData> StorageEngine::load_snapshot() const { return snapshot_store_.read(); }

void StorageEngine::append_entity_catalog_entry(EntityId id, const Value &value) {
    entity_catalog_log_.append(EntityCatalogRecord{id, value});
}

std::vector<EntityCatalogRecord> StorageEngine::load_entity_catalog() const { return entity_catalog_log_.read_all(); }

void StorageEngine::append_predicate_catalog_entry(PredicateId id, const std::string &name) {
    predicate_catalog_log_.append(PredicateCatalogRecord{id, name});
}

std::vector<PredicateCatalogRecord> StorageEngine::load_predicate_catalog() const {
    return predicate_catalog_log_.read_all();
}

void StorageEngine::write_payload(EntityId id, std::span<const std::byte> content) {
    payload_store_.write(id, content);
}

std::optional<std::vector<std::byte>> StorageEngine::load_payload(EntityId id) const { return payload_store_.read(id); }

std::vector<EntityId> StorageEngine::existing_payload_ids() const { return payload_store_.existing_ids(); }

const StorageConfig &StorageEngine::config() const { return config_; }

} // namespace knk