#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "kernel/storage_lock.hpp"

using namespace knk;

namespace {

void storage_lock_creates_the_root_directory() {
    auto root = std::filesystem::temp_directory_path() / "kernel_storage_lock_creates_root_test";
    std::filesystem::remove_all(root);

    StorageLock lock(root);

    assert(std::filesystem::exists(root));
    assert(std::filesystem::exists(root / "LOCK"));

    std::filesystem::remove_all(root);
}

void second_lock_on_a_held_root_throws_naming_the_path() {
    auto root = std::filesystem::temp_directory_path() / "kernel_storage_lock_conflict_test";
    std::filesystem::remove_all(root);

    StorageLock holder(root);

    bool threw = false;
    try {
        StorageLock contender(root);
    } catch (const std::runtime_error &e) {
        threw = true;
        assert(std::string(e.what()).find(root.string()) != std::string::npos);
    }

    assert(threw);

    std::filesystem::remove_all(root);
}

void lock_is_available_again_after_the_holder_is_destroyed() {
    auto root = std::filesystem::temp_directory_path() / "kernel_storage_lock_release_test";
    std::filesystem::remove_all(root);

    { StorageLock first(root); }

    StorageLock second(root); // should not throw

    std::filesystem::remove_all(root);
}

void move_construction_transfers_lock_ownership() {
    auto root = std::filesystem::temp_directory_path() / "kernel_storage_lock_move_test";
    std::filesystem::remove_all(root);

    StorageLock original(root);
    StorageLock moved(std::move(original));

    bool threw = false;
    try {
        StorageLock contender(root);
    } catch (const std::runtime_error &) {
        threw = true;
    }

    assert(threw);

    std::filesystem::remove_all(root);
}

// The whole point of an OS-level advisory lock over a hand-rolled lock file: a holder that dies
// via SIGKILL (no destructor, no cleanup handler ever runs) still releases the lock, because the
// kernel closes the process's file descriptors -- and with them, its flock()s -- as part of
// process exit. This is what the issue's "no manual cleanup" acceptance criterion is about.
void lock_is_released_when_the_holder_is_sigkilled() {
    auto root = std::filesystem::temp_directory_path() / "kernel_storage_lock_sigkill_test";
    std::filesystem::remove_all(root);

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
        throw std::runtime_error("pipe() failed");
    }

    pid_t child = ::fork();
    assert(child >= 0);

    if (child == 0) {
        ::close(pipe_fds[0]);
        try {
            StorageLock lock(root);
            char byte = 'k';
            static_cast<void>(::write(pipe_fds[1], &byte, 1));
            for (;;) {
                ::pause();
            }
        } catch (...) {
            ::_exit(1);
        }
    }

    ::close(pipe_fds[1]);
    char byte = 0;
    ssize_t n = ::read(pipe_fds[0], &byte, 1);
    ::close(pipe_fds[0]);
    assert(n == 1);

    bool threw = false;
    try {
        StorageLock contender(root);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);

    assert(::kill(child, SIGKILL) == 0);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);

    StorageLock lock_after_sigkill(root); // no manual cleanup required -- should not throw

    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    storage_lock_creates_the_root_directory();
    second_lock_on_a_held_root_throws_naming_the_path();
    lock_is_available_again_after_the_holder_is_destroyed();
    move_construction_transfers_lock_ownership();
    lock_is_released_when_the_holder_is_sigkilled();

    std::cout << "All storage lock tests passed.\n";
}
