#pragma once

// The MCP-facing tool registry and dispatcher: one MCP tool per KernelCommand variant, named after
// the mirrored KnowledgeKernel method (e.g. "commit", "current_by_object"). Deliberately not a
// generic "execute a KernelCommand blob" tool -- MCP tool schemas are meant to be individually
// discoverable and typed by an agent, which a single polymorphic tool would defeat. This file is the
// pure, I/O-free half of the server: no stdin/stdout, no JSON-RPC framing, so it is directly unit
// testable. mcp/main.cpp is the thin stdio loop built on top of it.

#include <filesystem>
#include <string>

#include "kernel/knowledge_kernel.hpp"

#include <nlohmann/json.hpp>

namespace knk::mcp {

struct ToolSpec {
    std::string name;
    std::string description;
    // JSON Schema object, per MCP's tools/list "inputSchema" field. ordered_json (not json) because
    // callers bind arguments positionally against the emitted "properties" order (see AGENTS.md's
    // "MCP parameter ordering" rule) -- json's default map-backed object would re-sort properties
    // alphabetically on serialization and silently break that convention.
    nlohmann::ordered_json input_schema;
};

// Every registered tool's name/description/schema, in a stable order (declaration order below).
const std::vector<ToolSpec> &tool_specs();

// content_text is what an MCP client sees in the tools/call response's content[0].text (the
// serialized KernelResult, or an error message). is_error mirrors MCP's convention of reporting a
// tool-level failure as a normal JSON-RPC success with isError=true rather than a JSON-RPC-level
// error -- see mcp/main.cpp.
// Server-supplied context a tool may need beyond its arguments. Today that is only where spilled
// results may be written: taking it from the server's own configuration rather than from a tool argument
// means a client cannot name an arbitrary path for the process to write to.
struct ToolContext {
    std::filesystem::path spill_directory; // empty means spilling is not configured
};

struct ToolCallResult {
    std::string content_text;
    bool is_error = false;
};

// Looks up tool_name, builds the matching KernelCommand from arguments, executes it against kernel,
// and serializes the result via kernel_result_to_json. An unknown tool_name, a missing/malformed
// argument (nlohmann::json throws on both), or an exception from kernel.execute() itself (e.g.
// commit_retraction against an unknown id) all come back as is_error=true rather than throwing --
// mcp/main.cpp never needs its own exception guard around this call.
ToolCallResult handle_tool_call(KnowledgeKernel &kernel, const std::string &tool_name, const nlohmann::json &arguments,
                                const ToolContext &context = {});

} // namespace knk::mcp
