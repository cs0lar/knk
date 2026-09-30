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

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "kernel/knowledge_kernel.hpp"
#include "kernel/mcp_tools.hpp"
#include "kernel/storage_config.hpp"
#include <system_error>

#include <nlohmann/json.hpp>

using namespace knk;

namespace {

constexpr const char *PROTOCOL_VERSION = "2025-06-18";
constexpr const char *SERVER_NAME = "knk-mcp-server";
constexpr const char *SERVER_VERSION = "0.1.0";

void write_message(const nlohmann::ordered_json &message) {
    std::cout << message.dump() << "\n";
    std::cout.flush();
}

nlohmann::ordered_json rpc_result(const nlohmann::json &id, nlohmann::ordered_json result) {
    return nlohmann::ordered_json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

nlohmann::ordered_json rpc_error(const nlohmann::json &id, int code, const std::string &message) {
    return nlohmann::ordered_json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

nlohmann::ordered_json handle_initialize() {
    return nlohmann::ordered_json{{"protocolVersion", PROTOCOL_VERSION},
                                  {"capabilities", {{"tools", nlohmann::ordered_json::object()}}},
                                  {"serverInfo", {{"name", SERVER_NAME}, {"version", SERVER_VERSION}}}};
}

// Built with ordered_json end-to-end (not just ToolSpec::input_schema): assigning an ordered_json
// value into a plain nlohmann::json object re-sorts its keys on the spot, so every container this
// schema passes through on its way to the wire -- the per-tool entry, the "tools" array, the
// "result" envelope -- has to stay ordered_json too, or the property order the client depends on
// (see AGENTS.md's "MCP parameter ordering" rule) dies at whichever level converts back to json.
nlohmann::ordered_json handle_tools_list() {
    nlohmann::ordered_json tools = nlohmann::ordered_json::array();

    for (const auto &spec : mcp::tool_specs()) {
        tools.push_back(nlohmann::ordered_json{
            {"name", spec.name}, {"description", spec.description}, {"inputSchema", spec.input_schema}});
    }

    return nlohmann::ordered_json{{"tools", tools}};
}

nlohmann::ordered_json handle_tools_call(KnowledgeKernel &kernel, const nlohmann::json &params,
                                         const mcp::ToolContext &context) {
    std::string name = params.at("name").get<std::string>();
    nlohmann::json arguments = params.value("arguments", nlohmann::json::object());

    mcp::ToolCallResult call_result = mcp::handle_tool_call(kernel, name, arguments, context);

    return nlohmann::ordered_json{
        {"content", nlohmann::ordered_json::array({{{"type", "text"}, {"text", call_result.content_text}}})},
        {"isError", call_result.is_error}};
}

// Dispatches one JSON-RPC request/notification. Returns std::nullopt for notifications (no "id"
// field), since JSON-RPC notifications never get a response, by spec.
std::optional<nlohmann::ordered_json> handle_request(KnowledgeKernel &kernel, const nlohmann::json &request,
                                                     const mcp::ToolContext &context) {
    bool is_notification = !request.contains("id");
    nlohmann::json id = is_notification ? nlohmann::json(nullptr) : request.at("id");

    std::string method;
    try {
        method = request.at("method").get<std::string>();
    } catch (const std::exception &) {
        return is_notification ? std::nullopt
                               : std::optional<nlohmann::ordered_json>(rpc_error(id, -32600, "invalid request"));
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
            return rpc_result(id, handle_tools_call(kernel, request.at("params"), context));
        }

        return is_notification
                   ? std::nullopt
                   : std::optional<nlohmann::ordered_json>(rpc_error(id, -32601, "method not found: " + method));
    } catch (const std::exception &error) {
        return is_notification ? std::nullopt
                               : std::optional<nlohmann::ordered_json>(rpc_error(id, -32602, error.what()));
    }
}

} // namespace

int main(int argc, char **argv) {
    // --read-only (Phase 13) is how an analytics process actually gets at a live store: it opens the
    // root without the writer lock, so it coexists with whatever process is writing, and every mutating
    // tool answers with an error instead of writing. It is a snapshot as of startup -- this process
    // replays once at open, so commits made afterwards need a restart to be seen.
    std::filesystem::path root;
    OpenMode mode = OpenMode::ReadWrite;
    mcp::ToolContext context;
    bool usage_error = false;

    for (int i = 1; i < argc; ++i) {
        std::string argument(argv[i]);
        if (argument == "--read-only") {
            mode = OpenMode::ReadOnly;
        } else if (argument == "--spill-dir") {
            // Taken from the command line rather than from a tool argument on purpose: a client naming the
            // path would be naming somewhere for this process to write.
            if (i + 1 >= argc) {
                usage_error = true;
            } else {
                context.spill_directory = std::filesystem::path(argv[++i]);
            }
        } else if (!argument.empty() && argument[0] == '-') {
            usage_error = true;
        } else if (root.empty()) {
            root = std::filesystem::path(argument);
        } else {
            usage_error = true;
        }
    }

    if (usage_error || root.empty()) {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "mcp_server")
                  << " <storage-root> [--read-only] [--spill-dir DIR]\n";
        return 1;
    }

    std::optional<KnowledgeKernel> kernel_storage;
    try {
        kernel_storage.emplace(StorageConfig{root}, mode);
    } catch (const std::exception &error) {
        std::cerr << "mcp_server: failed to open storage root: " << error.what() << "\n";
        return 1;
    }

    if (!context.spill_directory.empty()) {
        // Created up front so query_spill does not fail on its first call, and so a misconfigured path
        // fails at startup where an operator will see it.
        std::error_code error;
        std::filesystem::create_directories(context.spill_directory, error);
        if (error) {
            std::cerr << "mcp_server: cannot create spill directory '" << context.spill_directory.string()
                      << "': " << error.message() << "\n";
            return 1;
        }
        std::cerr << "mcp_server: spilling results to '" << context.spill_directory.string() << "'\n";
    }

    if (mode == OpenMode::ReadOnly) {
        std::cerr << "mcp_server: opened '" << root.string() << "' read-only (no writer lock; writes will fail)\n";
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

        auto response = handle_request(kernel, request, context);
        if (response.has_value()) {
            write_message(*response);
        }
    }

    return 0;
}
