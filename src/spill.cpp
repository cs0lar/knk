#include <array>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

#include "kernel/json_codec.hpp"
#include "kernel/spill.hpp"
#include "kernel/status.hpp"

#include <nlohmann/json.hpp>

namespace knk {

namespace {

constexpr uint32_t SPILL_FORMAT_VERSION = 1;

// Rows written per pass. Bounded so a ten-million-row spill streams rather than materializing another
// copy of the corpus in memory -- the whole reason the selection is row indices.
constexpr size_t CHUNK_ROWS = 8192;

void write_or_throw(std::ofstream &out, const char *data, std::streamsize size, const std::string &path) {
    out.write(data, size);
    if (!out) {
        throw std::runtime_error("failed to write spill column '" + path + "'");
    }
}

// One column, gathered through the selection and appended in chunks. A file-local template rather than
// ten near-identical loops; every column is fixed-width, which is what makes a reader trivial.
template <typename T, typename Extract>
void write_column(const std::filesystem::path &path, std::span<const uint32_t> selection,
                  std::span<const Assertion> assertions, Extract extract) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open spill column '" + path.string() + "'");
    }

    std::vector<char> buffer;
    buffer.reserve(CHUNK_ROWS * sizeof(T));

    for (size_t start = 0; start < selection.size(); start += CHUNK_ROWS) {
        size_t count = std::min(CHUNK_ROWS, selection.size() - start);
        buffer.resize(count * sizeof(T));

        for (size_t i = 0; i < count; ++i) {
            T value = extract(assertions[selection[start + i]]);
            std::memcpy(buffer.data() + i * sizeof(T), &value, sizeof(T));
        }

        write_or_throw(out, buffer.data(), static_cast<std::streamsize>(buffer.size()), path.string());
    }

    out.close();
}

SpillColumn describe(const std::string &name, const std::string &type, size_t width) {
    SpillColumn column;
    column.name = name;
    column.file = name + ".col";
    column.type = type;
    column.bytes_per_value = width;
    return column;
}

} // namespace

