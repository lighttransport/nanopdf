//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//
// MCP (Model Context Protocol) server for nanopdf
// Provides PDF document access to AI assistants via JSON-RPC over stdio
//

#include "mcp-json.hh"
#include "mcp-protocol.hh"
#include "mcp-tools.hh"
#include "mcp-transport.hh"

#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {

nanopdf::mcp::StdioTransport* g_transport = nullptr;

void signal_handler(int signum) {
  nanopdf::mcp::StdioTransport::log(nanopdf::mcp::LogLevel::Info,
                                     "Received signal " + std::to_string(signum) + ", shutting down...");
  if (g_transport) {
    g_transport->shutdown();
  }
  std::exit(0);
}

}  // namespace

int main(int argc, char** argv) {
  using namespace nanopdf::mcp;

  // Set up signal handlers
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Log startup
  StdioTransport::log(LogLevel::Info, "nanopdf MCP server starting...");

  // Create server info
  ServerInfo info;
  info.name = "nanopdf-mcp";
  info.version = "0.1.0";
  info.description = "MCP server for nanopdf - lightweight PDF parsing";

  // Set capabilities
  ServerCapabilities caps;
  caps.tools = true;
  caps.resources = false;
  caps.prompts = false;
  caps.logging = false;

  // Create protocol handler
  ProtocolHandler protocol(info, caps);

  // Register tools/list handler
  protocol.register_request_handler("tools/list", [](const JsonValue& params) -> JsonValue {
    auto& registry = get_tool_registry();
    auto tools = registry.list_tools();

    JsonValue result = JsonValue::object();
    JsonValue tools_array = JsonValue::array();

    for (const auto& tool : tools) {
      JsonValue tool_obj = JsonValue::object();
      tool_obj["name"] = JsonValue(tool.name);
      tool_obj["description"] = JsonValue(tool.description);
      tool_obj["inputSchema"] = tool.input_schema;
      tools_array.push_back(tool_obj);
    }

    result["tools"] = tools_array;
    return result;
  });

  // Register tools/call handler
  protocol.register_request_handler("tools/call", [](const JsonValue& params) -> JsonValue {
    if (!params.has("name")) {
      JsonValue error = JsonValue::object();
      error["error"] = JsonValue(true);
      error["code"] = JsonValue(JsonRpcError::InvalidParams);
      error["message"] = JsonValue("Missing required parameter: 'name'");
      return error;
    }

    std::string tool_name = params["name"].get_string();
    JsonValue arguments = params.has("arguments") ? params["arguments"] : JsonValue::object();

    auto& registry = get_tool_registry();
    ToolResult result = registry.call_tool(tool_name, arguments);

    if (result.is_error) {
      JsonValue error = JsonValue::object();
      error["error"] = JsonValue(true);
      error["code"] = JsonValue(JsonRpcError::InternalError);
      error["message"] = JsonValue("Tool execution failed");
      error["data"] = result.content;
      return error;
    }

    JsonValue response = JsonValue::object();
    response["content"] = result.content;
    response["isError"] = JsonValue(result.is_error);
    return response;
  });

  // Create transport
  StdioTransport transport;
  g_transport = &transport;

  // Set message callback
  transport.set_message_callback([&protocol, &transport](const std::string& message) {
    // Process message through protocol handler
    auto result = protocol.process_message(message);

    if (!result.success) {
      StdioTransport::log(LogLevel::Error, "Failed to process message: " + result.error);
      return;
    }

    // Send response (if any)
    if (!result.response.empty()) {
      transport.send(result.response);
    }
  });

  // Register all PDF tools
  register_all_tools();

  StdioTransport::log(LogLevel::Info, "Server initialized, waiting for requests...");

  // Run event loop (blocks until shutdown)
  transport.run();

  StdioTransport::log(LogLevel::Info, "Server shutdown complete");
  g_transport = nullptr;

  return 0;
}
