#pragma once

// Transport-agnostic JSON (de)serialization for the types an external boundary needs to move across
// the wire: Assertion, Value, ProvenanceRecord, AssertionStatus, and KernelResult. Deliberately does
// NOT include a generic tagged-variant codec for KernelCommand itself -- the one caller of this today
// (mcp/, see mcp_tools.hpp) already knows which command it's building from the MCP tool name, so each
// tool constructs its specific Command struct directly rather than round-tripping through a generic
// "type" tag. A future transport that needs to deserialize a KernelCommand without an externally-known
// type (e.g. a single generic HTTP POST /execute endpoint) would add that on top of these primitives,
// not by replacing them.
//
// nlohmann::json (vendored at third_party/nlohmann/json.hpp) is this project's first external
// dependency: KernelCommand/KernelResult together span ~38 command shapes and 11 result shapes with
// varied field types (ids, timestamps, strings, raw bytes, the tagged Value union), and a hand-rolled
// JSON parser correctly covering escaping/unicode/number formats for all of that is real, bug-prone
// surface for no benefit over a single-header, widely-used library.

#include <cstddef>
#include <vector>

#include "kernel/assertion.hpp"
#include "kernel/kernel_result.hpp"
#include "kernel/provenance_log.hpp"
#include "kernel/status.hpp"
#include "kernel/value.hpp"

#include <nlohmann/json.hpp>

namespace knk {

nlohmann::json status_to_json(AssertionStatus status);
AssertionStatus status_from_json(const nlohmann::json &json);

nlohmann::json assertion_to_json(const Assertion &assertion);
Assertion assertion_from_json(const nlohmann::json &json);

nlohmann::json value_to_json(const Value &value);
Value value_from_json(const nlohmann::json &json);

nlohmann::json provenance_record_to_json(const ProvenanceRecord &record);
ProvenanceRecord provenance_record_from_json(const nlohmann::json &json);

// Base64 encode/decode for the one raw-bytes field in the command/result surface (InternDocumentCommand's
// content, DocumentContentCommand's return value) -- JSON has no native binary type.
std::string base64_encode(const std::vector<std::byte> &bytes);
std::vector<std::byte> base64_decode(const std::string &encoded);

// Serializes whichever alternative kernel.execute(command) actually returned. The caller does not
// need to know in advance which of KernelResult's alternatives is active -- std::visit dispatches
// on the live one, same as KnowledgeKernel::execute itself dispatches on the live KernelCommand
// alternative.
nlohmann::json kernel_result_to_json(const KernelResult &result);

} // namespace knk
