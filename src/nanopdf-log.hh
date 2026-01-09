#pragma once

// Logging macros for nanopdf
// Set NANOPDF_DEBUG_PRINT to enable debug output

#include <cstdio>
#include <string>
#include <algorithm>

namespace nanopdf {
namespace log {

inline std::string format_hex_bytes(const uint8_t* data, size_t size, size_t max_bytes = 16) {
  std::string result;
  size_t n = std::min(size, max_bytes);
  char buf[4];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof(buf), "%02x ", data[i]);
    result += buf;
  }
  if (size > max_bytes) {
    result += "...";
  }
  return result;
}

}  // namespace log
}  // namespace nanopdf

#if defined(NANOPDF_DEBUG_PRINT) && NANOPDF_DEBUG_PRINT > 0

#define NANOPDF_LOG_ERROR(tag, fmt, ...) \
  do { std::fprintf(stderr, "[ERROR][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define NANOPDF_LOG_WARN(tag, fmt, ...) \
  do { std::fprintf(stderr, "[WARN][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#define NANOPDF_LOG_DEBUG(tag, fmt, ...) \
  do { std::fprintf(stderr, "[DEBUG][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#if NANOPDF_DEBUG_PRINT > 1
#define NANOPDF_LOG_TRACE(tag, fmt, ...) \
  do { std::fprintf(stderr, "[TRACE][%s] " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#else
#define NANOPDF_LOG_TRACE(tag, fmt, ...) do {} while (0)
#endif

#else  // NANOPDF_DEBUG_PRINT

#define NANOPDF_LOG_ERROR(tag, fmt, ...) do {} while (0)
#define NANOPDF_LOG_WARN(tag, fmt, ...) do {} while (0)
#define NANOPDF_LOG_DEBUG(tag, fmt, ...) do {} while (0)
#define NANOPDF_LOG_TRACE(tag, fmt, ...) do {} while (0)

#endif  // NANOPDF_DEBUG_PRINT
