#pragma once

#include <filesystem>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/assertion_log.hpp"
#include "kernel/ids.hpp"
#include "kernel/observed_time_index_log.hpp"
#include "kernel/storage_config.hpp"
#include "kernel/subject_index_log.hpp"
#include "kernel/time.hpp"

namespace knk {

class StorageEngine {
  public:
    explicit StorageEngine(StorageConfig config);

    void append_assertion(const Assertion &assertion);

    std::vector<Assertion> load_assertions() const;

    void append_observed_time_entry(EntityId subject, Timestamp observed_at, AssertionId id);

    std::vector<ObservedTimeIndexRecord> load_observed_time_index() const;

    void rewrite_observed_time_index(const std::vector<ObservedTimeIndexRecord> &records);

    void append_subject_entry(EntityId subject, AssertionId id);

    std::vector<SubjectIndexRecord> load_subject_index() const;

    void rewrite_subject_index(const std::vector<SubjectIndexRecord> &records);

    const StorageConfig &config() const;

  private:
    StorageConfig config_;
    AssertionLog assertion_log_;
    ObservedTimeIndexLog observed_time_index_log_;
    SubjectIndexLog subject_index_log_;
};

} // namespace knk