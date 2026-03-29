//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#ifndef NANOPDF_MCP_PROTOCOL_HH_
#define NANOPDF_MCP_PROTOCOL_HH_

#include "mcp-json.hh"

#include <functional>
#include <string>

namespace nanopdf {
namespace mcp {

// JSON-RPC error codes
namespace JsonRpcError {
  constexpr int ParseError = -32700;
  constexpr int InvalidRequest = -32600;
  constexpr int MethodNotFound = -32601;
  constexpr int InvalidParams = -32602;
  constexpr int InternalError = -32603;
}

// Protocol state machine
enum class ProtocolState {
  Uninitialized,
  Initializing,
  Ready,
  Shutdown
};

// Server information
struct ServerInfo {
  std::string name;
  std::string version;
  std::string description;
};

// Server capabilities
struct ServerCapabilities {
  bool tools;
  bool resources;
  bool prompts;
  bool logging;

  ServerCapabilities()
      : tools(false), resources(false), prompts(false), logging(false) {}
};

// JSON-RPC message structure
struct JsonRpcMessage {
  std::string jsonrpc;  // Always "2.0"
  JsonValue id;         // Request ID (can be number, string, or null)
  std::string method;   // Method name
  JsonValue params;     // Parameters object
  JsonValue result;     // Response result
  JsonValue error;      // Error object

  JsonRpcMessage() : jsonrpc("2.0") {}

  // Serialize to JSON string
  std::string to_json(bool pretty = false) const;

  // Parse from JSON string
  static JsonRpcMessage from_json(const std::string& json, bool* success = nullptr);

  // Helper: create request
  static JsonRpcMessage request(const JsonValue& id, const std::string& method, const JsonValue& params = JsonValue::object());

  // Helper: create response
  static JsonRpcMessage response(const JsonValue& id, const JsonValue& result);

  // Helper: create error response
  static JsonRpcMessage error_response(const JsonValue& id, int code, const std::string& message, const JsonValue& data = JsonValue::null());

  // Helper: create notification (no id)
  static JsonRpcMessage notification(const std::string& method, const JsonValue& params = JsonValue::object());

  // Check if this is a notification (no id)
  bool is_notification() const { return id.is_null() && !method.empty(); }

  // Check if this is a request
  bool is_request() const { return !id.is_null() && !method.empty(); }

  // Check if this is a response
  bool is_response() const { return !id.is_null() && method.empty(); }
};

// Request handler callback
using RequestHandler = std::function<JsonValue(const JsonValue& params)>;

// Process result
struct ProcessResult {
  bool success;
  std::string response;  // JSON response to send (empty if notification)
  std::string error;     // Error message (for logging)

  ProcessResult() : success(false) {}
  static ProcessResult ok(const std::string& resp = "") {
    ProcessResult r;
    r.success = true;
    r.response = resp;
    return r;
  }
  static ProcessResult fail(const std::string& err) {
    ProcessResult r;
    r.success = false;
    r.error = err;
    return r;
  }
};

// Protocol handler
class ProtocolHandler {
 public:
  ProtocolHandler(const ServerInfo& info, const ServerCapabilities& caps);
  ~ProtocolHandler() = default;

  // Process incoming JSON-RPC message
  ProcessResult process_message(const std::string& json);

  // Register custom request handler
  void register_request_handler(const std::string& method, RequestHandler handler);

  // Get current state
  ProtocolState state() const { return state_; }

  // Server info
  const ServerInfo& server_info() const { return server_info_; }
  const ServerCapabilities& capabilities() const { return capabilities_; }

 private:
  // Built-in method handlers
  JsonValue handle_initialize(const JsonValue& params);
  JsonValue handle_initialized(const JsonValue& params);
  JsonValue handle_ping(const JsonValue& params);

  // Dispatch request to handler
  ProcessResult dispatch_request(const JsonRpcMessage& msg);

  ServerInfo server_info_;
  ServerCapabilities capabilities_;
  ProtocolState state_;
  std::map<std::string, RequestHandler> handlers_;

  // Client info (from initialize)
  std::string client_name_;
  std::string client_version_;
};

}  // namespace mcp
}  // namespace nanopdf

#endif  // NANOPDF_MCP_PROTOCOL_HH_
