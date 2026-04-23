//
// Copyright 2025, nanopdf authors
// SPDX-License-Identifier: MIT
//

#include "mcp-transport.hh"

#include <ctime>
#include <iostream>
#include <sstream>

namespace nanopdf {
namespace mcp {

StdioTransport::StdioTransport() : shutdown_requested_(false) {}

void StdioTransport::set_message_callback(MessageCallback callback) {
  callback_ = callback;
}

bool StdioTransport::send(const std::string& message) {
  if (message.empty()) {
    return true;  // Nothing to send
  }

  // Write to stdout with newline
  std::cout << message << std::endl;
  std::cout.flush();
  return true;
}

void StdioTransport::run() {
  std::string line;

  while (!shutdown_requested_) {
    // Read line from stdin
    if (!std::getline(std::cin, line)) {
      // EOF or error
      break;
    }

    // Skip empty lines
    if (line.empty()) {
      continue;
    }

    // Call message callback
    if (callback_) {
      callback_(line);
    }
  }

  log(LogLevel::Info, "Transport shutdown");
}

void StdioTransport::shutdown() {
  shutdown_requested_ = true;
}

void StdioTransport::log(LogLevel level, const std::string& message) {
  // Format: [TIMESTAMP] [LEVEL] message
  std::time_t now = std::time(nullptr);
  char time_buf[64];
  std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

  const char* level_str = "INFO";
  switch (level) {
    case LogLevel::Debug: level_str = "DEBUG"; break;
    case LogLevel::Info: level_str = "INFO"; break;
    case LogLevel::Warning: level_str = "WARN"; break;
    case LogLevel::Error: level_str = "ERROR"; break;
  }

  std::cerr << "[" << time_buf << "] [" << level_str << "] " << message << std::endl;
  std::cerr.flush();
}

}  // namespace mcp
}  // namespace nanopdf
