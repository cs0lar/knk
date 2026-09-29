#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "kernel/checksum.hpp"
#include "kernel/column_store.hpp"
#include "kernel/durability.hpp"

namespace knk {

namespace {

constexpr std::array<char, 4> COLUMN_MAGIC{'K', 'N', 'K', 'L'};
constexpr std::array<char, 4> MANIFEST_MAGIC{'K', 'N', 'K', 'M'};
constexpr uint32_t FORMAT_VERSION = 1;

// [4-byte magic][uint32 version][uint64 element size], then the raw elements. The element size is stored
// so a stride change is caught as a format mismatch rather than read as garbage.
constexpr size_t COLUMN_HEADER_SIZE = COLUMN_MAGIC.size() + sizeof(uint32_t) + sizeof(uint64_t);

// Column order is part of the format: the manifest's checksums are in this order, and so are the spans
// ColumnStore::map hands back.
enum : size_t {
    COL_SUBJECT,
    COL_PREDICATE,
    COL_OBJECT,
    COL_VALID_FROM,
    COL_VALID_TO,
    COL_OBSERVED_AT,
    COL_CONFIDENCE,
    COL_STATUS,
    COL_SUPERSEDES_ID,
    COL_RETRACTS_ID,
    COLUMN_COUNT,
};

struct ColumnSpec {
    const char *file_name;
    size_t element_size;
};

constexpr std::array<ColumnSpec, COLUMN_COUNT> COLUMNS{{
    {"subject.col", sizeof(EntityId)},
    {"predicate.col", sizeof(PredicateId)},
    {"object.col", sizeof(EntityId)},
    {"valid_from.col", sizeof(Timestamp)},
    {"valid_to.col", sizeof(Timestamp)},
    {"observed_at.col", sizeof(Timestamp)},
    {"confidence.col", sizeof(double)},
    {"status.col", sizeof(uint8_t)},
    {"supersedes_id.col", sizeof(AssertionId)},
    {"retracts_id.col", sizeof(AssertionId)},
}};

// Extracts one field of every assertion as raw bytes, ready to append. A file-local template rather than
// eight near-identical loops; the alternative (a byte-oriented interface taking an offset and width into
// Assertion) would trade this for reinterpret_cast arithmetic at every call site.
template <typename T, typename Extract>
std::vector<char> encode_column(std::span<const Assertion> assertions, Extract extract) {
    std::vector<char> bytes(assertions.size() * sizeof(T));

    for (size_t i = 0; i < assertions.size(); ++i) {
        T value = extract(assertions[i]);
        std::memcpy(bytes.data() + i * sizeof(T), &value, sizeof(T));
    }

    return bytes;
}

std::vector<char> encode(size_t column, std::span<const Assertion> assertions) {
    switch (column) {
    case COL_SUBJECT:
        return encode_column<EntityId>(assertions, [](const Assertion &a) { return a.subject; });
    case COL_PREDICATE:
        return encode_column<PredicateId>(assertions, [](const Assertion &a) { return a.predicate; });
    case COL_OBJECT:
        return encode_column<EntityId>(assertions, [](const Assertion &a) { return a.object; });
    case COL_VALID_FROM:
        return encode_column<Timestamp>(assertions, [](const Assertion &a) { return a.valid_from; });
    case COL_VALID_TO:
        return encode_column<Timestamp>(assertions, [](const Assertion &a) { return a.valid_to; });
    case COL_OBSERVED_AT:
        return encode_column<Timestamp>(assertions, [](const Assertion &a) { return a.observed_at; });
    case COL_CONFIDENCE:
        return encode_column<double>(assertions, [](const Assertion &a) { return a.confidence; });
    case COL_STATUS:
        return encode_column<uint8_t>(assertions, [](const Assertion &a) { return static_cast<uint8_t>(a.status); });
    case COL_SUPERSEDES_ID:
        return encode_column<AssertionId>(assertions, [](const Assertion &a) { return a.supersedes_id; });
    default:
        return encode_column<AssertionId>(assertions, [](const Assertion &a) { return a.retracts_id; });
    }
}

void write_or_throw(std::ofstream &out, const char *data, std::streamsize size) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write column file");
    }
}

