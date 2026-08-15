// The stdio JSON-RPC loop: MCP's stdio transport framing is one JSON-RPC 2.0 message per line on
// stdin/stdout (no Content-Length framing, unlike LSP). stdout is reserved entirely for protocol
// messages -- anything diagnostic goes to stderr instead, since a stray stdout write would corrupt
// the stream for whatever MCP client has this process as a subprocess.
//
// This file is intentionally thin: request parsing/dispatch for tools/list and tools/call defers
// immediately to knk::mcp::tool_specs()/handle_tool_call() (mcp_tools.hpp), which is the unit-tested,
// I/O-free half of the server. Nothing here is unit tested, mirroring how other executable entry
// points in this repo (kernel_demo, the benchmarks) aren't -- verified instead by piping requests
// into the built binary manually.

#include <iostream>
#include <optional>
#include <string>

#include "kernel/knowledge_kernel.hpp"
#include "kernel/mcp_tools.hpp"
#include "kernel/storage_config.hpp"

#include <nlohmann/json.hpp>

using namespace knk;

namespace {

constexpr const char *PROTOCOL_VERSION = "2025-06-18";
constexpr const char *SERVER_NAME = "knk-mcp-server";
constexpr const char *SERVER_VERSION = "0.1.0";

void write_message(const nlohmann::json &message) {
    std::cout << message.dump() << "\n";
    std::cout.flush();
}

nlohmann::json rpc_result(const nlohmann::json &id, nlohmann::json result) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

nlohmann::json rpc_error(const nlohmann::json &id, int code, const std::string &message) {
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

nlohmann::json handle_initialize() {
    return nlohmann::json{{"protocolVersion", PROTOCOL_VERSION},
                          {"capabilities", {{"tools", nlohmann::json::object()}}},
                          {"serverInfo", {{"name", SERVER_NAME}, {"version", SERVER_VERSION}}}};
}

nlohmann::json handle_tools_list() {
    nlohmann::json tools = nlohmann::json::array();

    for (const auto &spec : mcp::tool_specs()) {
        tools.push_back(
            nlohmann::json{{"name", spec.name}, {"description", spec.description}, {"inputSchema", spec.input_schema}});
    }

    return nlohmann::json{{"tools", tools}};
}

nlohmann::json handle_tools_call(KnowledgeKernel &kernel, const nlohmann::json &params) {
    std::string name = params.at("name").get<std::string>();
    nlohmann::json arguments = params.value("arguments", nlohmann::json::object());

    mcp::ToolCallResult call_result = mcp::handle_tool_call(kernel, name, arguments);

    return nlohmann::json{{"content", nlohmann::json::array({{{"type", "text"}, {"text", call_result.content_text}}})},
                          {"isError", call_result.is_error}};
}

// Dispatches one JSON-RPC request/notification. Returns std::nullopt for notifications (no "id"
// field), since JSON-RPC notifications never get a response, by spec.
std::optional<nlohmann::json> handle_request(KnowledgeKernel &kernel, const nlohmann::json &request) {
    bool is_notification = !request.contains("id");
    nlohmann::json id = is_notification ? nlohmann::json(nullptr) : request.at("id");

    std::string method;
    try {
        method = request.at("method").get<std::string>();
    } catch (const std::exception &) {
        return is_notification ? std::nullopt : std::optional<nlohmann::json>(rpc_error(id, -32600, "invalid request"));
    }

    if (method == "notifications/initialized" || method == "notifications/cancelled") {
        return std::nullopt;
    }

    try {
        if (method == "initialize") {
            return rpc_result(id, handle_initialize());
        }
        if (method == "tools/list") {
            return rpc_result(id, handle_tools_list());
        }
        if (method == "tools/call") {
            return rpc_result(id, handle_tools_call(kernel, request.at("params")));
        }

        return is_notification ? std::nullopt
                               : std::optional<nlohmann::json>(rpc_error(id, -32601, "method not found: " + method));
    } catch (const std::exception &error) {
        return is_notification ? std::nullopt : std::optional<nlohmann::json>(rpc_error(id, -32602, error.what()));
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "mcp_server") << " <storage-root>\n";
        return 1;
    }

    std::optional<KnowledgeKernel> kernel_storage;
    try {
        kernel_storage.emplace(StorageConfig{std::filesystem::path(argv[1])});
    } catch (const std::exception &error) {
        std::cerr << "mcp_server: failed to open storage root: " << error.what() << "\n";
        return 1;
    }
    KnowledgeKernel &kernel = *kernel_storage;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        nlohmann::json request;
        try {
            request = nlohmann::json::parse(line);
        } catch (const std::exception &error) {
            write_message(rpc_error(nullptr, -32700, error.what()));
            continue;
        }

        auto response = handle_request(kernel, request);
        if (response.has_value()) {
            write_message(*response);
        }
    }

    return 0;
}
