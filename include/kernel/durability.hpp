#pragma once

#include <filesystem>
#include <functional>
#include <ostream>

namespace knk {

// Forces previously-written data for the file or directory at `path` to physical disk. Works for
// regular files and directories (POSIX fsync operates on any open file descriptor). Linux/POSIX-only.
void fsync_file(const std::filesystem::path &path);

// Writes `path`'s full contents via `write_contents`, using a temp-file-then-rename so a crash
// mid-write never leaves `path` partially written: readers always see either the old complete
// file or the new complete file. Fsyncs the temp file before the rename and the parent directory
// after it (the standard "durable rename" pattern).
void write_file_atomically(const std::filesystem::path &path,
                           const std::function<void(std::ostream &)> &write_contents);

} // namespace knk
