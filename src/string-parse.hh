// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Exception-free string → number parsers for nanopdf.
//
// Motivation: PDF content streams contain untrusted numeric literals.
// `std::stoi` / `std::stof` / `std::stod` throw `std::invalid_argument` or
// `std::out_of_range` on malformed input — a malformed PDF can crash the
// library just by reaching these calls. The functions below are portable,
// locale-independent, and signal failure through a `bool` return, leaving
// the output pointer untouched on error.
//
// Integer parsers are hand-written (ported from tinyusdz/src/tiny-string.cc).
// Float parsers use the C `strtod`/`strtof` entry points, which do not throw.
// Callers are expected to normalize locale-sensitive decimal separators
// before calling; PDF strings already use '.'.

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "pdf-content-scan.h"

namespace nanopdf {

// Parse a decimal signed 32-bit integer. Optional leading '+' or '-'. No
// whitespace, no base prefix, no thousands separators, no scientific notation.
// Returns false on empty / non-digit / overflow input; *ret is unchanged.
inline bool parse_int(std::string_view sv, int32_t *ret) {
  if (!ret || sv.empty()) return false;
  size_t i = 0;
  bool negative = false;
  if (sv[0] == '-') { negative = true; i = 1; }
  else if (sv[0] == '+') { i = 1; }
  if (i >= sv.size()) return false;
  int64_t result = 0;
  for (; i < sv.size(); ++i) {
    char c = sv[i];
    if (c < '0' || c > '9') return false;
    result = result * 10 + (c - '0');
    if (negative &&
        result > static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1)
      return false;
    if (!negative && result > std::numeric_limits<int32_t>::max()) return false;
  }
  *ret = static_cast<int32_t>(negative ? -result : result);
  return true;
}

// Parse a decimal signed 64-bit integer.
inline bool parse_int64(std::string_view sv, int64_t *ret) {
  if (!ret || sv.empty()) return false;
  size_t i = 0;
  bool negative = false;
  if (sv[0] == '-') { negative = true; i = 1; }
  else if (sv[0] == '+') { i = 1; }
  if (i >= sv.size()) return false;
  uint64_t result = 0;
  for (; i < sv.size(); ++i) {
    char c = sv[i];
    if (c < '0' || c > '9') return false;
    uint64_t digit = static_cast<uint64_t>(c - '0');
    // overflow check before multiply/add
    if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10ull)
      return false;
    result = result * 10ull + digit;
    if (negative &&
        result >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ull)
      return false;
    if (!negative &&
        result > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return false;
  }
  *ret = negative ? -static_cast<int64_t>(result)
                  : static_cast<int64_t>(result);
  return true;
}

// Parse a decimal unsigned 32-bit integer. Optional leading '+'.
inline bool parse_uint(std::string_view sv, uint32_t *ret) {
  if (!ret || sv.empty()) return false;
  size_t i = 0;
  if (sv[0] == '+') i = 1;
  if (i >= sv.size()) return false;
  uint64_t result = 0;
  for (; i < sv.size(); ++i) {
    char c = sv[i];
    if (c < '0' || c > '9') return false;
    result = result * 10 + static_cast<uint64_t>(c - '0');
    if (result > std::numeric_limits<uint32_t>::max()) return false;
  }
  *ret = static_cast<uint32_t>(result);
  return true;
}

// Parse a decimal unsigned 64-bit integer.
inline bool parse_uint64(std::string_view sv, uint64_t *ret) {
  if (!ret || sv.empty()) return false;
  size_t i = 0;
  if (sv[0] == '+') i = 1;
  if (i >= sv.size()) return false;
  uint64_t result = 0;
  for (; i < sv.size(); ++i) {
    char c = sv[i];
    if (c < '0' || c > '9') return false;
    uint64_t digit = static_cast<uint64_t>(c - '0');
    if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10ull)
      return false;
    result = result * 10ull + digit;
  }
  *ret = result;
  return true;
}

