//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#include "mcp-protocol.hh"

namespace nanopdf {
namespace mcp {

// ============================================================
// JsonRpcMessage implementation
// ============================================================

std::string JsonRpcMessage::to_json(bool pretty) const {
  JsonValue obj = JsonValue::object();
  obj["jsonrpc"] = JsonValue("2.0");

  // Add id if not a notification
  if (!id.is_null()) {
    obj["id"] = id;
  }

  // Request or notification
  if (!method.empty()) {
    obj["method"] = JsonValue(method);
    if (!params.is_null()) {
      obj["params"] = params;
    }
  }

  // Response
  if (!result.is_null()) {
    obj["result"] = result;
  }

  // Error
  if (!error.is_null()) {
    obj["error"] = error;
  }

  return JsonSerializer::serialize(obj, pretty);
}

JsonRpcMessage JsonRpcMessage::from_json(const std::string& json, bool* success) {
  JsonRpcMessage msg;

  auto parse_result = JsonParser::parse(json);
  if (!parse_result.success) {
    if (success) *success = false;
    return msg;
  }

  const JsonValue& obj = parse_result.value;
  if (!obj.is_object()) {
    if (success) *success = false;
    return msg;
  }

  // Check jsonrpc version
  if (obj.has("jsonrpc") && obj["jsonrpc"].is_string()) {
    msg.jsonrpc = obj["jsonrpc"].get_string();
  }

  // Get id (optional for notifications)
  if (obj.has("id")) {
    msg.id = obj["id"];
  }

  // Get method (for requests/notifications)
  if (obj.has("method") && obj["method"].is_string()) {
    msg.method = obj["method"].get_string();
  }

  // Get params (optional)
  if (obj.has("params")) {
    msg.params = obj["params"];
  }

  // Get result (for responses)
  if (obj.has("result")) {
    msg.result = obj["result"];
  }

  // Get error (for error responses)
  if (obj.has("error")) {
    msg.error = obj["error"];
  }

  if (success) *success = true;
  return msg;
}

JsonRpcMessage JsonRpcMessage::request(const JsonValue& id, const std::string& method, const JsonValue& params) {
  JsonRpcMessage msg;
  msg.id = id;
  msg.method = method;
  msg.params = params;
  return msg;
}

JsonRpcMessage JsonRpcMessage::response(const JsonValue& id, const JsonValue& result) {
  JsonRpcMessage msg;
  msg.id = id;
  msg.result = result;
  return msg;
}

JsonRpcMessage JsonRpcMessage::error_response(const JsonValue& id, int code, const std::string& message, const JsonValue& data) {
  JsonRpcMessage msg;
  msg.id = id;

  JsonValue error_obj = JsonValue::object();
  error_obj["code"] = JsonValue(code);
  error_obj["message"] = JsonValue(message);
  if (!data.is_null()) {
    error_obj["data"] = data;
  }

  msg.error = error_obj;
  return msg;
}

JsonRpcMessage JsonRpcMessage::notification(const std::string& method, const JsonValue& params) {
  JsonRpcMessage msg;
  msg.method = method;
  msg.params = params;
  // id remains null for notifications
  return msg;
}

// ============================================================
// ProtocolHandler implementation
// ============================================================

ProtocolHandler::ProtocolHandler(const ServerInfo& info, const ServerCapabilities& caps)
    : server_info_(info),
      capabilities_(caps),
      state_(ProtocolState::Uninitialized) {
  // Register built-in handlers
  handlers_["initialize"] = [this](const JsonValue& params) {
    return handle_initialize(params);
  };
  handlers_["notifications/initialized"] = [this](const JsonValue& params) {
    return handle_initialized(params);
  };
  handlers_["ping"] = [this](const JsonValue& params) {
    return handle_ping(params);
  };
}

ProcessResult ProtocolHandler::process_message(const std::string& json) {
  // Parse JSON-RPC message
  bool parse_ok = false;
  JsonRpcMessage msg = JsonRpcMessage::from_json(json, &parse_ok);

  if (!parse_ok) {
    // Parse error - send error response with null id
    auto err_msg = JsonRpcMessage::error_response(
        JsonValue::null(),
        JsonRpcError::ParseError,
        "Parse error");
    return ProcessResult::ok(err_msg.to_json());
  }

  // Validate JSON-RPC version
  if (msg.jsonrpc != "2.0") {
    auto err_msg = JsonRpcMessage::error_response(
        msg.id,
        JsonRpcError::InvalidRequest,
        "Invalid JSON-RPC version");
    return ProcessResult::ok(err_msg.to_json());
  }

  // Handle notification (no response needed)
  if (msg.is_notification()) {
    auto result = dispatch_request(msg);
    // Notifications don't get responses, even on error
    return ProcessResult::ok();
  }

  // Handle request
  if (msg.is_request()) {
    return dispatch_request(msg);
  }

  // Invalid message
  auto err_msg = JsonRpcMessage::error_response(
      msg.id,
      JsonRpcError::InvalidRequest,
      "Invalid request");
  return ProcessResult::ok(err_msg.to_json());
}

void ProtocolHandler::register_request_handler(const std::string& method, RequestHandler handler) {
  handlers_[method] = handler;
}

ProcessResult ProtocolHandler::dispatch_request(const JsonRpcMessage& msg) {
  // Find handler
  auto it = handlers_.find(msg.method);
  if (it == handlers_.end()) {
    auto err_msg = JsonRpcMessage::error_response(
        msg.id,
        JsonRpcError::MethodNotFound,
        "Method not found: " + msg.method);
    return ProcessResult::ok(err_msg.to_json());
  }

  // Execute handler
  try {
    JsonValue result = it->second(msg.params);

    // Check if result is an error object
    if (result.is_object() && result.has("error") && result["error"].is_boolean() && result["error"].get_boolean()) {
      // Handler returned an error
      int code = result.has("code") ? result["code"].get_int() : JsonRpcError::InternalError;
      std::string message = result.has("message") ? result["message"].get_string() : "Internal error";
      JsonValue data = result.has("data") ? result["data"] : JsonValue::null();

      auto err_msg = JsonRpcMessage::error_response(msg.id, code, message, data);
      return ProcessResult::ok(err_msg.to_json());
    }

    // Success response
    auto resp_msg = JsonRpcMessage::response(msg.id, result);
    return ProcessResult::ok(resp_msg.to_json());

  } catch (const std::exception& e) {
    auto err_msg = JsonRpcMessage::error_response(
        msg.id,
        JsonRpcError::InternalError,
        std::string("Internal error: ") + e.what());
    return ProcessResult::ok(err_msg.to_json());
  }
}

JsonValue ProtocolHandler::handle_initialize(const JsonValue& params) {
  if (state_ != ProtocolState::Uninitialized) {
    JsonValue error = JsonValue::object();
    error["error"] = JsonValue(true);
    error["code"] = JsonValue(JsonRpcError::InvalidRequest);
    error["message"] = JsonValue("Already initialized");
    return error;
  }

  state_ = ProtocolState::Initializing;

  // Extract client info
  if (params.has("clientInfo")) {
    const auto& client_info = params["clientInfo"];
    if (client_info.has("name")) {
      client_name_ = client_info["name"].get_string();
    }
    if (client_info.has("version")) {
      client_version_ = client_info["version"].get_string();
    }
  }

  // Build response
  JsonValue result = JsonValue::object();
  result["protocolVersion"] = JsonValue("2024-11-05");

  // Server info
  JsonValue server_info = JsonValue::object();
  server_info["name"] = JsonValue(server_info_.name);
  server_info["version"] = JsonValue(server_info_.version);
  result["serverInfo"] = server_info;

  // Capabilities
  JsonValue caps = JsonValue::object();
  if (capabilities_.tools) {
    caps["tools"] = JsonValue::object();
  }
  if (capabilities_.resources) {
    caps["resources"] = JsonValue::object();
  }
  if (capabilities_.prompts) {
    caps["prompts"] = JsonValue::object();
  }
  if (capabilities_.logging) {
    caps["logging"] = JsonValue::object();
  }
  result["capabilities"] = caps;

  return result;
}

JsonValue ProtocolHandler::handle_initialized(const JsonValue& params) {
  if (state_ != ProtocolState::Initializing) {
    JsonValue error = JsonValue::object();
    error["error"] = JsonValue(true);
    error["code"] = JsonValue(JsonRpcError::InvalidRequest);
    error["message"] = JsonValue("Not in initializing state");
    return error;
  }

  state_ = ProtocolState::Ready;

  // Notifications don't return results, but we need to return something
  // The protocol handler will not send a response for notifications
  return JsonValue::object();
}

JsonValue ProtocolHandler::handle_ping(const JsonValue& params) {
  // Simple ping/pong
  JsonValue result = JsonValue::object();
  return result;
}

}  // namespace mcp
}  // namespace nanopdf
