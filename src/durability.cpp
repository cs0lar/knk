#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>

#include "kernel/durability.hpp"

namespace knk {

void fsync_file(const std::filesystem::path &path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("failed to open '" + path.string() + "' for fsync");
    }

    int result = ::fsync(fd);
    ::close(fd);

    if (result != 0) {
        throw std::runtime_error("fsync failed for '" + path.string() + "'");
    }
}

void write_file_atomically(const std::filesystem::path &path,
                           const std::function<void(std::ostream &)> &write_contents) {
    std::filesystem::create_directories(path.parent_path());

    auto tmp_path = path;
    tmp_path += ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("failed to open '" + tmp_path.string() + "' for atomic write");
        }

        write_contents(out);

        if (!out) {
            throw std::runtime_error("failed to write '" + tmp_path.string() + "'");
        }

        out.close();
    }

    fsync_file(tmp_path);

    std::filesystem::rename(tmp_path, path);

    fsync_file(path.parent_path());
}

} // namespace knk