void write_column_header(std::ofstream &out, size_t element_size) {
    write_or_throw(out, COLUMN_MAGIC.data(), static_cast<std::streamsize>(COLUMN_MAGIC.size()));

    uint32_t version = FORMAT_VERSION;
    write_or_throw(out, reinterpret_cast<const char *>(&version), sizeof(version));

    uint64_t stride = element_size;
    write_or_throw(out, reinterpret_cast<const char *>(&stride), sizeof(stride));
}

// A column file is usable only if its header is intact *and* its payload is a whole number of elements.
// Returns the element count, or nullopt when the file is missing or malformed -- either way the answer is
// "rebuild", so the distinction does not need to travel further.
std::optional<size_t> readable_element_count(const std::filesystem::path &path, size_t element_size) {
    std::error_code error;
    auto size = std::filesystem::file_size(path, error);
    if (error || size < COLUMN_HEADER_SIZE) {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }

    std::array<char, 4> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    uint32_t version = 0;
    in.read(reinterpret_cast<char *>(&version), sizeof(version));
    uint64_t stride = 0;
    in.read(reinterpret_cast<char *>(&stride), sizeof(stride));

    if (!in || magic != COLUMN_MAGIC || version != FORMAT_VERSION || stride != element_size) {
        return std::nullopt;
    }

    size_t payload = static_cast<size_t>(size) - COLUMN_HEADER_SIZE;
    if (payload % element_size != 0) {
        return std::nullopt; // a torn append; rebuilding is cheaper than reasoning about the remainder
    }

    return payload / element_size;
}

} // namespace

ColumnStore::ColumnStore(std::filesystem::path directory) : directory_(std::move(directory)) {
    mappings_.resize(COLUMN_COUNT);
    load_manifest();
}

ColumnStore::~ColumnStore() { unmap(); }

ColumnStore::ColumnStore(ColumnStore &&other) noexcept
    : directory_(std::move(other.directory_)), row_count_(std::exchange(other.row_count_, 0)),
      manifest_checksums_(std::move(other.manifest_checksums_)), mappings_(std::move(other.mappings_)) {
    // other.mappings_ is left empty by the move, so its destructor unmaps nothing -- ownership of every
    // descriptor and mapping moved with it.
}

ColumnStore &ColumnStore::operator=(ColumnStore &&other) noexcept {
    if (this != &other) {
        unmap();
        directory_ = std::move(other.directory_);
        row_count_ = std::exchange(other.row_count_, 0);
        manifest_checksums_ = std::move(other.manifest_checksums_);
        mappings_ = std::move(other.mappings_);
    }

    return *this;
}

std::filesystem::path ColumnStore::column_path(size_t column) const {
    return directory_ / COLUMNS.at(column).file_name;
}

size_t ColumnStore::row_count() const { return row_count_; }

void ColumnStore::load_manifest() {
    row_count_ = 0;

    std::ifstream in(directory_ / "manifest", std::ios::binary);
    if (!in) {
        return;
    }

    std::array<char, 4> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    uint32_t version = 0;
    in.read(reinterpret_cast<char *>(&version), sizeof(version));
    uint64_t rows = 0;
    in.read(reinterpret_cast<char *>(&rows), sizeof(rows));

    std::array<uint32_t, COLUMN_COUNT> checksums{};
    in.read(reinterpret_cast<char *>(checksums.data()), sizeof(checksums));

    uint32_t stored_crc = 0;
    in.read(reinterpret_cast<char *>(&stored_crc), sizeof(stored_crc));

    if (!in || magic != MANIFEST_MAGIC || version != FORMAT_VERSION) {
        return; // unreadable manifest means "no usable store", which the caller answers by rebuilding
    }

    // The manifest is small and rewritten atomically, so a self-check here is cheap insurance against a
    // truncated or scribbled-on file being read as a row count.
    uint32_t running = crc32_update(CRC32_INIT, &rows, sizeof(rows));
    running = crc32_update(running, checksums.data(), sizeof(checksums));
    if (crc32_finalize(running) != stored_crc) {
        return;
    }

    row_count_ = static_cast<size_t>(rows);
    manifest_checksums_.assign(checksums.begin(), checksums.end());
}