SpillDescriptor write_spill(const std::filesystem::path &directory, const std::string &token,
                            std::span<const uint32_t> selection, std::span<const Assertion> assertions,
                            const Catalog &catalog, std::span<const uint8_t> status_override) {
    if (selection.size() > MAX_SPILL_ROWS) {
        throw std::runtime_error("spill exceeds MAX_SPILL_ROWS");
    }

    if (token.empty() || token.find('/') != std::string::npos || token == "." || token == "..") {
        throw std::runtime_error("invalid spill token: '" + token + "'");
    }

    std::filesystem::path spill_directory = directory / token;
    if (std::filesystem::exists(spill_directory)) {
        // Refused rather than overwritten: a result someone may still be reading is not ours to replace.
        throw std::runtime_error("spill '" + token + "' already exists");
    }

    std::filesystem::create_directories(spill_directory);

    SpillDescriptor descriptor;
    descriptor.token = token;
    descriptor.directory = spill_directory;
    descriptor.rows = selection.size();
    descriptor.dictionary_file = "dictionary.json";

    descriptor.columns = {
        describe("id", "uint64", sizeof(AssertionId)),
        describe("subject", "uint64", sizeof(EntityId)),
        describe("predicate", "uint64", sizeof(PredicateId)),
        describe("object", "uint64", sizeof(EntityId)),
        describe("valid_from", "int64", sizeof(Timestamp)),
        describe("valid_to", "int64", sizeof(Timestamp)),
        describe("observed_at", "int64", sizeof(Timestamp)),
        describe("confidence", "double", sizeof(double)),
        describe("status", "uint8", sizeof(uint8_t)),
        describe("supersedes_id", "uint64", sizeof(AssertionId)),
        describe("retracts_id", "uint64", sizeof(AssertionId)),
    };

    write_column<AssertionId>(spill_directory / "id.col", selection, assertions,
                              [](const Assertion &a) { return a.id; });
    write_column<EntityId>(spill_directory / "subject.col", selection, assertions,
                           [](const Assertion &a) { return a.subject; });
    write_column<PredicateId>(spill_directory / "predicate.col", selection, assertions,
                              [](const Assertion &a) { return a.predicate; });
    write_column<EntityId>(spill_directory / "object.col", selection, assertions,
                           [](const Assertion &a) { return a.object; });
    write_column<Timestamp>(spill_directory / "valid_from.col", selection, assertions,
                            [](const Assertion &a) { return a.valid_from; });
    write_column<Timestamp>(spill_directory / "valid_to.col", selection, assertions,
                            [](const Assertion &a) { return a.valid_to; });
    write_column<Timestamp>(spill_directory / "observed_at.col", selection, assertions,
                            [](const Assertion &a) { return a.observed_at; });
    write_column<double>(spill_directory / "confidence.col", selection, assertions,
                         [](const Assertion &a) { return a.confidence; });
    // Effective status, because a spill is a query result and must say what a query says -- or the
    // reconstructed status, when the query asked as of an earlier point.
    if (status_override.empty()) {
        write_column<uint8_t>(spill_directory / "status.col", selection, assertions,
                              [](const Assertion &a) { return static_cast<uint8_t>(a.status); });
    } else {
        write_column<uint8_t>(spill_directory / "status.col", selection, assertions,
                              [&status_override](const Assertion &a) { return status_override[a.id - 1]; });
    }
    write_column<AssertionId>(spill_directory / "supersedes_id.col", selection, assertions,
                              [](const Assertion &a) { return a.supersedes_id; });
    write_column<AssertionId>(spill_directory / "retracts_id.col", selection, assertions,
                              [](const Assertion &a) { return a.retracts_id; });

    // The dictionary: only the ids actually present, which is O(distinct) rather than O(rows). Without it
    // a column of ids is unusable to a consumer, and the alternative was making them join back through
    // entity_name_batch a page at a time.
    std::set<EntityId> entities;
    std::set<PredicateId> predicates;
    for (uint32_t row : selection) {
        const Assertion &assertion = assertions[row];
        entities.insert(assertion.subject);
        entities.insert(assertion.object);
        predicates.insert(assertion.predicate);
    }

    nlohmann::json entity_json = nlohmann::json::object();
    for (EntityId id : entities) {
        auto value = catalog.entity_value(id);
        if (value.has_value()) {
            entity_json[std::to_string(id)] = value_to_json(*value);
        }
    }

    nlohmann::json predicate_json = nlohmann::json::object();
    for (PredicateId id : predicates) {
        auto name = catalog.predicate_name(id);
        if (name.has_value()) {
            predicate_json[std::to_string(id)] = *name;
        }
    }

    {
        std::ofstream out(spill_directory / descriptor.dictionary_file, std::ios::binary | std::ios::trunc);
        out << nlohmann::json{{"entities", entity_json}, {"predicates", predicate_json}}.dump(2) << "\n";
    }

    nlohmann::json columns_json = nlohmann::json::array();
    for (const auto &column : descriptor.columns) {
        columns_json.push_back(nlohmann::json{{"name", column.name},
                                              {"file", column.file},
                                              {"type", column.type},
                                              {"bytes_per_value", column.bytes_per_value}});
    }

    // Everything a reader needs is here, which is why the column files carry no headers of their own: a
    // consumer maps or reads each file whole and interprets it with this.
    nlohmann::json descriptor_json{{"format", "knk-columnar-result"},
                                   {"version", SPILL_FORMAT_VERSION},
                                   {"token", descriptor.token},
                                   {"rows", descriptor.rows},
                                   {"byte_order", "little"},
                                   {"columns", columns_json},
                                   {"dictionary", descriptor.dictionary_file}};

    {
        std::ofstream out(spill_directory / "descriptor.json", std::ios::binary | std::ios::trunc);
        out << descriptor_json.dump(2) << "\n";
        if (!out) {
            throw std::runtime_error("failed to write spill descriptor");
        }
    }

    return descriptor;
}

void drop_spill(const std::filesystem::path &directory, const std::string &token) {
    if (token.empty() || token.find('/') != std::string::npos || token == "." || token == "..") {
        throw std::runtime_error("invalid spill token: '" + token + "'");
    }

    std::filesystem::remove_all(directory / token);
}

} // namespace knk
