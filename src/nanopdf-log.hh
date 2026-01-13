// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// nanopdf logging utilities
//

#ifndef NANOPDF_LOG_HH_
#define NANOPDF_LOG_HH_

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace nanopdf {
namespace log {

// Log levels in increasing verbosity order
enum class Level {
  kNone = 0,   // No logging
  kError = 1,  // Errors only
  kWarn = 2,   // Warnings and errors
  kInfo = 3,   // Informational messages
  kDebug = 4,  // Debug messages
  kTrace = 5   // Trace-level (very verbose)
};

// Global log level - can be changed at runtime
// Default level is controlled by NANOPDF_LOG_LEVEL compile flag
#ifndef NANOPDF_LOG_LEVEL
#define NANOPDF_LOG_LEVEL 3  // Default: Info
#endif

inline Level& global_log_level() {
  static Level level = static_cast<Level>(NANOPDF_LOG_LEVEL);
  return level;
}

inline void set_log_level(Level level) { global_log_level() = level; }

inline Level get_log_level() { return global_log_level(); }

// Convert log level to string
inline const char* level_to_string(Level level) {
  switch (level) {
    case Level::kNone:
      return "NONE";
    case Level::kError:
      return "ERROR";
    case Level::kWarn:
      return "WARN";
    case Level::kInfo:
      return "INFO";
    case Level::kDebug:
      return "DEBUG";
    case Level::kTrace:
      return "TRACE";
    default:
      return "UNKNOWN";
  }
}

// Parse log level from string (case-insensitive)
inline Level level_from_string(const char* str) {
  if (!str) return Level::kInfo;

  // Simple case-insensitive comparison
  auto iequals = [](const char* a, const char* b) {
    while (*a && *b) {
      char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
      char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
      if (ca != cb) return false;
      ++a;
      ++b;
    }
    return *a == *b;
  };

  if (iequals(str, "none") || iequals(str, "0")) return Level::kNone;
  if (iequals(str, "error") || iequals(str, "1")) return Level::kError;
  if (iequals(str, "warn") || iequals(str, "warning") || iequals(str, "2"))
    return Level::kWarn;
  if (iequals(str, "info") || iequals(str, "3")) return Level::kInfo;
  if (iequals(str, "debug") || iequals(str, "4")) return Level::kDebug;
  if (iequals(str, "trace") || iequals(str, "5")) return Level::kTrace;

  return Level::kInfo;  // Default
}

// Log output function type for custom handlers
using LogHandler = void (*)(Level level, const char* tag, const char* message);

// Default log handler - prints to stderr
inline void default_log_handler(Level level, const char* tag,
                                const char* message) {
  const char* level_str = level_to_string(level);
  if (tag && tag[0]) {
    fprintf(stderr, "[%s] [%s] %s\n", level_str, tag, message);
  } else {
    fprintf(stderr, "[%s] %s\n", level_str, message);
  }
}

// Global log handler
inline LogHandler& global_log_handler() {
  static LogHandler handler = default_log_handler;
  return handler;
}

inline void set_log_handler(LogHandler handler) {
  global_log_handler() = handler ? handler : default_log_handler;
}

// Core logging function
inline void log_message(Level level, const char* tag, const char* fmt, ...) {
  if (level > global_log_level()) return;

  char buffer[2048];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  global_log_handler()(level, tag, buffer);
}

// Convenience macros for logging with automatic level checks
// These check the level at compile time when possible for zero overhead

#define NANOPDF_LOG(level, tag, ...)                            \
  do {                                                          \
    if (static_cast<int>(level) <=                              \
        static_cast<int>(nanopdf::log::global_log_level())) {   \
      nanopdf::log::log_message(level, tag, __VA_ARGS__);       \
    }                                                           \
  } while (0)

#define NANOPDF_LOG_ERROR(tag, ...) \
  NANOPDF_LOG(nanopdf::log::Level::kError, tag, __VA_ARGS__)

#define NANOPDF_LOG_WARN(tag, ...) \
  NANOPDF_LOG(nanopdf::log::Level::kWarn, tag, __VA_ARGS__)

#define NANOPDF_LOG_INFO(tag, ...) \
  NANOPDF_LOG(nanopdf::log::Level::kInfo, tag, __VA_ARGS__)

#define NANOPDF_LOG_DEBUG(tag, ...) \
  NANOPDF_LOG(nanopdf::log::Level::kDebug, tag, __VA_ARGS__)

#define NANOPDF_LOG_TRACE(tag, ...) \
  NANOPDF_LOG(nanopdf::log::Level::kTrace, tag, __VA_ARGS__)

// Helper to format hex bytes
inline std::string format_hex_bytes(const uint8_t* data, size_t size,
                                    size_t max_bytes = 32) {
  std::string result;
  result.reserve(max_bytes * 3);
  size_t n = (size < max_bytes) ? size : max_bytes;
  for (size_t i = 0; i < n; i++) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02x ", data[i]);
    result += buf;
  }
  if (size > max_bytes) {
    result += "...";
  }
  return result;
}

}  // namespace log
}  // namespace nanopdf

#endif  // NANOPDF_LOG_HH_