void ColumnStore::write_manifest(const std::vector<uint32_t> &checksums) {
    std::array<uint32_t, COLUMN_COUNT> stored{};
    for (size_t column = 0; column < COLUMN_COUNT; ++column) {
        stored[column] = checksums.at(column);
    }

    uint64_t rows = row_count_;
    uint32_t running = crc32_update(CRC32_INIT, &rows, sizeof(rows));
    running = crc32_update(running, stored.data(), sizeof(stored));
    uint32_t crc = crc32_finalize(running);

    // Written in place, neither atomically nor fsynced -- and for the same reason the columns themselves
    // are not: every byte of this is derived. A crash mid-rewrite can leave a mixture of old and new
    // bytes, which the self-checksum above rejects, and a rejected manifest means "rebuild from the log".
    // The atomic-rename form costs two more fsyncs per commit (temp file, then parent directory) to
    // protect state that is reconstructible, which measured as a large share of this phase's commit-path
    // regression; see docs/benchmarks.md.
    std::ofstream out(directory_ / "manifest", std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write column manifest");
    }

    out.write(MANIFEST_MAGIC.data(), static_cast<std::streamsize>(MANIFEST_MAGIC.size()));
    uint32_t version = FORMAT_VERSION;
    out.write(reinterpret_cast<const char *>(&version), sizeof(version));
    out.write(reinterpret_cast<const char *>(&rows), sizeof(rows));
    out.write(reinterpret_cast<const char *>(stored.data()), sizeof(stored));
    out.write(reinterpret_cast<const char *>(&crc), sizeof(crc));

    if (!out) {
        throw std::runtime_error("failed to write column manifest");
    }

    manifest_checksums_.assign(stored.begin(), stored.end());
}

std::vector<uint32_t> ColumnStore::compute_checksums() const {
    std::vector<uint32_t> checksums(COLUMN_COUNT, crc32_finalize(CRC32_INIT));

    for (size_t column = 0; column < COLUMN_COUNT; ++column) {
        std::ifstream in(column_path(column), std::ios::binary);
        if (!in) {
            return {};
        }

        in.seekg(static_cast<std::streamoff>(COLUMN_HEADER_SIZE), std::ios::beg);

        uint32_t running = CRC32_INIT;
        std::array<char, 64 * 1024> buffer{};
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            auto read = static_cast<size_t>(in.gcount());
            if (read == 0) {
                break;
            }
            running = crc32_update(running, buffer.data(), read);
        }

        checksums[column] = crc32_finalize(running);
    }

    return checksums;
}

bool ColumnStore::verify() const {
    if (manifest_checksums_.size() != COLUMN_COUNT) {
        return false;
    }

    // Sizes first: it is the cheap check, and a disagreement between two columns is enough to reject the
    // store without reading a byte of payload.
    for (size_t column = 0; column < COLUMN_COUNT; ++column) {
        auto elements = readable_element_count(column_path(column), COLUMNS.at(column).element_size);
        if (!elements.has_value() || *elements != row_count_) {
            return false;
        }
    }

    auto checksums = compute_checksums();
    if (checksums.size() != COLUMN_COUNT) {
        return false;
    }

    for (size_t column = 0; column < COLUMN_COUNT; ++column) {
        if (checksums[column] != manifest_checksums_[column]) {
            return false;
        }
    }

    return true;
}

void ColumnStore::append(std::span<const Assertion> assertions) {
    if (assertions.empty()) {
        return;
    }

    // Any mapping handed out earlier describes the old file lengths, so it is invalidated here rather
    // than left to dangle past the growth.
    unmap();

    std::filesystem::create_directories(directory_);

    // The running state per column is the manifest's checksum with finalization undone -- which is what
    // keeps an append O(appended bytes) instead of O(store).
    std::vector<uint32_t> running(COLUMN_COUNT, CRC32_INIT);
    if (manifest_checksums_.size() == COLUMN_COUNT) {
        for (size_t column = 0; column < COLUMN_COUNT; ++column) {
            running[column] = manifest_checksums_[column] ^ 0xFFFFFFFF;
        }
    }

    for (size_t column = 0; column < COLUMN_COUNT; ++column) {
        auto path = column_path(column);
        bool is_new = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;

        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) {
            throw std::runtime_error("failed to open column file '" + path.string() + "' for append");
        }

        if (is_new) {
            write_column_header(out, COLUMNS.at(column).element_size);
        }

        auto bytes = encode(column, assertions);
        write_or_throw(out, bytes.data(), static_cast<std::streamsize>(bytes.size()));

        // Deliberately **not** fsynced, unlike every log in the storage root. Columns are derived: losing
        // unflushed bytes to a crash costs a rebuild, never data, and the recovery path already handles
        // both a short column (size mismatch -> rebuild) and one that lags the log (tail append). Paying
        // ten fsyncs per commit for state that is reconstructible measured at ~2.9x on single-commit
        // throughput -- see docs/benchmarks.md -- which is a real price for no durability gain.
        out.close();

        running[column] = crc32_update(running[column], bytes.data(), bytes.size());
    }

    row_count_ += assertions.size();

    std::vector<uint32_t> finalized(COLUMN_COUNT, 0);
    for (size_t column = 0; column < COLUMN_COUNT; ++column) {
        finalized[column] = crc32_finalize(running[column]);
    }

    // The manifest goes last, so a crash between the columns and it leaves the manifest describing fewer
    // rows than the files hold -- detected on the next open as a size mismatch, which rebuilds. The
    // reverse order would leave a manifest claiming rows that are not there.
    write_manifest(finalized);
}

