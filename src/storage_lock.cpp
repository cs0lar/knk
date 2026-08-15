#include "kernel/storage_lock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <stdexcept>
#include <utility>

namespace knk {

namespace {
constexpr const char *kLockFileName = "LOCK";
} // namespace

StorageLock::StorageLock(const std::filesystem::path &root) {
    std::filesystem::create_directories(root);

    std::filesystem::path lock_path = root / kLockFileName;

    fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) {
        throw std::runtime_error("failed to open lock file '" + lock_path.string() + "'");
    }

    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("storage root '" + root.string() +
                                 "' is already locked by another process (single-writer violation)");
    }
}

StorageLock::~StorageLock() { release(); }

StorageLock::StorageLock(StorageLock &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

StorageLock &StorageLock::operator=(StorageLock &&other) noexcept {
    if (this != &other) {
        release();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void StorageLock::release() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace knk