// Parse a floating-point value. Accepts scientific notation and optional sign.
// Consumes the entire input (no trailing garbage). Does not throw.
inline bool parse_double(std::string_view sv, double *ret) {
  if (!ret || sv.empty()) return false;
  return npdf_parse_double_span(sv.data(), sv.size(), ret) != 0;
}

inline bool parse_float(std::string_view sv, float *ret) {
  if (!ret || sv.empty()) return false;
  return npdf_parse_float_span(sv.data(), sv.size(), ret) != 0;
}

// Parse an unsigned integer in an arbitrary base in [2, 36]. No prefix.
// Case-insensitive for bases > 10.
inline bool parse_uint_base(std::string_view sv, int base, uint32_t *ret) {
  if (!ret || sv.empty() || base < 2 || base > 36) return false;
  uint64_t result = 0;
  for (char c : sv) {
    int digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') digit = c - 'A' + 10;
    else return false;
    if (digit >= base) return false;
    result = result * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
    if (result > std::numeric_limits<uint32_t>::max()) return false;
  }
  *ret = static_cast<uint32_t>(result);
  return true;
}

inline uint32_t stou_base_or(std::string_view sv, int base,
                              uint32_t fallback = 0) {
  uint32_t v = fallback;
  return parse_uint_base(sv, base, &v) ? v : fallback;
}

// Parse an unsigned hexadecimal integer (no "0x" prefix). Case-insensitive.
// Rejects empty input and non-hex characters.
inline bool parse_hex_uint(std::string_view sv, uint32_t *ret) {
  if (!ret || sv.empty()) return false;
  uint64_t result = 0;
  for (char c : sv) {
    uint32_t digit;
    if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
    else return false;
    result = (result << 4) | digit;
    if (result > std::numeric_limits<uint32_t>::max()) return false;
  }
  *ret = static_cast<uint32_t>(result);
  return true;
}

// Parse a decimal unsigned 32-bit integer, stopping at the first non-digit.
// On success returns true and writes the number of characters consumed into
// *consumed. Returns false only if no digits could be read.
inline bool parse_uint_consumed(std::string_view sv, uint32_t *ret,
                                size_t *consumed) {
  if (!ret || !consumed) return false;
  *consumed = 0;
  uint64_t result = 0;
  size_t i = 0;
  for (; i < sv.size(); ++i) {
    char c = sv[i];
    if (c < '0' || c > '9') break;
    result = result * 10 + static_cast<uint64_t>(c - '0');
    if (result > std::numeric_limits<uint32_t>::max()) return false;
  }
  if (i == 0) return false;
  *ret = static_cast<uint32_t>(result);
  *consumed = i;
  return true;
}

// ---------------------------------------------------------------------------
// Convenience wrappers that return a default value on parse failure. Use
// these when replacing `std::stoi(s)` / `std::stof(s)` / `std::stod(s)` call
// sites where the original code assumed success and would have thrown on
// malformed input. Returning a sentinel (0 / 0.0) keeps control flow simple
// while making the library safe against adversarial PDF content.

inline int32_t stoi_or(std::string_view sv, int32_t fallback = 0) {
  int32_t v = fallback;
  return parse_int(sv, &v) ? v : fallback;
}

inline int64_t stoll_or(std::string_view sv, int64_t fallback = 0) {
  int64_t v = fallback;
  return parse_int64(sv, &v) ? v : fallback;
}

inline uint32_t stou_or(std::string_view sv, uint32_t fallback = 0) {
  uint32_t v = fallback;
  return parse_uint(sv, &v) ? v : fallback;
}

inline uint64_t stoull_or(std::string_view sv, uint64_t fallback = 0) {
  uint64_t v = fallback;
  return parse_uint64(sv, &v) ? v : fallback;
}

inline float stof_or(std::string_view sv, float fallback = 0.0f) {
  float v = fallback;
  return parse_float(sv, &v) ? v : fallback;
}

inline double stod_or(std::string_view sv, double fallback = 0.0) {
  double v = fallback;
  return parse_double(sv, &v) ? v : fallback;
}

}  // namespace nanopdf