void ColumnStore::overwrite_all(std::span<const Assertion> assertions) {
    unmap();

    std::filesystem::create_directories(directory_);

    for (size_t column = 0; column < COLUMN_COUNT; ++column) {
        std::filesystem::remove(column_path(column));
    }
    std::filesystem::remove(directory_ / "manifest");

    row_count_ = 0;
    manifest_checksums_.clear();

    if (assertions.empty()) {
        // Still write a manifest, so an empty store is a *known* empty store rather than an absent one.
        write_manifest(std::vector<uint32_t>(COLUMN_COUNT, crc32_finalize(CRC32_INIT)));
        for (size_t column = 0; column < COLUMN_COUNT; ++column) {
            auto path = column_path(column);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            write_column_header(out, COLUMNS.at(column).element_size);
            out.close();
        }
        return;
    }

    append(assertions);
}

ColumnSpans ColumnStore::map() const {
    ColumnSpans spans;

    if (row_count_ == 0) {
        return spans;
    }

    if (mappings_[COL_SUBJECT].address == nullptr) {
        for (size_t column = 0; column < COLUMN_COUNT; ++column) {
            auto path = column_path(column);
            size_t length = COLUMN_HEADER_SIZE + row_count_ * COLUMNS.at(column).element_size;

            auto elements = readable_element_count(path, COLUMNS.at(column).element_size);
            if (!elements.has_value() || *elements < row_count_) {
                unmap();
                return spans; // unusable: the caller falls back to the log
            }

            int fd = ::open(path.c_str(), O_RDONLY);
            if (fd < 0) {
                unmap();
                return spans;
            }

            void *address = ::mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
            if (address == MAP_FAILED) {
                ::close(fd);
                unmap();
                return spans;
            }

            mappings_[column] = Mapping{fd, address, length};
        }
    }

    auto payload = [this](size_t column) {
        return static_cast<const char *>(mappings_[column].address) + COLUMN_HEADER_SIZE;
    };

    spans.subject = {reinterpret_cast<const EntityId *>(payload(COL_SUBJECT)), row_count_};
    spans.predicate = {reinterpret_cast<const PredicateId *>(payload(COL_PREDICATE)), row_count_};
    spans.object = {reinterpret_cast<const EntityId *>(payload(COL_OBJECT)), row_count_};
    spans.valid_from = {reinterpret_cast<const Timestamp *>(payload(COL_VALID_FROM)), row_count_};
    spans.valid_to = {reinterpret_cast<const Timestamp *>(payload(COL_VALID_TO)), row_count_};
    spans.observed_at = {reinterpret_cast<const Timestamp *>(payload(COL_OBSERVED_AT)), row_count_};
    spans.confidence = {reinterpret_cast<const double *>(payload(COL_CONFIDENCE)), row_count_};
    spans.status = {reinterpret_cast<const uint8_t *>(payload(COL_STATUS)), row_count_};
    spans.supersedes_id = {reinterpret_cast<const AssertionId *>(payload(COL_SUPERSEDES_ID)), row_count_};
    spans.retracts_id = {reinterpret_cast<const AssertionId *>(payload(COL_RETRACTS_ID)), row_count_};

    return spans;
}

void ColumnStore::unmap() const {
    for (auto &mapping : mappings_) {
        if (mapping.address != nullptr) {
            ::munmap(mapping.address, mapping.length);
        }
        if (mapping.fd >= 0) {
            ::close(mapping.fd);
        }
        mapping = Mapping{};
    }
}

} // namespace knk
