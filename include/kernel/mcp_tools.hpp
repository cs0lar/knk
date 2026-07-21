#pragma once

// The MCP-facing tool registry and dispatcher: one MCP tool per KernelCommand variant, named after
// the mirrored KnowledgeKernel method (e.g. "commit", "current_by_object"). Deliberately not a
// generic "execute a KernelCommand blob" tool -- MCP tool schemas are meant to be individually
// discoverable and typed by an agent, which a single polymorphic tool would defeat. This file is the
// pure, I/O-free half of the server: no stdin/stdout, no JSON-RPC framing, so it is directly unit
// testable. mcp/main.cpp is the thin stdio loop built on top of it.

#include <string>

#include "kernel/knowledge_kernel.hpp"

#include <nlohmann/json.hpp>

namespace knk::mcp {

struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json input_schema; // JSON Schema object, per MCP's tools/list "inputSchema" field.
};

// Every registered tool's name/description/schema, in a stable order (declaration order below).
const std::vector<ToolSpec> &tool_specs();

// content_text is what an MCP client sees in the tools/call response's content[0].text (the
// serialized KernelResult, or an error message). is_error mirrors MCP's convention of reporting a
// tool-level failure as a normal JSON-RPC success with isError=true rather than a JSON-RPC-level
// error -- see mcp/main.cpp.
struct ToolCallResult {
    std::string content_text;
    bool is_error = false;
};

// Looks up tool_name, builds the matching KernelCommand from arguments, executes it against kernel,
// and serializes the result via kernel_result_to_json. An unknown tool_name, a missing/malformed
// argument (nlohmann::json throws on both), or an exception from kernel.execute() itself (e.g.
// commit_retraction against an unknown id) all come back as is_error=true rather than throwing --
// mcp/main.cpp never needs its own exception guard around this call.
ToolCallResult handle_tool_call(KnowledgeKernel &kernel, const std::string &tool_name, const nlohmann::json &arguments);

} // namespace knk::mcp
