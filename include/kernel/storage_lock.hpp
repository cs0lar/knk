#pragma once

#include <filesystem>

namespace knk {

// Enforces the single-writer model AGENTS.md's Concurrency Rules section declares ("single writer,
// many readers later") but that nothing previously checked: two processes (or two live objects in
// one process) opening the same storage root would both replay the logs and both start appending,
// silently, with no error at either point -- the failure only surfaces later as a log that no
// longer replays cleanly.
//
// Construction takes an advisory exclusive lock (POSIX flock(), LOCK_EX | LOCK_NB) on a lock file
// inside `root`, held for the object's lifetime; a root already held by a live holder throws
// std::runtime_error naming the path immediately, leaving that holder unaffected. The lock is
// released by the kernel on destruction or on any process exit, including SIGKILL -- unlike a
// hand-rolled lock file, there is no stale-lock state a crashed holder can leave behind for the
// next opener to reason about.
//
// This is `LOCK_EX` unconditionally: the declared model has no readers yet, so every open -- read
// or write -- takes the exclusive lock. When "many readers later" is implemented, a read-only open
// can switch to `LOCK_SH` as an additive change here.
//
// Linux/POSIX-only, matching kernel/durability.hpp.
class StorageLock {
  public:
    explicit StorageLock(const std::filesystem::path &root);

    ~StorageLock();

    StorageLock(const StorageLock &) = delete;
    StorageLock &operator=(const StorageLock &) = delete;

    StorageLock(StorageLock &&other) noexcept;
    StorageLock &operator=(StorageLock &&other) noexcept;

  private:
    int fd_ = -1;

    void release();
};

} // namespace knk
