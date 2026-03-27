//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#ifndef NANOPDF_MCP_TRANSPORT_HH_
#define NANOPDF_MCP_TRANSPORT_HH_

#include <functional>
#include <string>

namespace nanopdf {
namespace mcp {

// Log levels
enum class LogLevel {
  Debug,
  Info,
  Warning,
  Error
};

// Message callback
using MessageCallback = std::function<void(const std::string&)>;

// stdio transport for MCP
// Reads newline-delimited JSON from stdin
// Writes newline-delimited JSON to stdout
// Logs to stderr
class StdioTransport {
 public:
  StdioTransport();
  ~StdioTransport() = default;

  // Set message callback (called when a line is read from stdin)
  void set_message_callback(MessageCallback callback);

  // Send message to stdout (adds newline automatically)
  bool send(const std::string& message);

  // Run event loop (blocks until shutdown or EOF)
  void run();

  // Request shutdown
  void shutdown();

  // Static logging to stderr
  static void log(LogLevel level, const std::string& message);

 private:
  MessageCallback callback_;
  bool shutdown_requested_;
};

}  // namespace mcp
}  // namespace nanopdf

#endif  // NANOPDF_MCP_TRANSPORT_HH_
