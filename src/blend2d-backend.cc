// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifdef NANOPDF_USE_BLEND2D

#include "blend2d-backend.hh"
#include "font-unicode-map.hh"
#include "shared-font-cache.hh"
#include "render-cache.hh"
#include "nanopdf-log.hh"
#include "string-parse.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>

// For PNG saving
#include "stb_image_write.h"
#include "cff-wrapper.hh"
#include "font-provider.hh"
#include "pdf-function.hh"

#ifdef NANOPDF_EMBED_CJK_FONTS
#include "embedded-cjk-fonts.hh"
#endif

// External C variable for PNG compression level
extern "C" int stbi_write_png_compression_level;

namespace nanopdf {

static uint64_t fnv1a64(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = 14695981039346656037ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static bool is_pdf_content_delimiter(char c) {
  return std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '[' ||
         c == ']' || c == '(' || c == ')' || c == '<' || c == '>' ||
         c == '{' || c == '}';
}

static bool is_render_progress_operator(std::string_view token) {
  return token == "f" || token == "F" || token == "f*" || token == "S" ||
         token == "s" || token == "B" || token == "B*" || token == "b" ||
         token == "b*" || token == "Tj" || token == "TJ" || token == "'" ||
         token == "\"" || token == "Do" || token == "BI" || token == "sh";
}

static void skip_inline_image_payload(std::string_view content, size_t& pos) {
  const size_t size = content.size();

  while (pos < size && std::isspace(static_cast<unsigned char>(content[pos]))) {
    pos++;
  }

  while (pos < size) {
    while (pos < size && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= size) break;

    if ((pos + 1) < size && content[pos] == 'I' && content[pos + 1] == 'D' &&
        ((pos + 2) >= size ||
         std::isspace(static_cast<unsigned char>(content[pos + 2])))) {
      pos += 2;
      if (pos < size &&
          std::isspace(static_cast<unsigned char>(content[pos]))) {
        pos++;
      }
      break;
    }

    while (pos < size &&
           !std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
  }

  while ((pos + 1) < size) {
    if (content[pos] == 'E' && content[pos + 1] == 'I' &&
        (pos == 0 ||
         std::isspace(static_cast<unsigned char>(content[pos - 1]))) &&
        ((pos + 2) >= size ||
         std::isspace(static_cast<unsigned char>(content[pos + 2])))) {
      pos += 2;
      return;
    }
    pos++;
  }
}

static size_t count_render_objects_impl(std::string_view content) {
  size_t count = 0;
  size_t pos = 0;

  while (pos < content.size()) {
    while (pos < content.size() &&
           std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.size()) break;

    if (content[pos] == '%') {
      while (pos < content.size() && content[pos] != '\n' &&
             content[pos] != '\r') {
        pos++;
      }
      continue;
    }

    if (content[pos] == '(') {
      int depth = 1;
      pos++;
      while (pos < content.size() && depth > 0) {
        if (content[pos] == '\\' && (pos + 1) < content.size()) {
          pos += 2;
        } else if (content[pos] == '(') {
          depth++;
          pos++;
        } else if (content[pos] == ')') {
          depth--;
          pos++;
        } else {
          pos++;
        }
      }
      continue;
    }

    if (content[pos] == '<') {
      if ((pos + 1) < content.size() && content[pos + 1] == '<') {
        pos += 2;
      } else {
        pos++;
        while (pos < content.size() && content[pos] != '>') {
          pos++;
        }
        if (pos < content.size()) pos++;
      }
      continue;
    }

    if (content[pos] == '[' || content[pos] == ']' || content[pos] == '{' ||
        content[pos] == '}') {
      pos++;
      continue;
    }

    size_t start = pos;
    while (pos < content.size() && !is_pdf_content_delimiter(content[pos])) {
      pos++;
    }

    if (start == pos) {
      pos++;
      continue;
    }

    std::string_view token = content.substr(start, pos - start);
    if (!is_render_progress_operator(token)) {
      continue;
    }

    count++;
    if (token == "BI") {
      skip_inline_image_payload(content, pos);
    }
  }

  return count;
}

// Computes extra progress weight for XObject Do operators beyond the
// baseline count of 1.  Returns 0 when the XObject is not found or is an
// unknown subtype.
//   Image: weight = max(1, w*h/10000) - 1
//   Form:  weight = form_operator_count - 1
static size_t compute_xobject_extra_weight(const std::string& xobj_name,
                                            const Pdf& pdf,
                                            const Dictionary& resources) {
  auto xobj_it = resources.find("XObject");
  if (xobj_it == resources.end()) return 0;
  const Value& xobj_dict = xobj_it->second;
  if (xobj_dict.type != Value::DICTIONARY) return 0;

  auto name_it = xobj_dict.dict.find(xobj_name);
  if (name_it == xobj_dict.dict.end()) return 0;

  Value entry = name_it->second;
  uint32_t obj_num = 0;
  uint16_t obj_gen = 0;
  if (entry.type == Value::REFERENCE) {
    obj_num = entry.ref_object_number;
    obj_gen = entry.ref_generation_number;
    auto res = resolve_reference(pdf, obj_num, obj_gen);
    if (!res.success) return 0;
    entry = std::move(res.value);
  }
  if (entry.type != Value::STREAM) return 0;

  auto sub_it = entry.stream.dict.find("Subtype");
  if (sub_it == entry.stream.dict.end() ||
      sub_it->second.type != Value::NAME)
    return 0;

  if (sub_it->second.name == "Image") {
    int w = 0, h = 0;
    auto w_it = entry.stream.dict.find("Width");
    auto h_it = entry.stream.dict.find("Height");
    if (w_it != entry.stream.dict.end() &&
        w_it->second.type == Value::NUMBER)
      w = static_cast<int>(w_it->second.number);
    if (h_it != entry.stream.dict.end() &&
        h_it->second.type == Value::NUMBER)
      h = static_cast<int>(h_it->second.number);
    if (w <= 0 || h <= 0) return 0;
    size_t weight =
        std::max<size_t>(1, (static_cast<size_t>(w) * h) / 10000);
    return weight - 1;  // Do already counted as 1
  }

  if (sub_it->second.name == "Form") {
    auto dec = decode_stream(pdf, entry, obj_num, obj_gen);
    if (!dec.success || dec.data.empty()) return 0;
    size_t n = count_render_objects_impl(std::string_view(
        reinterpret_cast<const char*>(dec.data.data()), dec.data.size()));
    return n - 1;  // Do already counted as 1
  }

  return 0;
}

// Scans content for "NAME Do" patterns and accumulates extra progress
// weight from Image/Form XObjects.  Uses page-level resources.
static size_t scan_do_extra_weight(std::string_view content,
                                    const Pdf& pdf,
                                    const Dictionary& resources) {
  size_t extra = 0;
  size_t pos = 0;
  std::string last_token;

  while (pos < content.size()) {
    while (pos < content.size() &&
           std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.size()) break;

    if (content[pos] == '%') {
      while (pos < content.size() && content[pos] != '\n' &&
             content[pos] != '\r') {
        pos++;
      }
      continue;
    }

    if (content[pos] == '(') {
      int depth = 1;
      pos++;
      while (pos < content.size() && depth > 0) {
        if (content[pos] == '\\' && (pos + 1) < content.size()) {
          pos += 2;
        } else if (content[pos] == '(') {
          depth++;
          pos++;
        } else if (content[pos] == ')') {
          depth--;
          pos++;
        } else {
          pos++;
        }
      }
      continue;
    }

    if (content[pos] == '<') {
      if ((pos + 1) < content.size() && content[pos + 1] == '<') {
        pos += 2;
      } else {
        pos++;
        while (pos < content.size() && content[pos] != '>') pos++;
        if (pos < content.size()) pos++;
      }
      continue;
    }

    if (content[pos] == '[' || content[pos] == ']' ||
        content[pos] == '{' || content[pos] == '}') {
      pos++;
      continue;
    }

    size_t start = pos;
    while (pos < content.size() &&
           !is_pdf_content_delimiter(content[pos])) {
      pos++;
    }
    if (start == pos) {
      pos++;
      continue;
    }

    std::string_view token(content.data() + start, pos - start);
    if (token == "Do") {
      if (!last_token.empty() && last_token[0] == '/') {
        extra += compute_xobject_extra_weight(last_token.substr(1),
                                               pdf, resources);
      }
    }
    last_token.assign(token.data(), token.size());
  }
  return extra;
}

// glyph_name_to_unicode is now in font-unicode-map.hh (shared, sorted-array+bsearch)

// WinAnsiEncoding to Unicode mapping table
static const uint16_t kWinAnsiEncoding[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF
};

// MacRomanEncoding to Unicode mapping table
static const uint16_t kMacRomanEncoding[256] = {
    0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
    0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
    0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017,
    0x0018, 0x0019, 0x001A, 0x001B, 0x001C, 0x001D, 0x001E, 0x001F,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7
};

// Adobe-Japan1 CID to Unicode mapping. Best-effort fallback when the PDF
// omits a ToUnicode CMap. Regular closed-form ranges come from the shared
// helper; this function adds a hand-maintained kanji / punctuation table
// that covers the characters observed in sample corpora.
static uint32_t adobe_japan1_cid_to_unicode(uint32_t cid) {
  if (uint32_t u = adobe_japan1_cid_to_unicode_regular(cid)) return u;

  // CID 95-96: Fullwidth tilde and overline
  if (cid == 95) return 0xFF5E;  // ～
  if (cid == 96) return 0xFFE3;  // ￣

  // CID 97-117: Japanese punctuation
  static const uint32_t cid_97_117[] = {
    0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3, 0x30A5, 0x30A7,
    0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC, 0x30A2, 0x30A4, 0x30A6, 0x30A8,
    0x30AA
  };
  if (cid >= 97 && cid <= 117) {
    return cid_97_117[cid - 97];
  }

  // Common Japanese punctuation
  if (cid == 124) return 0x3002;  // 。
  if (cid == 125) return 0x3001;  // 、
  if (cid == 126) return 0x30FB;  // ・
  if (cid == 127) return 0x300C;  // 「
  if (cid == 128) return 0x300D;  // 」

  // Kanji lookup table - expanded with ~500 common characters
  // CID mappings based on Adobe-Japan1 standard
  static const std::map<uint32_t, uint32_t> kanji_map = {
    // Basic Kanji numbers
    {1879, 0x4E00}, // 一
    {2098, 0x4E8C}, // 二
    {1900, 0x4E09}, // 三
    {2025, 0x56DB}, // 四
    {2054, 0x4E94}, // 五
    {2192, 0x516D}, // 六
    {2014, 0x4E03}, // 七
    {2191, 0x516B}, // 八
    {1938, 0x4E5D}, // 九
    {2012, 0x5341}, // 十
    {2179, 0x767E}, // 百
    {2008, 0x5343}, // 千
    {2149, 0x4E07}, // 万

    // Time-related
    {1940, 0x65E5}, // 日
    {2144, 0x6708}, // 月
    {2145, 0x5E74}, // 年
    {2266, 0x6642}, // 時
    {2146, 0x5206}, // 分
    {2103, 0x79D2}, // 秒
    {1948, 0x9031}, // 週
    {2267, 0x4ECA}, // 今
    {2268, 0x6628}, // 昨
    {2269, 0x660E}, // 明

    // Common verbs/actions
    {1891, 0x898B}, // 見
    {2070, 0x805E}, // 聞
    {2044, 0x8AAD}, // 読
    {2045, 0x66F8}, // 書
    {2063, 0x8A71}, // 話
    {1955, 0x8A00}, // 言
    {2069, 0x601D}, // 思
    {2071, 0x77E5}, // 知
    {1904, 0x4F7F}, // 使
    {1892, 0x4F5C}, // 作
    {2046, 0x5165}, // 入
    {2047, 0x51FA}, // 出
    {1893, 0x884C}, // 行
    {1894, 0x6765}, // 来
    {1895, 0x5E30}, // 帰
    {2048, 0x98DF}, // 食
    {2049, 0x98F2}, // 飲
    {1896, 0x8CB7}, // 買
    {2050, 0x58F2}, // 売
    {1897, 0x6301}, // 持
    {2051, 0x5F85}, // 待
    {1898, 0x7ACB}, // 立
    {1899, 0x5EA7}, // 座
    {2052, 0x5BDD}, // 寝
    {2053, 0x8D77}, // 起

    // People/pronouns
    {1901, 0x4EBA}, // 人
    {1902, 0x5B50}, // 子
    {1903, 0x5973}, // 女
    {2055, 0x7537}, // 男
    {1905, 0x7236}, // 父
    {2056, 0x6BCD}, // 母
    {2057, 0x5144}, // 兄
    {2058, 0x59C9}, // 姉
    {2059, 0x5F1F}, // 弟
    {2060, 0x59B9}, // 妹
    {1907, 0x53CB}, // 友
    {2061, 0x5148}, // 先
    {2062, 0x751F}, // 生

    // Places/directions
    {1908, 0x5927}, // 大
    {2256, 0x5C0F}, // 小
    {2288, 0x4E2D}, // 中
    {1909, 0x4E0A}, // 上
    {1910, 0x4E0B}, // 下
    {1911, 0x5DE6}, // 左
    {1912, 0x53F3}, // 右
    {2064, 0x524D}, // 前
    {2065, 0x5F8C}, // 後
    {2066, 0x5185}, // 内
    {2067, 0x5916}, // 外
    {1913, 0x5317}, // 北
    {1914, 0x5357}, // 南
    {1915, 0x6771}, // 東
    {1916, 0x897F}, // 西
    {1917, 0x56FD}, // 国
    {2068, 0x753A}, // 町
    {1918, 0x5E02}, // 市
    {1919, 0x6751}, // 村

    // Nature/weather
    {1920, 0x5C71}, // 山
    {1921, 0x5DDD}, // 川
    {1922, 0x6D77}, // 海
    {2074, 0x6728}, // 木
    {2075, 0x6797}, // 林
    {2076, 0x68EE}, // 森
    {1923, 0x82B1}, // 花
    {2077, 0x7A7A}, // 空
    {2078, 0x96E8}, // 雨
    {2079, 0x96EA}, // 雪
    {2080, 0x98A8}, // 風
    {1924, 0x706B}, // 火
    {1925, 0x6C34}, // 水
    {1926, 0x571F}, // 土
    {2081, 0x77F3}, // 石
    {2082, 0x91D1}, // 金

    // Body
    {2083, 0x76EE}, // 目
    {2084, 0x8033}, // 耳
    {2085, 0x53E3}, // 口
    {2086, 0x624B}, // 手
    {2087, 0x8DB3}, // 足
    {2088, 0x982D}, // 頭
    {2089, 0x9854}, // 顔
    {2090, 0x5FC3}, // 心
    {2091, 0x4F53}, // 体

    // Colors
    {2092, 0x8D64}, // 赤
    {2093, 0x9752}, // 青
    {2094, 0x767D}, // 白
    {2095, 0x9ED2}, // 黒
    {2096, 0x9EC4}, // 黄

    // School/study
    {2097, 0x5B66}, // 学
    {1927, 0x6821}, // 校
    {1928, 0x6559}, // 教
    {1929, 0x5BA4}, // 室
    {2099, 0x672C}, // 本
    {1906, 0x6587}, // 文
    {2251, 0x5B57}, // 字
    {2147, 0x8A9E}, // 語
    {2100, 0x7B54}, // 答
    {2072, 0x554F}, // 問
    {2073, 0x984C}, // 題
    {2101, 0x8A66}, // 試
    {2102, 0x9A13}, // 験

    // Objects/things
    {1930, 0x8ECA}, // 車
    {1931, 0x99C5}, // 駅
    {1932, 0x96FB}, // 電
    {2104, 0x8A71}, // 話 (phone)
    {1933, 0x5E97}, // 店
    {2105, 0x9053}, // 道
    {1934, 0x5BB6}, // 家
    {2106, 0x9580}, // 門
    {2107, 0x7A93}, // 窓
    {2108, 0x58C1}, // 壁
    {1935, 0x673A}, // 机
    {2109, 0x6905}, // 椅
    {2110, 0x5E8A}, // 床
    {2111, 0x5929}, // 天
    {2112, 0x4E95}, // 井

    // Food/meals
    {2113, 0x7C73}, // 米
    {2114, 0x8089}, // 肉
    {2115, 0x9B5A}, // 魚
    {2116, 0x91CE}, // 野
    {2117, 0x83DC}, // 菜
    {2118, 0x8336}, // 茶
    {2119, 0x9152}, // 酒
    {1936, 0x98EF}, // 飯
    {2120, 0x6599}, // 料
    {2121, 0x7406}, // 理

    // Adjectives
    {2122, 0x65B0}, // 新
    {2123, 0x53E4}, // 古
    {2124, 0x9AD8}, // 高
    {2125, 0x5B89}, // 安
    {2126, 0x591A}, // 多
    {2127, 0x5C11}, // 少
    {2128, 0x9577}, // 長
    {2129, 0x77ED}, // 短
    {2130, 0x5E83}, // 広
    {2131, 0x72ED}, // 狭
    {2132, 0x6697}, // 暗
    {2133, 0x660E}, // 明
    {2134, 0x6E29}, // 温
    {2135, 0x5BD2}, // 寒
    {2136, 0x6691}, // 暑
    {2137, 0x6DB2}, // 涼
    {2138, 0x7F8E}, // 美
    {2139, 0x60AA}, // 悪
    {2140, 0x826F}, // 良
    {2141, 0x6B63}, // 正
    {2142, 0x5F37}, // 強
    {2143, 0x5F31}, // 弱

    // Work/business
    {2148, 0x4ED5}, // 仕
    {1937, 0x4E8B}, // 事
    {2150, 0x4F1A}, // 会
    {2151, 0x793E}, // 社
    {2152, 0x54E1}, // 員
    {2153, 0x5E30}, // 度 (actually different char)
    {2154, 0x5F79}, // 役
    {2155, 0x6240}, // 所
    {2156, 0x7D66}, // 給
    {2157, 0x4F11}, // 休

    // Additional common characters from the PDF
    {638, 0x3059},   // す (hiragana)
    {674, 0x306E},   // の (hiragana)
    {675, 0x306F},   // は (hiragana)
    {783, 0x30C8},   // ト (katakana)
    {803, 0x30E9},   // ラ (katakana)
    {1952, 0x8A00},  // 言
    {2248, 0x80FD},  // 能
    {2269, 0x660E},  // 明
    {2890, 0x8A9E},  // 語
    {2956, 0x529B},  // 力
    {3592, 0x8A66},  // 試
    {3824, 0x65E5},  // 日
    {4011, 0x672C},  // 本
    {4782, 0x8A9E},  // 語

    // More JLPT common kanji
    {2158, 0x59CB}, // 始
    {2159, 0x7D42}, // 終
    {2160, 0x958B}, // 開
    {2161, 0x9589}, // 閉
    {2162, 0x6B62}, // 止
    {2163, 0x52D5}, // 動
    {2164, 0x50CD}, // 働
    {2165, 0x904A}, // 遊
    {2166, 0x6B69}, // 歩
    {2167, 0x8D70}, // 走
    {2168, 0x6CF3}, // 泳
    {2169, 0x98DB}, // 飛
    {2170, 0x4E57}, // 乗
    {2171, 0x964D}, // 降
    {2172, 0x6E21}, // 渡
    {2173, 0x901A}, // 通
    {2174, 0x9001}, // 送
    {2175, 0x8FD4}, // 返
    {2176, 0x501F}, // 借
    {2177, 0x8CB8}, // 貸
    {2178, 0x6255}, // 払

    // Numbers and counters
    {2180, 0x56DE}, // 回
    {2181, 0x756A}, // 番
    {2182, 0x7B2C}, // 第
    {2183, 0x679A}, // 枚
    {2184, 0x500B}, // 個
    {2185, 0x672C}, // 本 (counter)
    {2186, 0x53F0}, // 台
    {2187, 0x5339}, // 匹
    {2188, 0x982D}, // 頭
    {2189, 0x7F8A}, // 羊
    {2190, 0x9CE5}, // 鳥

    // Communication/expression
    {2193, 0x8A73}, // 詳
    {2194, 0x8AAC}, // 説
    {2195, 0x6848}, // 案
    {2196, 0x63D0}, // 提
    {2197, 0x793A}, // 示
    {2198, 0x4F1D}, // 伝
    {2199, 0x5831}, // 報
    {2200, 0x77E5}, // 知
  };

  auto it = kanji_map.find(cid);
  if (it != kanji_map.end()) {
    return it->second;
  }

  // Not found - return 0 to indicate no mapping. Rely on ToUnicode CMap
  // at the caller side for authoritative coverage.
  return 0;
}

// Map character code to Unicode based on font encoding
static uint32_t map_char_to_unicode(uint32_t char_code, const BaseFont* font) {
  if (!font) {
    return char_code;  // No font info, assume identity
  }

  // First check ToUnicode CMap
  // Note: map_code_to_unicode returns code as-is if no mapping found
  // So we check if it's actually in the map
  if (!font->to_unicode_cmap.code_to_unicode.empty()) {
    auto it = font->to_unicode_cmap.code_to_unicode.find(char_code);
    if (it != font->to_unicode_cmap.code_to_unicode.end()) {
      // printf("DEBUG: ToUnicode mapped %u -> U+%04X\n", char_code, it->second);
      return it->second;
    }
  }

  // For Type0 fonts with Adobe-Japan1, try CID-to-Unicode mapping
  auto* type0_font = as_type0_font(font);
  if (type0_font && type0_font->ordering == "Japan1") {
    uint32_t japan1_unicode = adobe_japan1_cid_to_unicode(char_code);
    if (japan1_unicode != 0) {
      return japan1_unicode;
    }
  }

  // Check encoding differences
  auto diff_it = font->encoding_differences.find(char_code);
  if (diff_it != font->encoding_differences.end()) {
    uint32_t mapped = glyph_name_to_unicode(diff_it->second);
    if (mapped != 0) {
      return mapped;
    }
  }

  // Use base encoding
  if (font->encoding == "WinAnsiEncoding") {
    if (char_code < 256) {
      return kWinAnsiEncoding[char_code];
    }
  } else if (font->encoding == "MacRomanEncoding") {
    if (char_code < 256) {
      return kMacRomanEncoding[char_code];
    }
  }

  // Default: assume ASCII/Latin-1 identity mapping
  return char_code;
}

// Check if font uses two-byte CID encoding. Thin wrapper over the shared
// helper so existing call sites keep the familiar name.
static bool is_two_byte_cid_font(const Type0Font* type0_font) {
  return type0_font ? type0_font->is_two_byte_cid : false;
}

// ICC Profile parsing helpers
enum class ICCColorSpaceType {
  Unknown,
  Gray,
  RGB,
  CMYK,
  Lab
};

struct ICCProfileInfo {
  uint32_t size{0};
  uint32_t version{0};
  ICCColorSpaceType color_space{ICCColorSpaceType::Unknown};
  std::string description;
  bool is_srgb{false};
  bool is_adobe_rgb{false};
};

// Parse ICC profile header to extract basic info
static ICCProfileInfo parse_icc_profile(const std::vector<uint8_t>& data) {
  ICCProfileInfo info;

  if (data.size() < 128) {
    return info;  // ICC header is at least 128 bytes
  }

  // Profile size (bytes 0-3, big-endian)
  info.size = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

  // Profile version (bytes 8-11)
  info.version = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];

  // Color space signature (bytes 16-19)
  uint32_t cs_sig = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];

  // 'RGB ' = 0x52474220, 'CMYK' = 0x434D594B, 'GRAY' = 0x47524159, 'Lab ' = 0x4C616220
  if (cs_sig == 0x52474220) {
    info.color_space = ICCColorSpaceType::RGB;
  } else if (cs_sig == 0x434D594B) {
    info.color_space = ICCColorSpaceType::CMYK;
  } else if (cs_sig == 0x47524159) {
    info.color_space = ICCColorSpaceType::Gray;
  } else if (cs_sig == 0x4C616220) {
    info.color_space = ICCColorSpaceType::Lab;
  }

  // Check for sRGB by looking at profile description tag
  // Tag table starts at byte 128, first 4 bytes = tag count
  if (data.size() > 132) {
    uint32_t tag_count = (data[128] << 24) | (data[129] << 16) | (data[130] << 8) | data[131];

    // Each tag entry is 12 bytes: signature(4) + offset(4) + size(4)
    for (uint32_t i = 0; i < tag_count && (132 + i * 12 + 11) < data.size(); i++) {
      size_t entry_offset = 132 + i * 12;
      uint32_t tag_sig = (data[entry_offset] << 24) | (data[entry_offset + 1] << 16) |
                         (data[entry_offset + 2] << 8) | data[entry_offset + 3];
      uint32_t tag_offset = (data[entry_offset + 4] << 24) | (data[entry_offset + 5] << 16) |
                            (data[entry_offset + 6] << 8) | data[entry_offset + 7];
      uint32_t tag_size = (data[entry_offset + 8] << 24) | (data[entry_offset + 9] << 16) |
                          (data[entry_offset + 10] << 8) | data[entry_offset + 11];

      // 'desc' = 0x64657363 - profile description
      if (tag_sig == 0x64657363 && tag_offset + tag_size <= data.size()) {
        // Read description (simplified - just check for keywords)
        std::string desc;
        for (size_t j = tag_offset; j < tag_offset + tag_size && j < data.size(); j++) {
          if (data[j] >= 32 && data[j] < 127) {
            desc += static_cast<char>(data[j]);
          }
        }
        info.description = desc;

        // Check for common profile names
        if (desc.find("sRGB") != std::string::npos ||
            desc.find("IEC 61966-2.1") != std::string::npos) {
          info.is_srgb = true;
        } else if (desc.find("Adobe RGB") != std::string::npos) {
          info.is_adobe_rgb = true;
        }
      }
    }
  }

  return info;
}

// Convert ICC-based pixel data to sRGB
// This is a simplified conversion that handles common cases
static void convert_icc_to_srgb(
    const std::vector<uint8_t>& src_data,
    std::vector<uint8_t>& dst_data,
    int width, int height,
    const ColorSpace& color_space) {

  ICCProfileInfo profile_info;
  if (!color_space.icc_profile_data.empty()) {
    profile_info = parse_icc_profile(color_space.icc_profile_data);
  }

  int num_components = color_space.num_components;
  if (num_components == 0) {
    // Guess from ICC profile
    switch (profile_info.color_space) {
      case ICCColorSpaceType::Gray: num_components = 1; break;
      case ICCColorSpaceType::CMYK: num_components = 4; break;
      case ICCColorSpaceType::Lab: num_components = 3; break;
      default: num_components = 3; break;
    }
  }

  dst_data.resize(width * height * 3);  // Output is always RGB

  if (num_components == 1) {
    // Grayscale - simple expansion
    for (int i = 0; i < width * height; i++) {
      uint8_t gray = (i < static_cast<int>(src_data.size())) ? src_data[i] : 0;
      dst_data[i * 3] = gray;
      dst_data[i * 3 + 1] = gray;
      dst_data[i * 3 + 2] = gray;
    }
  } else if (num_components == 3) {
    // RGB profile
    if (profile_info.is_srgb) {
      // sRGB - direct copy
      for (int i = 0; i < width * height; i++) {
        int src_idx = i * 3;
        if (src_idx + 2 < static_cast<int>(src_data.size())) {
          dst_data[i * 3] = src_data[src_idx];
          dst_data[i * 3 + 1] = src_data[src_idx + 1];
          dst_data[i * 3 + 2] = src_data[src_idx + 2];
        }
      }
    } else if (profile_info.is_adobe_rgb) {
      // Adobe RGB to sRGB approximate conversion
      // Adobe RGB has gamma 2.2 and wider gamut
      for (int i = 0; i < width * height; i++) {
        int src_idx = i * 3;
        if (src_idx + 2 < static_cast<int>(src_data.size())) {
          // Simplified conversion: apply gamut compression
          float r = src_data[src_idx] / 255.0f;
          float g = src_data[src_idx + 1] / 255.0f;
          float b = src_data[src_idx + 2] / 255.0f;

          // Adobe RGB to XYZ (D65)
          float x = 0.5767309f * r + 0.1855540f * g + 0.1881852f * b;
          float y = 0.2973769f * r + 0.6273491f * g + 0.0752741f * b;
          float z = 0.0270343f * r + 0.0706872f * g + 0.9911085f * b;

          // XYZ to sRGB
          float sr = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
          float sg = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
          float sb = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;

          // Clamp and convert to 8-bit
          dst_data[i * 3] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sr)) * 255);
          dst_data[i * 3 + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sg)) * 255);
          dst_data[i * 3 + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, sb)) * 255);
        }
      }
    } else {
      // Unknown RGB profile - assume sRGB-like, direct copy
      for (int i = 0; i < width * height; i++) {
        int src_idx = i * 3;
        if (src_idx + 2 < static_cast<int>(src_data.size())) {
          dst_data[i * 3] = src_data[src_idx];
          dst_data[i * 3 + 1] = src_data[src_idx + 1];
          dst_data[i * 3 + 2] = src_data[src_idx + 2];
        }
      }
    }
  } else if (num_components == 4) {
    // CMYK profile - convert to RGB
    for (int i = 0; i < width * height; i++) {
      int src_idx = i * 4;
      if (src_idx + 3 < static_cast<int>(src_data.size())) {
        float c = src_data[src_idx] / 255.0f;
        float m = src_data[src_idx + 1] / 255.0f;
        float y = src_data[src_idx + 2] / 255.0f;
        float k = src_data[src_idx + 3] / 255.0f;

        // CMYK to RGB conversion
        dst_data[i * 3] = static_cast<uint8_t>(255 * (1.0f - c) * (1.0f - k));
        dst_data[i * 3 + 1] = static_cast<uint8_t>(255 * (1.0f - m) * (1.0f - k));
        dst_data[i * 3 + 2] = static_cast<uint8_t>(255 * (1.0f - y) * (1.0f - k));
      }
    }
  }
}

Blend2DBackend::Blend2DBackend() {}

Blend2DBackend::~Blend2DBackend() {}

void Blend2DBackend::set_current_font(const std::string& font_name, const BaseFont* font) {
    current_font_name_ = font_name;
    current_font_ = font;
    load_font(*current_pdf_, font_name, current_font_);
}

bool Blend2DBackend::initialize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;

  // Create image with PRGB32 format (premultiplied RGBA)
  BLResult result = image_.create(static_cast<int>(width), static_cast<int>(height), BL_FORMAT_PRGB32);
  if (result != BL_SUCCESS) {
    return false;
  }

  // Attach context to image
  result = ctx_.begin(image_);
  if (result != BL_SUCCESS) {
    return false;
  }

  // Use bilinear interpolation for image scaling (default is nearest-neighbor)
  ctx_.set_pattern_quality(BL_PATTERN_QUALITY_BILINEAR);

  // Clear to white
  ctx_.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx_.set_fill_style(BLRgba32(0xFFFFFFFF));
  ctx_.fill_all();
  ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);

  initialized_ = true;
  return true;
}

bool Blend2DBackend::begin_scene() {
  if (!initialized_) return false;

  // Clear to white
  ctx_.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx_.set_fill_style(BLRgba32(0xFFFFFFFF));
  ctx_.fill_all();
  ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);

  return true;
}

bool Blend2DBackend::end_scene() {
  if (!initialized_) return false;
  ctx_.flush(BL_CONTEXT_FLUSH_SYNC);
  return true;
}

bool Blend2DBackend::draw_rectangle(float x, float y, float w, float h,
                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  ctx_.set_fill_style(BLRgba32(r, g, b, a));
  ctx_.fill_rect(x, y, w, h);
  return true;
}

bool Blend2DBackend::draw_circle(float cx, float cy, float radius,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  ctx_.set_fill_style(BLRgba32(r, g, b, a));
  ctx_.fill_circle(cx, cy, radius);
  return true;
}

bool Blend2DBackend::draw_path(const BLPath& path,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  ctx_.set_fill_style(BLRgba32(r, g, b, a));
  ctx_.fill_path(path);
  return true;
}

bool Blend2DBackend::draw_text(float x, float y, const std::string& text, float size,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  // Draw each character as a glyph
  float cursor_x = x;
  for (char c : text) {
    draw_glyph(static_cast<int>(c), cursor_x, y, size, r, g, b, a);
    // Approximate character width
    cursor_x += size * 0.6f;
  }
  return true;
}

bool Blend2DBackend::draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  ctx_.set_stroke_style(BLRgba32(r, g, b, a));
  ctx_.set_stroke_width(stroke_width);
  ctx_.stroke_line(x1, y1, x2, y2);
  return true;
}

Blend2DRenderResult Blend2DBackend::get_buffer() {
  Blend2DRenderResult result;

  if (!initialized_) {
    result.error = "Backend not initialized";
    return result;
  }

  ctx_.flush(BL_CONTEXT_FLUSH_SYNC);

  BLImageData data;
  if (image_.get_data(&data) != BL_SUCCESS) {
    result.error = "Failed to get image data";
    return result;
  }

  result.width = width_;
  result.height = height_;

  // Convert from PRGB32 to RGBA8888
  result.pixels.resize(width_ * height_ * 4);
  const uint8_t* src = static_cast<const uint8_t*>(data.pixel_data);

  for (uint32_t y = 0; y < height_; y++) {
    for (uint32_t x = 0; x < width_; x++) {
      size_t src_idx = y * data.stride + x * 4;
      size_t dst_idx = (y * width_ + x) * 4;

      // PRGB32 is BGRA in memory (little-endian)
      uint8_t b_val = src[src_idx + 0];
      uint8_t g_val = src[src_idx + 1];
      uint8_t r_val = src[src_idx + 2];
      uint8_t a_val = src[src_idx + 3];

      // Convert from premultiplied to straight alpha
      if (a_val > 0) {
        r_val = static_cast<uint8_t>(std::min(255, (r_val * 255) / a_val));
        g_val = static_cast<uint8_t>(std::min(255, (g_val * 255) / a_val));
        b_val = static_cast<uint8_t>(std::min(255, (b_val * 255) / a_val));
      }

      result.pixels[dst_idx + 0] = r_val;
      result.pixels[dst_idx + 1] = g_val;
      result.pixels[dst_idx + 2] = b_val;
      result.pixels[dst_idx + 3] = a_val;
    }
  }

  result.success = true;
  return result;
}

Blend2DBackend::CacheStats Blend2DBackend::get_cache_stats() const {
  CacheStats stats;
  auto rc = RenderCache::instance().stats();
  stats.hits = rc.hits;
  stats.misses = rc.misses;
  stats.evictions = rc.evictions;
  stats.entries = rc.entries;
  stats.bytes_used = rc.bytes_used;
  stats.max_size = RenderCache::instance().max_size();
  return stats;
}

bool Blend2DBackend::save_to_png(const std::string& filename) {
  RenderOptions options;
  options.format = RenderOptions::Format::PNG;
  return save_to_file(filename, options);
}

bool Blend2DBackend::save_to_file(const std::string& filename, const RenderOptions& options) {
  auto result = get_buffer();
  if (!result.success) {
    return false;
  }

  int ret = 0;
  int width = static_cast<int>(result.width);
  int height = static_cast<int>(result.height);
  int stride = width * 4;

  switch (options.format) {
    case RenderOptions::Format::PNG: {
      // Set PNG compression level (stb_image_write uses this global)
      stbi_write_png_compression_level = options.png_compression;
      ret = stbi_write_png(filename.c_str(), width, height, 4,
                           result.pixels.data(), stride);
      break;
    }
    case RenderOptions::Format::JPEG: {
      // JPEG doesn't support alpha, convert RGBA to RGB
      std::vector<uint8_t> rgb_pixels(width * height * 3);
      for (int i = 0; i < width * height; i++) {
        rgb_pixels[i * 3 + 0] = result.pixels[i * 4 + 0];
        rgb_pixels[i * 3 + 1] = result.pixels[i * 4 + 1];
        rgb_pixels[i * 3 + 2] = result.pixels[i * 4 + 2];
      }
      ret = stbi_write_jpg(filename.c_str(), width, height, 3,
                           rgb_pixels.data(), options.jpeg_quality);
      break;
    }
    case RenderOptions::Format::BMP:
      ret = stbi_write_bmp(filename.c_str(), width, height, 4,
                           result.pixels.data());
      break;
    case RenderOptions::Format::TGA:
      ret = stbi_write_tga(filename.c_str(), width, height, 4,
                           result.pixels.data());
      break;
  }

  return ret != 0;
}

size_t Blend2DBackend::count_render_objects(
    const std::vector<uint8_t>& content_data) {
  return count_render_objects_impl(
      std::string_view(reinterpret_cast<const char*>(content_data.data()),
                       content_data.size()));
}

void Blend2DBackend::begin_progress(const RenderProgressCallback& callback,
                                    size_t total_objects,
                                    size_t object_threshold,
                                    uint32_t percent_step) {
  progress_ = RenderProgressState();
  if (!callback || total_objects == 0 || total_objects < object_threshold) {
    return;
  }

  progress_.callback = callback;
  progress_.total_objects = total_objects;
  progress_.percent_step = std::max<uint32_t>(1, percent_step);
  progress_.enabled = true;
  progress_.callback(RenderProgressInfo{0, progress_.total_objects, 0});
}

void Blend2DBackend::advance_progress(size_t processed_objects) {
  if (!progress_.enabled || processed_objects == 0) {
    return;
  }

  progress_.processed_objects =
      std::min(progress_.processed_objects + processed_objects,
               progress_.total_objects);

  const auto current_percent = static_cast<uint32_t>(std::min<long double>(
      100.0L, (static_cast<long double>(progress_.processed_objects) * 100.0L) /
                  static_cast<long double>(progress_.total_objects)));

  for (uint32_t percent = progress_.last_percent + progress_.percent_step;
       percent <= current_percent && percent <= 100;
       percent += progress_.percent_step) {
    progress_.callback(
        RenderProgressInfo{progress_.processed_objects, progress_.total_objects,
                           percent});
    progress_.last_percent = percent;
  }
}

void Blend2DBackend::finish_progress() {
  if (!progress_.enabled) {
    progress_ = RenderProgressState();
    return;
  }

  progress_.processed_objects = progress_.total_objects;
  if (progress_.last_percent < 100) {
    progress_.callback(RenderProgressInfo{progress_.processed_objects,
                                          progress_.total_objects, 100});
  }
  progress_ = RenderProgressState();
}

void Blend2DBackend::set_progress_callback(RenderProgressCallback callback,
                                          size_t object_threshold,
                                          uint32_t percent_step) {
  progress_config_.callback = std::move(callback);
  progress_config_.object_threshold = object_threshold;
  progress_config_.percent_step = std::max<uint32_t>(1, percent_step);
}

void Blend2DBackend::clear_progress_callback() {
  progress_config_ = RenderProgressConfig();
}

Blend2DRenderResult Blend2DBackend::render_page(const Pdf& pdf, const Page& page, 
                                                 const RenderOptions& options) {
  Blend2DRenderResult result;

  // Get page dimensions
  float page_width = 612.0f;
  float page_height = 792.0f;

  if (page.media_box.size() >= 4) {
    page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
    page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
  }

  // Calculate scale based on DPI (72 DPI is standard PDF resolution)
  float dpi_scale = options.dpi / 72.0f;

  // Resize canvas if needed for DPI scaling
  uint32_t target_width = static_cast<uint32_t>(page_width * dpi_scale);
  uint32_t target_height = static_cast<uint32_t>(page_height * dpi_scale);

  if (target_width != width_ || target_height != height_) {
    if (!initialize(target_width, target_height)) {
      result.error = "Failed to resize canvas for DPI scaling";
      return result;
    }
  }

  // Calculate scale to fit in canvas
  float scale_x = static_cast<float>(width_) / page_width;
  float scale_y = static_cast<float>(height_) / page_height;
  float scale = std::min(scale_x, scale_y);

  // Initialize graphics state
  state_ = GraphicsState();
  state_.page_width = page_width;
  state_.page_height = page_height;
  state_.scale = scale;

  current_pdf_ = &pdf;
  current_page_ = &page;

  std::vector<std::vector<uint8_t>> decoded_contents;
  decoded_contents.reserve(page.contents.size());
  size_t total_render_objects = 0;

  for (const auto& content_obj : page.contents) {
    Value resolved_obj = content_obj;
    uint32_t obj_num = 0;
    uint16_t gen_num = 0;

    if (content_obj.type == Value::REFERENCE) {
      obj_num = content_obj.ref_object_number;
      gen_num = content_obj.ref_generation_number;
      auto resolved = resolve_reference(pdf, obj_num, gen_num);
      if (!resolved.success) {
        continue;
      }
      resolved_obj = std::move(resolved.value);
    }

    if (resolved_obj.type != Value::STREAM) {
      continue;
    }

    auto decoded_result = decode_stream(pdf, resolved_obj, obj_num, gen_num);
    if (!decoded_result.success) {
      continue;
    }

    total_render_objects += count_render_objects(decoded_result.data);
    total_render_objects += scan_do_extra_weight(
        std::string_view(
            reinterpret_cast<const char*>(decoded_result.data.data()),
            decoded_result.data.size()),
        pdf, page.resources);
    decoded_contents.push_back(std::move(decoded_result.data));
  }

  const bool has_explicit_progress = static_cast<bool>(options.progress_callback);
  begin_progress(has_explicit_progress ? options.progress_callback
                                       : progress_config_.callback,
                 total_render_objects,
                 has_explicit_progress ? options.progress_object_threshold
                                       : progress_config_.object_threshold,
                 has_explicit_progress ? options.progress_percent_step
                                       : progress_config_.percent_step);

  // Clear canvas with background color
  ctx_.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx_.set_fill_style(BLRgba32(options.bg_r, options.bg_g, options.bg_b, options.bg_a));
  ctx_.fill_all();
  ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);

  // Process each decoded content stream
  for (const auto& decoded_content : decoded_contents) {
    state_ = GraphicsState();
    state_.page_width = page_width;
    state_.page_height = page_height;
    state_.scale = scale;
    parse_pdf_content(decoded_content);
  }

  ctx_.flush(BL_CONTEXT_FLUSH_SYNC);
  finish_progress();

  current_pdf_ = nullptr;
  current_page_ = nullptr;

  return get_buffer();
}

// Helper to determine font category from name
// Returns: 0=sans-serif, 1=monospace, 2=serif, 3=symbol, 4=CJK
static int get_font_category(const std::string& font_name) {
  // Convert to lowercase for comparison
  std::string lower_name;
  for (char c : font_name) {
    lower_name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  // Check for CJK fonts (Japanese, Chinese, Korean)
  if (lower_name.find("japan") != std::string::npos ||
      lower_name.find("mincho") != std::string::npos ||
      lower_name.find("gothic") != std::string::npos ||
      lower_name.find("meiryo") != std::string::npos ||
      lower_name.find("hiragino") != std::string::npos ||
      lower_name.find("msgothic") != std::string::npos ||
      lower_name.find("msmincho") != std::string::npos ||
      lower_name.find("futo") != std::string::npos ||
      lower_name.find("china") != std::string::npos ||
      lower_name.find("korea") != std::string::npos ||
      lower_name.find("simsun") != std::string::npos ||
      lower_name.find("simhei") != std::string::npos ||
      lower_name.find("heiti") != std::string::npos ||
      lower_name.find("songti") != std::string::npos ||
      lower_name.find("noto") != std::string::npos) {
    return 4;  // CJK
  }

  // Check for monospace/typewriter fonts
  if (lower_name.find("courier") != std::string::npos ||
      lower_name.find("mono") != std::string::npos ||
      lower_name.find("typewriter") != std::string::npos ||
      lower_name.find("consol") != std::string::npos ||
      lower_name.find("fixed") != std::string::npos) {
    return 1;  // Monospace
  }

  // Check for serif fonts
  if (lower_name.find("times") != std::string::npos ||
      lower_name.find("serif") != std::string::npos ||
      lower_name.find("roman") != std::string::npos ||
      lower_name.find("garamond") != std::string::npos ||
      lower_name.find("palatino") != std::string::npos ||
      lower_name.find("georgia") != std::string::npos ||
      lower_name.find("cambria") != std::string::npos) {
    return 2;  // Serif
  }

  // Check for symbol/dingbat fonts
  if (lower_name.find("symbol") != std::string::npos ||
      lower_name.find("dingbat") != std::string::npos ||
      lower_name.find("wingding") != std::string::npos ||
      lower_name.find("zapf") != std::string::npos) {
    return 3;  // Symbol
  }

  // Default to sans-serif (most common)
  return 0;  // Sans-serif
}

// Check if font is CJK based on Type0Font ordering
static bool is_cjk_font(const BaseFont* font) {
  if (!font) return false;
  auto* type0_font = as_type0_font(font);
  if (type0_font) {
    // Check ordering for CJK collections
    const std::string& ordering = type0_font->ordering;
    if (ordering == "Japan1" || ordering == "CNS1" || ordering == "GB1" ||
        ordering == "Korea1" || ordering == "KR") {
      return true;
    }
    // Also check CMap name
    const std::string& cmap_name = type0_font->encoding_cmap.name;
    if (cmap_name.find("Japan") != std::string::npos ||
        cmap_name.find("CNS") != std::string::npos ||
        cmap_name.find("GB") != std::string::npos ||
        cmap_name.find("Korea") != std::string::npos ||
        cmap_name.find("UniJIS") != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool Blend2DBackend::load_fallback_font(const std::string& font_name) {
  return load_fallback_font_with_hint(font_name, nullptr);
}

bool Blend2DBackend::load_fallback_font_with_hint(const std::string& font_name, const BaseFont* font) {
  // Determine font category for better substitution
  int category = get_font_category(font_name);

  // Override category to CJK if font structure indicates CJK
  if (is_cjk_font(font)) {
    category = 4;  // CJK
  }

  // Font paths organized by category
  // Category 0: Sans-serif, 1: Monospace, 2: Serif, 3: Symbol, 4: CJK
  std::vector<std::vector<std::string>> font_paths_by_category = {
    // Sans-serif fonts
    {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
      "/System/Library/Fonts/SFNSText.ttf",
      "/Library/Fonts/Arial.ttf",
      "C:\\Windows\\Fonts\\arial.ttf",
      "C:\\Windows\\Fonts\\segoeui.ttf"
    },
    // Monospace fonts
    {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
      "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
      "/System/Library/Fonts/Courier.ttc",
      "/System/Library/Fonts/Monaco.ttf",
      "/Library/Fonts/Courier New.ttf",
      "C:\\Windows\\Fonts\\cour.ttf",
      "C:\\Windows\\Fonts\\consola.ttf"
    },
    // Serif fonts
    {
      "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
      "/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
      "/System/Library/Fonts/Times.ttc",
      "/Library/Fonts/Times New Roman.ttf",
      "C:\\Windows\\Fonts\\times.ttf",
      "C:\\Windows\\Fonts\\georgia.ttf"
    },
    // Symbol fonts (fallback to sans if not found)
    {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/System/Library/Fonts/Symbol.ttf",
      "C:\\Windows\\Fonts\\symbol.ttf"
    },
    // CJK sans (Gothic) fonts - default
    {
      // Bundled Noto Sans JP (relative to project root)
      "fonts/noto-sans-jp/NotoSansJP-Regular.otf",
      "../fonts/noto-sans-jp/NotoSansJP-Regular.otf",
      // System-installed Noto
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/opentype/noto/NotoSansJP-Regular.otf",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/noto/NotoSansJP-Regular.ttf",
      // Other Japanese Gothic fonts
      "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
      "/usr/share/fonts/truetype/takao-gothic/TakaoGothic.ttf",
      "/usr/share/fonts/opentype/ipafont-gothic/ipagp.ttf",
      "/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf",
      "/usr/share/fonts/truetype/sazanami/sazanami-gothic.ttf",
      // Chinese
      "/usr/share/fonts/opentype/noto/NotoSansSC-Regular.otf",
      "/usr/share/fonts/truetype/arphic/uming.ttc",
      "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
      // Korean
      "/usr/share/fonts/opentype/noto/NotoSansKR-Regular.otf",
      "/usr/share/fonts/truetype/nanum/NanumGothic.ttf",
      // macOS
      "/System/Library/Fonts/Hiragino Sans GB.ttc",
      "/System/Library/Fonts/\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf W3.ttc",
      "/Library/Fonts/Osaka.ttf",
      // Windows
      "C:\\Windows\\Fonts\\msgothic.ttc",
      "C:\\Windows\\Fonts\\meiryo.ttc",
      "C:\\Windows\\Fonts\\YuGothR.ttc",
      "C:\\Windows\\Fonts\\simsun.ttc",
      "C:\\Windows\\Fonts\\malgun.ttf"
    }
  };

  // CJK serif (Mincho) font paths for serif CJK selection
  static const std::vector<std::string> cjk_serif_paths = {
    // Bundled Noto Serif JP (relative to project root)
    "fonts/noto-serif-jp/NotoSerifJP-Regular.otf",
    "../fonts/noto-serif-jp/NotoSerifJP-Regular.otf",
    // System-installed Noto Serif
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifJP-Regular.otf",
    "/usr/share/fonts/truetype/noto/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSerifJP-Regular.ttf",
    // Japanese Mincho fonts
    "/usr/share/fonts/opentype/ipafont-mincho/ipamp.ttf",
    "/usr/share/fonts/truetype/sazanami/sazanami-mincho.ttf",
    // Fallback to sans CJK if no serif available
    "fonts/noto-sans-jp/NotoSansJP-Regular.otf",
    "../fonts/noto-sans-jp/NotoSansJP-Regular.otf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    // macOS
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/Library/Fonts/Osaka.ttf",
    // Windows
    "C:\\Windows\\Fonts\\msmincho.ttc",
    "C:\\Windows\\Fonts\\YuMincho.ttc",
    "C:\\Windows\\Fonts\\simsun.ttc"
  };

  // For CJK, select serif (Mincho) or sans (Gothic) based on font name
  const std::vector<std::string>* preferred_ptr = &font_paths_by_category[category];
  if (category == 4) {
    std::string lower;
    for (char c : font_name)
      lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("mincho") != std::string::npos ||
        lower.find("ming") != std::string::npos ||
        lower.find("serif") != std::string::npos ||
        lower.find("songti") != std::string::npos) {
      preferred_ptr = &cjk_serif_paths;
    }
  }

  // Check font provider first (for runtime-registered fonts, all categories)
  {
    auto& provider = FontProvider::instance();
    FontCategory pcat;
    if (category == 4) {
      pcat = (preferred_ptr == &cjk_serif_paths)
          ? FontCategory::kCJKSerif : FontCategory::kCJKSans;
    } else {
      switch (category) {
        case 0: pcat = FontCategory::kSans; break;
        case 1: pcat = FontCategory::kMono; break;
        case 2: pcat = FontCategory::kSerif; break;
        case 3: pcat = FontCategory::kSymbol; break;
        default: pcat = FontCategory::kSans; break;
      }
    }
    const ProvidedFont* pf = provider.find_by_category(pcat);
    if (!pf && category == 4) pf = provider.find_by_category(FontCategory::kCJKSans);
    if (pf && !pf->data.empty()) {
      FontCache& cache = font_cache_[font_name];
      cache.font_data = pf->data;
      int off = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
      if (off < 0) off = 0;
      if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), off)) {
        cache.initialized = true;
        return true;
      }
    }
  }

#ifdef NANOPDF_EMBED_CJK_FONTS
  // Check embedded CJK fonts
  if (category == 4) {
    const char* target = (preferred_ptr == &cjk_serif_paths)
        ? "NotoSerifJP-Regular" : "NotoSansJP-Regular";
    auto* entry = embedded_cjk_fonts::find_font(target);
    if (entry) {
      FontCache& cache = font_cache_[font_name];
      if (embedded_cjk_fonts::decompress_font(entry, cache.font_data)) {
        int off = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
        if (off < 0) off = 0;
        if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), off)) {
          cache.initialized = true;
          return true;
        }
      }
    }
  }
#endif

  // Try fonts in the matching category first
  const auto& preferred_paths = *preferred_ptr;
  for (const auto& path : preferred_paths) {
    std::ifstream file(path, std::ios::binary);
    if (file) {
      file.seekg(0, std::ios::end);
      size_t size = file.tellg();
      file.seekg(0, std::ios::beg);

      FontCache& cache = font_cache_[font_name];
      cache.font_data.resize(size);
      file.read(reinterpret_cast<char*>(cache.font_data.data()), size);

      // Handle TTC (TrueType Collection) files
      int font_offset = 0;
      if (cache.font_data.size() > 4) {
        font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
        if (font_offset < 0) font_offset = 0;
      }

      if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
        cache.initialized = true;
        // printf("[Blend2D] Using fallback font: %s for '%s' (cat=%d)\n", path.c_str(), font_name.c_str(), category);
        return true;
      }
    }
  }

  // If category-specific fonts not found, try sans-serif as ultimate fallback
  if (category != 0) {
    for (const auto& path : font_paths_by_category[0]) {
      std::ifstream file(path, std::ios::binary);
      if (file) {
        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);

        FontCache& cache = font_cache_[font_name];
        cache.font_data.resize(size);
        file.read(reinterpret_cast<char*>(cache.font_data.data()), size);

        if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), 0)) {
          cache.initialized = true;
          // printf("[Blend2D] Using fallback font: %s for '%s' (fallback cat=%d)\n", path.c_str(), font_name.c_str(), category);
          return true;
        }
      }
    }
  }

  return false;
}

bool Blend2DBackend::load_font(const Pdf& pdf, const std::string& font_name, const BaseFont* font) {
  // Check if already loaded
  auto it = font_cache_.find(font_name);
  if (it != font_cache_.end() && it->second.initialized) {
    return true;
  }

  // Shared font cache: avoid re-loading and re-decoding font data when the
  // same Pdf font is loaded by a different backend or a second page pass.
  {
    SharedFontEntry shared;
    if (SharedFontCache::instance().find(&pdf, font_name, shared)) {
      FontCache& cache = font_cache_[font_name];
      cache.font_data = std::move(shared.font_data);
      int font_offset = 0;
      if (cache.font_data.size() > 4) {
        font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
        if (font_offset < 0) font_offset = 0;
      }
      if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
        cache.initialized = true;
        return true;
      }
    }
  }

  // For Type0 fonts, check descendant font's descriptor for embedded font
  const FontDescriptor* desc = nullptr;
  auto* type0_font = as_type0_font(font);
  if (font) {
    if (type0_font && type0_font->descendant_font) {
      BaseFont* descendant = type0_font->descendant_font.get();
      if (descendant && descendant->descriptor) {
        desc = descendant->descriptor;
      }
    }
    // If no descendant descriptor, try the main font's descriptor
    if (!desc && font->descriptor) {
      desc = font->descriptor;
    }
  }

  // Try to load embedded font
  if (desc) {
    // Check if font file is available
    if (desc->font_file.type == Value::UNDEFINED ||
        desc->font_file.type == Value::NULL_OBJ) {
      // No embedded font, try fallback
      return load_fallback_font_with_hint(font_name, font);
    }

    // Resolve and decode font file stream
    Value font_file_val = desc->font_file;
    if (font_file_val.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, font_file_val.ref_object_number,
                                        font_file_val.ref_generation_number);
      if (!resolved.success) {
        return load_fallback_font_with_hint(font_name, font);
      }
      font_file_val = resolved.value;
    }

    if (font_file_val.type == Value::STREAM) {
      auto decoded = decode_stream(pdf, font_file_val);
      if (decoded.success && !decoded.data.empty()) {
        // Check for raw CFF data and wrap it in an OpenType container.
        // OpenType-CFF (OTTO header) is supported natively by stb_truetype.
        if (cff_wrapper::is_raw_cff(decoded.data.data(), decoded.data.size())) {
          auto wrapped = cff_wrapper::wrap_cff_in_opentype(decoded.data);
          if (wrapped.empty()) {
            return load_fallback_font_with_hint(font_name, font);
          }
          decoded.data = std::move(wrapped);
        }

        FontCache& cache = font_cache_[font_name];
        cache.font_data = decoded.data;
        // Get font offset for font collection (TTC) files
        int font_offset = 0;
        if (cache.font_data.size() > 4) {
          font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
          if (font_offset < 0) font_offset = 0;  // Invalid offset, try from start
        }
        if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
          cache.initialized = true;

          // Share with other backends / page passes.
          SharedFontCache::instance().store(&pdf, font_name, {
            cache.font_data,  // copy
            true,             // is_embedded (Blend2D doesn't track this, but LightVG/ThorVG do)
            false,            // has_ttf_parse
            {}                // cid_to_gid (Blend2D doesn't use this)
          });

          return true;
        }
      }
    }
  }

  // Load fallback font
  return load_fallback_font_with_hint(font_name, font);
}

Blend2DBackend::FontCache* Blend2DBackend::get_font(const std::string& font_name) {
  auto it = font_cache_.find(font_name);
  if (it != font_cache_.end() && it->second.initialized) {
    return &it->second;
  }
  return nullptr;
}

bool Blend2DBackend::draw_missing_glyph_placeholder(float x, float y, float size,
                                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  float glyph_width = size * 0.5f;
  float box_margin = size * 0.05f;
  float stroke_width = size * 0.04f;

  ctx_.set_stroke_style(BLRgba32(r, g, b, static_cast<uint8_t>(a * 0.4f)));
  ctx_.set_stroke_width(stroke_width);
  ctx_.stroke_rect(x + box_margin, y - size + box_margin,
                   glyph_width - 2 * box_margin, size - 2 * box_margin);
  return true;
}

bool Blend2DBackend::draw_glyph(int codepoint, float x, float y, float size,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  FontCache* font = get_font(current_font_name_);
  if (!font) {
    // Fallback to tofu box placeholder
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  stbtt_vertex* vertices = nullptr;
  int num_verts = stbtt_GetCodepointShape(&font->font_info, codepoint, &vertices);

  if (num_verts == 0 || !vertices) {
    // Glyph not found in font - draw tofu box placeholder
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, size);

  BLPath path;
  float curr_x = x, curr_y = y;

  for (int i = 0; i < num_verts; i++) {
    stbtt_vertex* v = &vertices[i];
    float vx = x + v->x * scale;
    float vy = y - v->y * scale;

    switch (v->type) {
      case STBTT_vmove:
        path.move_to(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vline:
        path.line_to(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vcurve: {
        float cx = x + v->cx * scale;
        float cy = y - v->cy * scale;
        path.quad_to(cx, cy, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
      case STBTT_vcubic: {
        float cx1 = x + v->cx * scale;
        float cy1 = y - v->cy * scale;
        float cx2 = x + v->cx1 * scale;
        float cy2 = y - v->cy1 * scale;
        path.cubic_to(cx1, cy1, cx2, cy2, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
    }
  }

  path.close();

  // Apply text rendering mode
  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 || render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 || render_mode == 5 || render_mode == 6);
  bool add_to_clip = (render_mode >= 4 && render_mode <= 7);
  bool invisible = (render_mode == 3 || render_mode == 7);

  stbtt_FreeShape(&font->font_info, vertices);

  // Accumulate glyph path into text clip path for modes 4-7
  if (add_to_clip) {
    state_.text_clip_active = true;
    state_.text_clip_path.add_path(path);
  }

  if (invisible) {
    return true;
  }

  if (do_fill) {
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_path(path);
  }

  if (do_stroke) {
    float stroke_width = state_.stroke_width * state_.scale;
    if (stroke_width < 0.5f) stroke_width = 0.5f;
    ctx_.set_stroke_width(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    ctx_.set_stroke_style(BLRgba32(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha));
    ctx_.stroke_path(path);
  }

  return true;
}

bool Blend2DBackend::draw_glyph_by_index(int glyph_index, float x, float y, float size,
                                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  FontCache* font = get_font(current_font_name_);
  if (!font) {
    // Fallback to tofu box placeholder
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  stbtt_vertex* vertices = nullptr;
  int num_verts = stbtt_GetGlyphShape(&font->font_info, glyph_index, &vertices);

  if (num_verts == 0 || !vertices) {
    // Glyph not found - draw tofu box placeholder
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, size);

  BLPath path;
  float curr_x = x, curr_y = y;

  for (int i = 0; i < num_verts; i++) {
    stbtt_vertex* v = &vertices[i];
    float vx = x + v->x * scale;
    float vy = y - v->y * scale;

    switch (v->type) {
      case STBTT_vmove:
        path.move_to(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vline:
        path.line_to(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vcurve: {
        float cx = x + v->cx * scale;
        float cy = y - v->cy * scale;
        path.quad_to(cx, cy, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
      case STBTT_vcubic: {
        float cx1 = x + v->cx * scale;
        float cy1 = y - v->cy * scale;
        float cx2 = x + v->cx1 * scale;
        float cy2 = y - v->cy1 * scale;
        path.cubic_to(cx1, cy1, cx2, cy2, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
    }
  }

  path.close();

  // Apply text rendering mode
  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 || render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 || render_mode == 5 || render_mode == 6);
  bool add_to_clip = (render_mode >= 4 && render_mode <= 7);
  bool invisible = (render_mode == 3 || render_mode == 7);

  stbtt_FreeShape(&font->font_info, vertices);

  // Accumulate glyph path into text clip path for modes 4-7
  if (add_to_clip) {
    state_.text_clip_active = true;
    state_.text_clip_path.add_path(path);
  }

  // Restore clipping to full canvas before drawing text
  ctx_.restore_clipping();

  if (invisible) {
    return true;
  }

  if (do_fill) {
    ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_path(path);
  }

  if (do_stroke) {
    float stroke_width = state_.stroke_width * state_.scale;
    if (stroke_width < 0.5f) stroke_width = 0.5f;
    ctx_.set_stroke_width(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    ctx_.set_stroke_style(BLRgba32(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha));
    ctx_.stroke_path(path);
  }

  return true;
}

bool Blend2DBackend::render_type3_glyph(const Type3Font* type3_font, const std::string& glyph_name,
                                         float x, float y, float size,
                                         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!type3_font || !current_pdf_ || glyph_name.empty()) {
    return false;
  }

  // Look up the glyph procedure in CharProcs
  auto proc_it = type3_font->char_procs.find(glyph_name);
  if (proc_it == type3_font->char_procs.end()) {
    // Glyph not found - draw tofu box placeholder
    draw_missing_glyph_placeholder(x, y, size, r, g, b, a);
    return true;
  }

  // Resolve the glyph procedure (it's usually a stream)
  Value proc_value = proc_it->second;
  if (proc_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_, proc_value.ref_object_number,
                                       proc_value.ref_generation_number);
    if (!resolved.success) {
      return false;
    }
    proc_value = resolved.value;
  }

  if (proc_value.type != Value::STREAM) {
    return false;
  }

  // Decode the glyph content stream
  auto decoded = decode_stream(*current_pdf_, proc_value);
  if (!decoded.success || decoded.data.empty()) {
    return false;
  }

  // Calculate transformation based on font matrix and size
  double fm_a = type3_font->font_matrix.size() >= 1 ? type3_font->font_matrix[0] : 0.001;
  double fm_d = type3_font->font_matrix.size() >= 4 ? type3_font->font_matrix[3] : 0.001;

  // Save current state
  GraphicsState saved_state = state_;

  // Set up graphics state for glyph rendering
  float glyph_scale = size * static_cast<float>(fm_a) * state_.scale;

  // Offset to glyph position
  state_.transform.e = x;
  state_.transform.f = y;

  // Apply font matrix scaling with Y-flip for glyph space
  state_.transform.a = glyph_scale;
  state_.transform.d = -glyph_scale;
  state_.transform.b = 0.0f;
  state_.transform.c = 0.0f;

  // Set fill color
  state_.fill_r = r;
  state_.fill_g = g;
  state_.fill_b = b;
  state_.fill_a = a;

  // Push Type 3 font resources
  if (!type3_font->resources.empty()) {
    form_resources_stack_.push_back(type3_font->resources);
  }

  // Parse and render the glyph content stream
  bool progress_enabled = progress_.enabled;
  progress_.enabled = false;
  parse_pdf_content(decoded.data);
  progress_.enabled = progress_enabled;

  // Pop resources
  if (!type3_font->resources.empty() && !form_resources_stack_.empty()) {
    form_resources_stack_.pop_back();
  }

  // Restore state
  state_ = saved_state;

  return true;
}

float Blend2DBackend::calculate_text_width(const std::string& text, float font_size) {
  // First try to use PDF font width information for Type0/CID fonts
  auto* type0_font = as_type0_font(current_font_);
  if (type0_font) {
    bool is_two_byte_cid = is_two_byte_cid_font(type0_font);

    float width = 0.0f;
    for (size_t i = 0; i < text.length(); ) {
      uint32_t char_code;
      size_t bytes_consumed = 1;

      if (is_two_byte_cid && i + 1 < text.length()) {
        // Two-byte CID encoding
        uint8_t high_byte = static_cast<unsigned char>(text[i]);
        uint8_t low_byte = static_cast<unsigned char>(text[i + 1]);
        char_code = (static_cast<uint32_t>(high_byte) << 8) | low_byte;
        bytes_consumed = 2;
      } else {
        char_code = static_cast<unsigned char>(text[i]);
      }

      // Look up width in PDF font's width table
      auto width_it = type0_font->cid_widths.find(char_code);
      if (width_it != type0_font->cid_widths.end()) {
        // PDF widths are in 1/1000 of text space unit
        width += width_it->second / 1000.0f * font_size;
      } else {
        // Use default width
        width += type0_font->default_width / 1000.0f * font_size;
      }
      i += bytes_consumed;
    }
    return width;
  }

  // Check if BaseFont has width information
  if (current_font_ && !current_font_->widths.empty()) {
    float width = 0.0f;
    for (size_t i = 0; i < text.length(); i++) {
      uint32_t char_code = static_cast<unsigned char>(text[i]);
      int first_char = current_font_->first_char;
      int last_char = current_font_->last_char;

      if (static_cast<int>(char_code) >= first_char &&
          static_cast<int>(char_code) <= last_char) {
        size_t idx = char_code - first_char;
        if (idx < current_font_->widths.size()) {
          // PDF widths are in 1/1000 of text space unit
          width += current_font_->widths[idx] / 1000.0f * font_size;
          continue;
        }
      }
      // Default width for missing glyphs
      width += font_size * 0.5f;
    }
    return width;
  }

  // Fallback to stb_truetype metrics
  FontCache* font = get_font(current_font_name_);
  if (!font) {
    return text.length() * font_size * 0.5f;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, font_size);
  float width = 0.0f;

  for (size_t i = 0; i < text.length(); i++) {
    int codepoint = static_cast<unsigned char>(text[i]);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->font_info, codepoint, &advance, &lsb);
    width += advance * scale;

    if (i + 1 < text.length()) {
      int next_codepoint = static_cast<unsigned char>(text[i + 1]);
      int kern = stbtt_GetCodepointKernAdvance(&font->font_info, codepoint, next_codepoint);
      width += kern * scale;
    }
  }

  return width;
}

float Blend2DBackend::render_text_string(const std::string& raw_text,
    float x, float y, float font_size,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  std::string text = raw_text;

  // Check for Type 3 font (user-defined glyphs) first
  auto* type3_font = as_type3_font(current_font_);
  if (type3_font) {
    float cursor_x = x;

    for (size_t i = 0; i < text.length(); ++i) {
      uint8_t char_code = static_cast<uint8_t>(text[i]);

      // Get glyph name from encoding
      std::string glyph_name;
      if (char_code < type3_font->encoding.size()) {
        glyph_name = type3_font->encoding[char_code];
      }

      if (glyph_name.empty() || glyph_name == ".notdef") {
        // No glyph - draw tofu box placeholder and advance
        draw_missing_glyph_placeholder(cursor_x, y, font_size, r, g, b, a);
        cursor_x += font_size * 0.6f;
        continue;
      }

      // Render the Type 3 glyph
      render_type3_glyph(type3_font, glyph_name, cursor_x, y, font_size, r, g, b, a);

      // Get advance width from Widths array if available
      float advance = font_size * 0.6f;  // Default advance
      if (char_code < type3_font->widths.size()) {
        double fm_a = type3_font->font_matrix.size() >= 1 ? type3_font->font_matrix[0] : 0.001;
        advance = static_cast<float>(type3_font->widths[char_code] * fm_a * font_size);
      }

      cursor_x += advance;
    }
    return cursor_x - x;
  }

  FontCache* font = get_font(current_font_name_);
  float cursor_x = x;

  if (!font) {
    // Fallback: draw per-character tofu box placeholders
    float char_width = font_size * 0.5f;
    for (size_t i = 0; i < text.length(); i++) {
      draw_missing_glyph_placeholder(cursor_x, y, font_size, r, g, b, a);
      cursor_x += char_width * 1.2f;
    }
    return cursor_x - x;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, font_size);

  // Check if this is a Type0/CID font that uses two-byte encoding
  auto* type0_font = as_type0_font(current_font_);
  bool is_two_byte_cid = is_two_byte_cid_font(type0_font);


  for (size_t i = 0; i < text.length(); ) {
    uint32_t char_code;
    size_t bytes_consumed = 1;

    if (type0_font && is_two_byte_cid && i + 1 < text.length()) {
      // Two-byte CID encoding: high byte first, then low byte
      uint8_t high_byte = static_cast<unsigned char>(text[i]);
      uint8_t low_byte = static_cast<unsigned char>(text[i + 1]);
      char_code = (static_cast<uint32_t>(high_byte) << 8) | low_byte;
      bytes_consumed = 2;
    } else {
      // Single-byte encoding
      char_code = static_cast<unsigned char>(text[i]);
    }

    // For Type0/CID fonts, check CIDToGIDMap first
    if (type0_font && !type0_font->cid_to_gid_map.empty()) {
      if (char_code < type0_font->cid_to_gid_map.size()) {
        // Use glyph index directly instead of codepoint
        int gid = type0_font->cid_to_gid_map[char_code];
        // Draw using glyph index
        draw_glyph_by_index(gid, cursor_x, y, font_size, r, g, b, a);

        // Get advance width - prefer PDF width table, fall back to stbtt
        float advance_width;
        auto width_it = type0_font->cid_widths.find(char_code);
        if (width_it != type0_font->cid_widths.end()) {
          advance_width = width_it->second / 1000.0f * font_size;
        } else {
          advance_width = type0_font->default_width / 1000.0f * font_size;
        }
        cursor_x += advance_width;
        i += bytes_consumed;
        continue;
      }
    }

    // For Identity-H/Identity-V CMap without CIDToGIDMap
    // Skip this path - it only works with embedded CFF fonts which we can't render
    // Always fall through to Unicode-based rendering with CID-to-Unicode mapping

    // Try ToUnicode CMap for Type0 fonts
    if (type0_font && !type0_font->to_unicode_cmap.code_to_unicode.empty()) {
      uint32_t unicode = type0_font->to_unicode_cmap.map_code_to_unicode(char_code);
      if (unicode != char_code || type0_font->to_unicode_cmap.code_to_unicode.count(char_code)) {
        // Found a mapping, use it
        // printf("DEBUG: ToUnicode CMap: %u -> U+%04X\n", char_code, unicode);
        draw_glyph(static_cast<int>(unicode), cursor_x, y, font_size, r, g, b, a);

        // Get advance width - prefer PDF width table, fall back to stbtt
        float advance_width;
        auto width_it = type0_font->cid_widths.find(char_code);
        if (width_it != type0_font->cid_widths.end()) {
          advance_width = width_it->second / 1000.0f * font_size;
        } else if (type0_font->default_width > 0) {
          advance_width = type0_font->default_width / 1000.0f * font_size;
        } else {
          int advance, lsb;
          stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(unicode), &advance, &lsb);
          advance_width = advance * scale;
        }
        cursor_x += advance_width;
        i += bytes_consumed;
        continue;
      }
    }

    // Map character code to Unicode using encoding tables and glyph names
    uint32_t codepoint = map_char_to_unicode(char_code, current_font_);

    // Draw the glyph
    draw_glyph(static_cast<int>(codepoint), cursor_x, y, font_size, r, g, b, a);

    // Get advance width
    float advance_width;
    if (current_font_ && !current_font_->widths.empty()) {
      int first_char = current_font_->first_char;
      int last_char = current_font_->last_char;
      if (static_cast<int>(char_code) >= first_char &&
          static_cast<int>(char_code) <= last_char) {
        size_t idx = char_code - first_char;
        if (idx < current_font_->widths.size()) {
          advance_width = current_font_->widths[idx] / 1000.0f * font_size;
        } else {
          int advance, lsb;
          stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(codepoint), &advance, &lsb);
          advance_width = advance * scale;
        }
      } else {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(codepoint), &advance, &lsb);
        advance_width = advance * scale;
      }
    } else {
      int advance, lsb;
      stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(codepoint), &advance, &lsb);
      advance_width = advance * scale;
    }

    cursor_x += advance_width;

    // Add kerning if there's a next character (only for single-byte fonts)
    if (!is_two_byte_cid && i + bytes_consumed < text.length()) {
      uint32_t next_char_code = static_cast<unsigned char>(text[i + bytes_consumed]);
      uint32_t next_codepoint = map_char_to_unicode(next_char_code, current_font_);
      int kern = stbtt_GetCodepointKernAdvance(&font->font_info,
                                                static_cast<int>(codepoint),
                                                static_cast<int>(next_codepoint));
      cursor_x += kern * scale;
    }

    i += bytes_consumed;
  }

  return cursor_x - x;  // Return total width rendered
}

// Forward declaration for evaluate_pdf_function (defined below)
static bool evaluate_pdf_function(const Pdf& pdf, const Value& function,
                                   const std::vector<double>& inputs,
                                   std::vector<double>& outputs);

bool Blend2DBackend::draw_image(const ImageXObject& image, float x, float y, float w, float h,
                                uint8_t fill_r, uint8_t fill_g, uint8_t fill_b) {
  if (!initialized_ || image.data.empty()) {
    return false;
  }

  int img_width = image.width;
  int img_height = image.height;

  // Create Blend2D image
  BLImage img;
  img.create(img_width, img_height, BL_FORMAT_PRGB32);

  BLImageData img_data;
  img.get_data(&img_data);

  uint8_t* dst = static_cast<uint8_t*>(img_data.pixel_data);

  // Fast path: cached ARGB from render cache.
  std::vector<uint32_t> argb_data;
  if (image.color_space.type == ColorSpaceType::CacheARGB) {
    argb_data.resize(static_cast<size_t>(img_width) * img_height);
    memcpy(argb_data.data(), image.data.data(), argb_data.size() * sizeof(uint32_t));
    // Convert cached ARGB to BGRA for Blend2D
    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        size_t src_idx = static_cast<size_t>(row) * img_width + col;
        size_t dst_idx = row * img_data.stride + col * 4;
        uint32_t argb = argb_data[src_idx];
        uint8_t a = (argb >> 24) & 0xFF;
        uint8_t r = (argb >> 16) & 0xFF;
        uint8_t g = (argb >> 8) & 0xFF;
        uint8_t b = argb & 0xFF;
        dst[dst_idx + 0] = b;
        dst[dst_idx + 1] = g;
        dst[dst_idx + 2] = r;
        dst[dst_idx + 3] = a;
      }
    }
  } else {
    argb_data.resize(img_width * img_height);
    const uint8_t* src = image.data.data();

  // Handle image mask (1-bit stencil data)
  if (image.image_mask) {
    int stride = (img_width + 7) / 8;  // Bytes per row

    // PDF spec: With default Decode [0 1], sample 0 = paint, sample 1 = masked.
    // With Decode [1 0], sample 1 = paint, sample 0 = masked.
    bool invert = false;
    if (image.decode.size() >= 2 && image.decode[0] > image.decode[1]) {
      invert = true;
    }

    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        int byte_idx = row * stride + col / 8;
        int bit_idx = 7 - (col % 8);
        size_t dst_idx = row * img_data.stride + col * 4;

        if (byte_idx < static_cast<int>(image.data.size())) {
          bool bit_set = (image.data[byte_idx] >> bit_idx) & 1;
          bool should_paint = invert ? bit_set : !bit_set;
          if (should_paint) {
            // PRGB32 is BGRA with premultiplied alpha
            dst[dst_idx + 0] = fill_b;
            dst[dst_idx + 1] = fill_g;
            dst[dst_idx + 2] = fill_r;
            dst[dst_idx + 3] = 255;
          } else {
            dst[dst_idx + 0] = 0;
            dst[dst_idx + 1] = 0;
            dst[dst_idx + 2] = 0;
            dst[dst_idx + 3] = 0;
          }
        }
      }
    }
  }
  // Handle ICCBased color space with profile conversion
  else if (image.color_space.type == ColorSpaceType::ICCBased) {
    std::vector<uint8_t> rgb_data;
    convert_icc_to_srgb(image.data, rgb_data, img_width, img_height, image.color_space);

    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        size_t src_idx = (row * img_width + col) * 3;
        size_t dst_idx = row * img_data.stride + col * 4;

        uint8_t r_val = 0, g_val = 0, b_val = 0;
        if (src_idx + 2 < rgb_data.size()) {
          r_val = rgb_data[src_idx];
          g_val = rgb_data[src_idx + 1];
          b_val = rgb_data[src_idx + 2];
        }

        // PRGB32 is BGRA
        dst[dst_idx + 0] = b_val;
        dst[dst_idx + 1] = g_val;
        dst[dst_idx + 2] = r_val;
        dst[dst_idx + 3] = 255;
      }
    }
  }
  // Handle Indexed color space
  else if (image.color_space.type == ColorSpaceType::Indexed) {
    const auto& lookup = image.color_space.lookup_table;
    int base_components = 3;  // Assume RGB base
    if (image.color_space.base_color_space) {
      ColorSpaceType base_type = image.color_space.base_color_space->type;
      if (base_type == ColorSpaceType::DeviceGray || base_type == ColorSpaceType::CalGray) {
        base_components = 1;
      } else if (base_type == ColorSpaceType::DeviceCMYK) {
        base_components = 4;
      }
    }

    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        int pixel = row * img_width + col;
        size_t dst_idx = row * img_data.stride + col * 4;
        uint8_t idx = (pixel < static_cast<int>(image.data.size())) ? image.data[pixel] : 0;
        int lookup_idx = idx * base_components;

        uint8_t r = 0, g = 0, b = 0;
        if (base_components == 1 && lookup_idx < static_cast<int>(lookup.size())) {
          r = g = b = lookup[lookup_idx];
        } else if (base_components == 3 && lookup_idx + 2 < static_cast<int>(lookup.size())) {
          r = lookup[lookup_idx];
          g = lookup[lookup_idx + 1];
          b = lookup[lookup_idx + 2];
        } else if (base_components == 4 && lookup_idx + 3 < static_cast<int>(lookup.size())) {
          float c = lookup[lookup_idx] / 255.0f;
          float m = lookup[lookup_idx + 1] / 255.0f;
          float y_val = lookup[lookup_idx + 2] / 255.0f;
          float k = lookup[lookup_idx + 3] / 255.0f;
          r = static_cast<uint8_t>(255 * (1.0f - c) * (1.0f - k));
          g = static_cast<uint8_t>(255 * (1.0f - m) * (1.0f - k));
          b = static_cast<uint8_t>(255 * (1.0f - y_val) * (1.0f - k));
        }

        // PRGB32 is BGRA
        dst[dst_idx + 0] = b;
        dst[dst_idx + 1] = g;
        dst[dst_idx + 2] = r;
        dst[dst_idx + 3] = 255;
      }
    }
  }
  // Handle Separation color space (single spot color)
  else if (image.color_space.type == ColorSpaceType::Separation) {
    const auto& tint_func = image.color_space.tint_function;

    // Determine alternate color space components
    int alt_components = 4;  // Default CMYK
    if (image.color_space.base_color_space) {
      ColorSpaceType alt_type = image.color_space.base_color_space->type;
      if (alt_type == ColorSpaceType::DeviceGray || alt_type == ColorSpaceType::CalGray) {
        alt_components = 1;
      } else if (alt_type == ColorSpaceType::DeviceRGB || alt_type == ColorSpaceType::CalRGB) {
        alt_components = 3;
      }
    }

    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        int pixel = row * img_width + col;
        size_t dst_idx = row * img_data.stride + col * 4;

        // Get tint value (0-1 range from 8-bit data)
        double tint = (pixel < static_cast<int>(image.data.size())) ? image.data[pixel] / 255.0 : 0.0;

        // Evaluate tint function if present
        std::vector<double> outputs;
        uint8_t r = 0, g = 0, b = 0;

        if (tint_func.type != Value::UNDEFINED && current_pdf_ &&
            evaluate_pdf_function(*current_pdf_, tint_func, {tint}, outputs) &&
            !outputs.empty()) {
          // Convert alternate color to RGB
          if (alt_components == 1) {
            uint8_t gray = static_cast<uint8_t>(outputs[0] * 255);
            r = g = b = gray;
          } else if (alt_components == 3 && outputs.size() >= 3) {
            r = static_cast<uint8_t>(outputs[0] * 255);
            g = static_cast<uint8_t>(outputs[1] * 255);
            b = static_cast<uint8_t>(outputs[2] * 255);
          } else if (alt_components == 4 && outputs.size() >= 4) {
            float c = static_cast<float>(outputs[0]);
            float m = static_cast<float>(outputs[1]);
            float y = static_cast<float>(outputs[2]);
            float k = static_cast<float>(outputs[3]);
            r = static_cast<uint8_t>(255 * (1.0f - c) * (1.0f - k));
            g = static_cast<uint8_t>(255 * (1.0f - m) * (1.0f - k));
            b = static_cast<uint8_t>(255 * (1.0f - y) * (1.0f - k));
          }
        } else {
          // Fallback: treat as grayscale tint
          uint8_t gray = static_cast<uint8_t>((1.0 - tint) * 255);
          r = g = b = gray;
        }

        // PRGB32 is BGRA
        dst[dst_idx + 0] = b;
        dst[dst_idx + 1] = g;
        dst[dst_idx + 2] = r;
        dst[dst_idx + 3] = 255;
      }
    }
  }
  // Handle DeviceN color space (multiple spot colors)
  else if (image.color_space.type == ColorSpaceType::DeviceN) {
    int n_colorants = static_cast<int>(image.color_space.colorant_names.size());
    if (n_colorants == 0) n_colorants = image.color_space.num_components;
    if (n_colorants == 0) n_colorants = 1;  // Fallback
    const auto& tint_func = image.color_space.tint_function;

    // Determine alternate color space components
    int alt_components = 4;  // Default CMYK
    if (image.color_space.base_color_space) {
      ColorSpaceType alt_type = image.color_space.base_color_space->type;
      if (alt_type == ColorSpaceType::DeviceGray || alt_type == ColorSpaceType::CalGray) {
        alt_components = 1;
      } else if (alt_type == ColorSpaceType::DeviceRGB || alt_type == ColorSpaceType::CalRGB) {
        alt_components = 3;
      }
    }

    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        int pixel = row * img_width + col;
        int src_idx = pixel * n_colorants;
        size_t dst_idx = row * img_data.stride + col * 4;

        // Collect tint values
        std::vector<double> inputs;
        for (int j = 0; j < n_colorants; j++) {
          if (src_idx + j < static_cast<int>(image.data.size())) {
            inputs.push_back(image.data[src_idx + j] / 255.0);
          } else {
            inputs.push_back(0.0);
          }
        }

        // Evaluate tint function
        std::vector<double> outputs;
        uint8_t r = 0, g = 0, b = 0;

        if (tint_func.type != Value::UNDEFINED && current_pdf_ &&
            evaluate_pdf_function(*current_pdf_, tint_func, inputs, outputs) &&
            !outputs.empty()) {
          // Convert alternate color to RGB
          if (alt_components == 1) {
            uint8_t gray = static_cast<uint8_t>(outputs[0] * 255);
            r = g = b = gray;
          } else if (alt_components == 3 && outputs.size() >= 3) {
            r = static_cast<uint8_t>(outputs[0] * 255);
            g = static_cast<uint8_t>(outputs[1] * 255);
            b = static_cast<uint8_t>(outputs[2] * 255);
          } else if (alt_components == 4 && outputs.size() >= 4) {
            float c = static_cast<float>(outputs[0]);
            float m = static_cast<float>(outputs[1]);
            float y = static_cast<float>(outputs[2]);
            float k = static_cast<float>(outputs[3]);
            r = static_cast<uint8_t>(255 * (1.0f - c) * (1.0f - k));
            g = static_cast<uint8_t>(255 * (1.0f - m) * (1.0f - k));
            b = static_cast<uint8_t>(255 * (1.0f - y) * (1.0f - k));
          }
        } else {
          // Fallback: average tints as gray
          double avg = 0;
          for (double t : inputs) avg += t;
          avg /= (inputs.empty() ? 1.0 : inputs.size());
          uint8_t gray = static_cast<uint8_t>((1.0 - avg) * 255);
          r = g = b = gray;
        }

        // PRGB32 is BGRA
        dst[dst_idx + 0] = b;
        dst[dst_idx + 1] = g;
        dst[dst_idx + 2] = r;
        dst[dst_idx + 3] = 255;
      }
    }
  } else {
    // Convert image data to PRGB32
    int components = 1;
    if (image.color_space.type == ColorSpaceType::DeviceRGB) components = 3;
    else if (image.color_space.type == ColorSpaceType::DeviceCMYK) components = 4;
    else if (image.color_space.type == ColorSpaceType::DeviceGray) components = 1;

    size_t src_size = image.data.size();
    size_t expected_size = static_cast<size_t>(img_width) * img_height * components;

    // Bounds check - if data is too small, fill with gray
    if (src_size < expected_size) {
#ifdef NANOPDF_DEBUG_PRINT
      printf("DEBUG: Image data size mismatch: got %zu, expected %zu\n", src_size, expected_size);
#endif
      // Fill with gray placeholder
      for (int row = 0; row < img_height; row++) {
        for (int col = 0; col < img_width; col++) {
          size_t dst_idx = row * img_data.stride + col * 4;
          dst[dst_idx + 0] = 200;  // B
          dst[dst_idx + 1] = 200;  // G
          dst[dst_idx + 2] = 200;  // R
          dst[dst_idx + 3] = 255;  // A
        }
      }
    } else {
      for (int row = 0; row < img_height; row++) {
        for (int col = 0; col < img_width; col++) {
          size_t src_idx = (row * img_width + col) * components;
          size_t dst_idx = row * img_data.stride + col * 4;

          uint8_t r_val, g_val, b_val, a_val = 255;

          if (components == 1) {
            r_val = g_val = b_val = src[src_idx];
          } else if (components == 3) {
            r_val = src[src_idx];
            g_val = src[src_idx + 1];
            b_val = src[src_idx + 2];
          } else if (components == 4) {
            // CMYK to RGB
            float c = src[src_idx] / 255.0f;
            float m = src[src_idx + 1] / 255.0f;
            float y_val = src[src_idx + 2] / 255.0f;
            float k = src[src_idx + 3] / 255.0f;
            r_val = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
            g_val = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
            b_val = static_cast<uint8_t>((1.0f - y_val) * (1.0f - k) * 255);
          } else {
            r_val = g_val = b_val = 128;
          }

          // PRGB32 is BGRA
          dst[dst_idx + 0] = b_val;
          dst[dst_idx + 1] = g_val;
          dst[dst_idx + 2] = r_val;
          dst[dst_idx + 3] = a_val;
        }
      }
    }

    // Populate argb_data from BGRA for render cache.
    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        size_t dst_idx = row * img_data.stride + col * 4;
        size_t src_idx = static_cast<size_t>(row) * img_width + col;
        uint8_t b = dst[dst_idx + 0];
        uint8_t g = dst[dst_idx + 1];
        uint8_t r = dst[dst_idx + 2];
        uint8_t a = dst[dst_idx + 3];
        argb_data[src_idx] = (static_cast<uint32_t>(a) << 24) |
                             (static_cast<uint32_t>(r) << 16) |
                             (static_cast<uint32_t>(g) << 8) | b;
      }
    }
  }  // else (non-cached ARGB path)

  // Save ARGB for render cache if needed.
  last_image_argb_ = std::move(argb_data);

  // Draw image scaled to fit
  ctx_.save();
  ctx_.translate(x, y);
  ctx_.scale(w / img_width, h / img_height);
  ctx_.set_pattern_quality(BL_PATTERN_QUALITY_BILINEAR);
  ctx_.blit_image(BLPointI(0, 0), img);
  ctx_.restore();

  return true;
}

// C++17 clamp wrapper
template<typename T>
constexpr T clamp14(const T& v, const T& lo, const T& hi) {
  return std::clamp(v, lo, hi);
}

// PDF function evaluator - evaluates a PDF function at given input values
static bool evaluate_pdf_function(const Pdf& pdf, const Value& function,
                                   const std::vector<double>& inputs,
                                   std::vector<double>& outputs) {
  return pdfunc::evaluate(pdf, function, inputs, outputs);
}

// Evaluate Separation color space tint and convert to RGB
static bool evaluate_separation_color(const Pdf& pdf, const ColorSpace& cs,
                                       double tint, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (cs.type != ColorSpaceType::Separation) {
    return false;
  }

  // Evaluate the tint transform function
  // First, resolve the tint function if it's a reference
  Value tint_func = cs.tint_function;
  if (tint_func.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, tint_func.ref_object_number, tint_func.ref_generation_number);
    if (resolved.success) {
      tint_func = resolved.value;
    }
  }

  std::vector<double> outputs;
  bool eval_result = false;
  if (tint_func.type != Value::UNDEFINED) {
    eval_result = evaluate_pdf_function(pdf, tint_func, {tint}, outputs);
  }
  if (tint_func.type != Value::UNDEFINED && eval_result && !outputs.empty()) {

    // Determine alternate color space type and convert to RGB
    ColorSpaceType alt_type = ColorSpaceType::DeviceGray;
    int num_components = 1;
    if (cs.base_color_space) {
      alt_type = cs.base_color_space->type;
      // For ICCBased, get number of components
      if (alt_type == ColorSpaceType::ICCBased) {
        num_components = cs.base_color_space->num_components;
        if (num_components == 0) num_components = 3;  // Default to RGB
      }
    }

    if (alt_type == ColorSpaceType::DeviceGray || alt_type == ColorSpaceType::CalGray) {
      if (outputs.size() >= 1) {
        uint8_t gray = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
        r = g = b = gray;
        return true;
      }
    } else if (alt_type == ColorSpaceType::DeviceRGB || alt_type == ColorSpaceType::CalRGB ||
               (alt_type == ColorSpaceType::ICCBased && num_components == 3)) {
      // ICCBased with 3 components is treated as RGB
      if (outputs.size() >= 3) {
        r = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
        g = static_cast<uint8_t>(clamp14(outputs[1], 0.0, 1.0) * 255);
        b = static_cast<uint8_t>(clamp14(outputs[2], 0.0, 1.0) * 255);
        return true;
      }
    } else if (alt_type == ColorSpaceType::DeviceCMYK ||
               (alt_type == ColorSpaceType::ICCBased && num_components == 4)) {
      // ICCBased with 4 components is treated as CMYK
      if (outputs.size() >= 4) {
        float c = static_cast<float>(clamp14(outputs[0], 0.0, 1.0));
        float m = static_cast<float>(clamp14(outputs[1], 0.0, 1.0));
        float y = static_cast<float>(clamp14(outputs[2], 0.0, 1.0));
        float k = static_cast<float>(clamp14(outputs[3], 0.0, 1.0));
        r = static_cast<uint8_t>(255 * (1.0f - c) * (1.0f - k));
        g = static_cast<uint8_t>(255 * (1.0f - m) * (1.0f - k));
        b = static_cast<uint8_t>(255 * (1.0f - y) * (1.0f - k));
        return true;
      }
    } else if (alt_type == ColorSpaceType::ICCBased && num_components == 1) {
      // ICCBased with 1 component is treated as grayscale
      if (outputs.size() >= 1) {
        uint8_t gray = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
        r = g = b = gray;
        return true;
      }
    }
  }

  // Fallback: treat as grayscale tint
  uint8_t gray = static_cast<uint8_t>((1.0 - tint) * 255);
  r = g = b = gray;
  return true;
}

// Helper to lookup a resource, checking Form XObject resources first, then page resources
const Value* Blend2DBackend::lookup_resource(const std::string& resource_type, const std::string& name) const {
  // Check Form XObject resources stack (from top to bottom)
  for (auto it = form_resources_stack_.rbegin(); it != form_resources_stack_.rend(); ++it) {
    auto type_it = it->find(resource_type);
    if (type_it != it->end()) {
      Value resolved_dict = type_it->second;

      // Resolve reference if needed
      if (resolved_dict.type == Value::REFERENCE && current_pdf_) {
        auto resolved = resolve_reference(*current_pdf_, resolved_dict.ref_object_number,
                                          resolved_dict.ref_generation_number);
        if (resolved.success) {
          resolved_dict = resolved.value;
        }
      }

      if (resolved_dict.type == Value::DICTIONARY) {
        auto name_it = resolved_dict.dict.find(name);
        if (name_it != resolved_dict.dict.end()) {
          return &name_it->second;
        }
      }
    }
  }

  // Fall back to page resources
  if (current_page_) {
    auto type_it = current_page_->resources.find(resource_type);
    if (type_it != current_page_->resources.end()) {
      Value resolved_dict = type_it->second;

      // Resolve reference if needed
      if (resolved_dict.type == Value::REFERENCE && current_pdf_) {
        auto resolved = resolve_reference(*current_pdf_, resolved_dict.ref_object_number,
                                          resolved_dict.ref_generation_number);
        if (resolved.success) {
          resolved_dict = resolved.value;
        }
      }

      if (resolved_dict.type == Value::DICTIONARY) {
        auto name_it = resolved_dict.dict.find(name);
        if (name_it != resolved_dict.dict.end()) {
          return &name_it->second;
        }
      }
    }
  }

  return nullptr;
}

Dictionary Blend2DBackend::lookup_resource_type_dict(const std::string& resource_type) const {
  // Check Form XObject resources stack (from top to bottom)
  for (auto it = form_resources_stack_.rbegin(); it != form_resources_stack_.rend(); ++it) {
    auto type_it = it->find(resource_type);
    if (type_it != it->end()) {
      Value resolved_dict = type_it->second;
      if (resolved_dict.type == Value::REFERENCE && current_pdf_) {
        auto resolved = resolve_reference(*current_pdf_, resolved_dict.ref_object_number,
                                          resolved_dict.ref_generation_number);
        if (resolved.success && resolved.value.type == Value::DICTIONARY) {
          return resolved.value.dict;
        }
      } else if (resolved_dict.type == Value::DICTIONARY) {
        return resolved_dict.dict;
      }
    }
  }

  // Fall back to page resources
  if (current_page_) {
    auto type_it = current_page_->resources.find(resource_type);
    if (type_it != current_page_->resources.end()) {
      Value resolved_dict = type_it->second;
      if (resolved_dict.type == Value::REFERENCE && current_pdf_) {
        auto resolved = resolve_reference(*current_pdf_, resolved_dict.ref_object_number,
                                          resolved_dict.ref_generation_number);
        if (resolved.success && resolved.value.type == Value::DICTIONARY) {
          return resolved.value.dict;
        }
      } else if (resolved_dict.type == Value::DICTIONARY) {
        return resolved_dict.dict;
      }
    }
  }

  return Dictionary();
}

// Extract color stops from a PDF function (Type 2 or Type 3 stitching)
// Returns vector of (offset, BLRgba32) pairs suitable for BLGradient::add_stop()
static std::vector<std::pair<float, BLRgba32>> extract_color_stops_from_function(
    const Pdf& pdf, const Value& function) {
  std::vector<std::pair<float, BLRgba32>> stops;

  if (function.type != Value::DICTIONARY) {
    stops.push_back({0.0f, BLRgba32(0, 0, 0, 255)});
    stops.push_back({1.0f, BLRgba32(255, 255, 255, 255)});
    return stops;
  }

  auto func_type_it = function.dict.find("FunctionType");
  if (func_type_it == function.dict.end() || func_type_it->second.type != Value::NUMBER) {
    stops.push_back({0.0f, BLRgba32(0, 0, 0, 255)});
    stops.push_back({1.0f, BLRgba32(255, 255, 255, 255)});
    return stops;
  }

  int func_type = static_cast<int>(func_type_it->second.number);

  if (func_type == 2) {
    // Type 2: Exponential interpolation function
    auto c0_it = function.dict.find("C0");
    auto c1_it = function.dict.find("C1");

    float r0 = 0, g0 = 0, b0 = 0;
    float r1 = 1, g1 = 1, b1 = 1;

    if (c0_it != function.dict.end() && c0_it->second.type == Value::ARRAY &&
        c0_it->second.array.size() >= 3) {
      r0 = static_cast<float>(c0_it->second.array[0].number);
      g0 = static_cast<float>(c0_it->second.array[1].number);
      b0 = static_cast<float>(c0_it->second.array[2].number);
    }

    if (c1_it != function.dict.end() && c1_it->second.type == Value::ARRAY &&
        c1_it->second.array.size() >= 3) {
      r1 = static_cast<float>(c1_it->second.array[0].number);
      g1 = static_cast<float>(c1_it->second.array[1].number);
      b1 = static_cast<float>(c1_it->second.array[2].number);
    }

    stops.push_back({0.0f, BLRgba32(
      static_cast<uint8_t>(r0 * 255), static_cast<uint8_t>(g0 * 255),
      static_cast<uint8_t>(b0 * 255), 255)});
    stops.push_back({1.0f, BLRgba32(
      static_cast<uint8_t>(r1 * 255), static_cast<uint8_t>(g1 * 255),
      static_cast<uint8_t>(b1 * 255), 255)});
  }
  else if (func_type == 3) {
    // Type 3: Stitching function - combines multiple sub-functions
    auto functions_it = function.dict.find("Functions");
    auto bounds_it = function.dict.find("Bounds");

    if (functions_it == function.dict.end() || functions_it->second.type != Value::ARRAY) {
      stops.push_back({0.0f, BLRgba32(0, 0, 0, 255)});
      stops.push_back({1.0f, BLRgba32(255, 255, 255, 255)});
      return stops;
    }

    const auto& functions = functions_it->second.array;

    // Build boundary positions: [0, bounds[0], bounds[1], ..., 1]
    std::vector<float> boundaries;
    boundaries.push_back(0.0f);

    if (bounds_it != function.dict.end() && bounds_it->second.type == Value::ARRAY) {
      for (const auto& b : bounds_it->second.array) {
        if (b.type == Value::NUMBER) {
          boundaries.push_back(static_cast<float>(b.number));
        }
      }
    }
    boundaries.push_back(1.0f);

    // For each sub-function, extract its start color (C0)
    for (size_t i = 0; i < functions.size() && i < boundaries.size(); i++) {
      Value sub_func = functions[i];

      if (sub_func.type == Value::REFERENCE) {
        auto resolved = resolve_reference(pdf, sub_func.ref_object_number,
                                          sub_func.ref_generation_number);
        if (resolved.success) {
          sub_func = resolved.value;
        }
      }

      if (sub_func.type == Value::DICTIONARY) {
        auto sub_c0_it = sub_func.dict.find("C0");
        if (sub_c0_it != sub_func.dict.end() && sub_c0_it->second.type == Value::ARRAY &&
            sub_c0_it->second.array.size() >= 3) {
          float r = static_cast<float>(sub_c0_it->second.array[0].number);
          float g = static_cast<float>(sub_c0_it->second.array[1].number);
          float b = static_cast<float>(sub_c0_it->second.array[2].number);
          stops.push_back({boundaries[i], BLRgba32(
            static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255),
            static_cast<uint8_t>(b * 255), 255)});
        }
      }
    }

    // Add final color from last sub-function's C1
    if (!functions.empty()) {
      Value last_func = functions.back();
      if (last_func.type == Value::REFERENCE) {
        auto resolved = resolve_reference(pdf, last_func.ref_object_number,
                                          last_func.ref_generation_number);
        if (resolved.success) {
          last_func = resolved.value;
        }
      }

      if (last_func.type == Value::DICTIONARY) {
        auto c1_it = last_func.dict.find("C1");
        if (c1_it != last_func.dict.end() && c1_it->second.type == Value::ARRAY &&
            c1_it->second.array.size() >= 3) {
          float r = static_cast<float>(c1_it->second.array[0].number);
          float g = static_cast<float>(c1_it->second.array[1].number);
          float b = static_cast<float>(c1_it->second.array[2].number);
          stops.push_back({1.0f, BLRgba32(
            static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255),
            static_cast<uint8_t>(b * 255), 255)});
        }
      }
    }

    // Ensure we have at least 2 stops
    if (stops.size() < 2) {
      stops.clear();
      stops.push_back({0.0f, BLRgba32(0, 0, 0, 255)});
      stops.push_back({1.0f, BLRgba32(255, 255, 255, 255)});
    }
  }
  else {
    // Unsupported function type - default gradient
    stops.push_back({0.0f, BLRgba32(0, 0, 0, 255)});
    stops.push_back({1.0f, BLRgba32(255, 255, 255, 255)});
  }

  return stops;
}

// ============================================================
// Mesh shading helpers (shared between Types 4, 5, 6, 7)
// ============================================================

// Bit-packed stream reader for mesh shading data
class BitStream {
public:
  BitStream(const uint8_t* data, size_t size) : data_(data), size_(size), bit_pos_(0) {}

  bool is_eof() const { return bit_pos_ >= size_ * 8; }
  size_t bits_remaining() const { return size_ * 8 - bit_pos_; }

  uint32_t get_bits(uint32_t num_bits) {
    if (num_bits == 0 || num_bits > 32) return 0;
    if (bit_pos_ + num_bits > size_ * 8) return 0;

    uint32_t result = 0;
    while (num_bits > 0) {
      size_t byte_pos = bit_pos_ / 8;
      size_t bit_offset = bit_pos_ % 8;
      size_t bits_in_byte = std::min(num_bits, 8 - static_cast<uint32_t>(bit_offset));

      uint8_t mask = static_cast<uint8_t>((1 << bits_in_byte) - 1);
      uint8_t value = (data_[byte_pos] >> (8 - bit_offset - bits_in_byte)) & mask;

      result = (result << bits_in_byte) | value;
      bit_pos_ += bits_in_byte;
      num_bits -= bits_in_byte;
    }
    return result;
  }

  void byte_align() {
    if (bit_pos_ % 8 != 0) {
      bit_pos_ = ((bit_pos_ / 8) + 1) * 8;
    }
  }

private:
  const uint8_t* data_;
  size_t size_;
  size_t bit_pos_;
};

// Mesh vertex with position and RGB color
struct MeshVertex {
  float x, y;
  float r, g, b;  // RGB in [0,1] range
};

// Decode coordinate from bit-packed value
static float decode_coord(uint32_t encoded, uint32_t bits_per_coord,
                          float min_val, float max_val) {
  if (bits_per_coord == 0) return min_val;
  uint32_t max_encoded = (bits_per_coord == 32) ? 0xFFFFFFFF : (1u << bits_per_coord) - 1;
  if (max_encoded == 0) return min_val;
  return min_val + (encoded / static_cast<float>(max_encoded)) * (max_val - min_val);
}

// Decode color component from bit-packed value
static float decode_component(uint32_t encoded, uint32_t bits_per_component,
                               float min_val, float max_val) {
  if (bits_per_component == 0) return min_val;
  uint32_t max_encoded = (1u << bits_per_component) - 1;
  if (max_encoded == 0) return min_val;
  return min_val + (encoded / static_cast<float>(max_encoded)) * (max_val - min_val);
}

// Draw a Gouraud-shaded triangle to a bitmap (scanline algorithm)
// Pixel format: PRGB32 (BGRA with premultiplied alpha, opaque)
static void draw_gouraud_triangle(uint32_t* bitmap, int width, int height,
                                   const MeshVertex& v0, const MeshVertex& v1, const MeshVertex& v2) {
  float min_y = std::min({v0.y, v1.y, v2.y});
  float max_y = std::max({v0.y, v1.y, v2.y});

  if (min_y == max_y) return;

  int min_yi = std::max(static_cast<int>(std::floor(min_y)), 0);
  int max_yi = std::min(static_cast<int>(std::ceil(max_y)), height - 1);

  for (int y = min_yi; y <= max_yi; ++y) {
    float fy = static_cast<float>(y);

    struct Intersection {
      float x;
      float r, g, b;
    };
    std::vector<Intersection> intersections;

    const MeshVertex* edges[3][2] = {{&v0, &v1}, {&v1, &v2}, {&v2, &v0}};

    for (int i = 0; i < 3; ++i) {
      const MeshVertex* a = edges[i][0];
      const MeshVertex* bp = edges[i][1];

      if (a->y == bp->y) continue;

      bool intersects = (a->y < bp->y) ? (fy >= a->y && fy <= bp->y) : (fy >= bp->y && fy <= a->y);
      if (!intersects) continue;

      float t = (fy - a->y) / (bp->y - a->y);
      Intersection isect;
      isect.x = a->x + t * (bp->x - a->x);
      isect.r = a->r + t * (bp->r - a->r);
      isect.g = a->g + t * (bp->g - a->g);
      isect.b = a->b + t * (bp->b - a->b);
      intersections.push_back(isect);
    }

    if (intersections.size() != 2) continue;

    if (intersections[0].x > intersections[1].x) {
      std::swap(intersections[0], intersections[1]);
    }

    int x_start = std::max(static_cast<int>(std::floor(intersections[0].x)), 0);
    int x_end = std::min(static_cast<int>(std::ceil(intersections[1].x)), width - 1);

    if (x_start >= x_end) continue;

    float dx = intersections[1].x - intersections[0].x;
    if (dx == 0) continue;

    uint32_t* row = bitmap + y * width;
    for (int x = x_start; x <= x_end; ++x) {
      float t = (x - intersections[0].x) / dx;
      float cr = intersections[0].r + t * (intersections[1].r - intersections[0].r);
      float cg = intersections[0].g + t * (intersections[1].g - intersections[0].g);
      float cb = intersections[0].b + t * (intersections[1].b - intersections[0].b);

      uint8_t ri = static_cast<uint8_t>(clamp14(cr * 255.0f, 0.0f, 255.0f));
      uint8_t gi = static_cast<uint8_t>(clamp14(cg * 255.0f, 0.0f, 255.0f));
      uint8_t bi = static_cast<uint8_t>(clamp14(cb * 255.0f, 0.0f, 255.0f));

      // PRGB32: 0xAARRGGBB (opaque)
      row[x] = 0xFF000000u | (static_cast<uint32_t>(ri) << 16) |
               (static_cast<uint32_t>(gi) << 8) | static_cast<uint32_t>(bi);
    }
  }
}

// Cubic Bezier patch for Coons/Tensor-product patch meshes
struct BezierPatch {
  float points[4][4][2];  // 4x4 grid of control points (x, y)
  float colors[4][3];     // 4 corner colors (r, g, b)

  bool is_small() const {
    float min_x = points[0][0][0], max_x = points[0][0][0];
    float min_y = points[0][0][1], max_y = points[0][0][1];

    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        min_x = std::min(min_x, points[i][j][0]);
        max_x = std::max(max_x, points[i][j][0]);
        min_y = std::min(min_y, points[i][j][1]);
        max_y = std::max(max_y, points[i][j][1]);
      }
    }

    return (max_x - min_x < 2.0f) && (max_y - min_y < 2.0f);
  }

  void subdivide_vertical(BezierPatch& top, BezierPatch& bottom) const {
    for (int x = 0; x < 4; ++x) {
      float p0[2] = {points[x][0][0], points[x][0][1]};
      float p1[2] = {points[x][1][0], points[x][1][1]};
      float p2[2] = {points[x][2][0], points[x][2][1]};
      float p3[2] = {points[x][3][0], points[x][3][1]};

      float q0[2] = {(p0[0] + p1[0]) * 0.5f, (p0[1] + p1[1]) * 0.5f};
      float q1[2] = {(p1[0] + p2[0]) * 0.5f, (p1[1] + p2[1]) * 0.5f};
      float q2[2] = {(p2[0] + p3[0]) * 0.5f, (p2[1] + p3[1]) * 0.5f};

      float r0[2] = {(q0[0] + q1[0]) * 0.5f, (q0[1] + q1[1]) * 0.5f};
      float r1[2] = {(q1[0] + q2[0]) * 0.5f, (q1[1] + q2[1]) * 0.5f};

      float s[2] = {(r0[0] + r1[0]) * 0.5f, (r0[1] + r1[1]) * 0.5f};

      top.points[x][0][0] = p0[0]; top.points[x][0][1] = p0[1];
      top.points[x][1][0] = q0[0]; top.points[x][1][1] = q0[1];
      top.points[x][2][0] = r0[0]; top.points[x][2][1] = r0[1];
      top.points[x][3][0] = s[0];  top.points[x][3][1] = s[1];

      bottom.points[x][0][0] = s[0];  bottom.points[x][0][1] = s[1];
      bottom.points[x][1][0] = r1[0]; bottom.points[x][1][1] = r1[1];
      bottom.points[x][2][0] = q2[0]; bottom.points[x][2][1] = q2[1];
      bottom.points[x][3][0] = p3[0]; bottom.points[x][3][1] = p3[1];
    }

    for (int i = 0; i < 3; ++i) {
      top.colors[0][i] = colors[0][i];
      top.colors[1][i] = (colors[0][i] + colors[1][i]) * 0.5f;
      top.colors[2][i] = (colors[0][i] + colors[1][i]) * 0.5f;
      top.colors[3][i] = colors[1][i];

      bottom.colors[0][i] = (colors[0][i] + colors[1][i]) * 0.5f;
      bottom.colors[1][i] = colors[1][i];
      bottom.colors[2][i] = colors[2][i];
      bottom.colors[3][i] = (colors[2][i] + colors[3][i]) * 0.5f;
    }
  }

  void subdivide_horizontal(BezierPatch& left, BezierPatch& right) const {
    for (int y = 0; y < 4; ++y) {
      float p0[2] = {points[0][y][0], points[0][y][1]};
      float p1[2] = {points[1][y][0], points[1][y][1]};
      float p2[2] = {points[2][y][0], points[2][y][1]};
      float p3[2] = {points[3][y][0], points[3][y][1]};

      float q0[2] = {(p0[0] + p1[0]) * 0.5f, (p0[1] + p1[1]) * 0.5f};
      float q1[2] = {(p1[0] + p2[0]) * 0.5f, (p1[1] + p2[1]) * 0.5f};
      float q2[2] = {(p2[0] + p3[0]) * 0.5f, (p2[1] + p3[1]) * 0.5f};

      float r0[2] = {(q0[0] + q1[0]) * 0.5f, (q0[1] + q1[1]) * 0.5f};
      float r1[2] = {(q1[0] + q2[0]) * 0.5f, (q1[1] + q2[1]) * 0.5f};

      float s[2] = {(r0[0] + r1[0]) * 0.5f, (r0[1] + r1[1]) * 0.5f};

      left.points[0][y][0] = p0[0]; left.points[0][y][1] = p0[1];
      left.points[1][y][0] = q0[0]; left.points[1][y][1] = q0[1];
      left.points[2][y][0] = r0[0]; left.points[2][y][1] = r0[1];
      left.points[3][y][0] = s[0];  left.points[3][y][1] = s[1];

      right.points[0][y][0] = s[0];  right.points[0][y][1] = s[1];
      right.points[1][y][0] = r1[0]; right.points[1][y][1] = r1[1];
      right.points[2][y][0] = q2[0]; right.points[2][y][1] = q2[1];
      right.points[3][y][0] = p3[0]; right.points[3][y][1] = p3[1];
    }

    for (int i = 0; i < 3; ++i) {
      left.colors[0][i] = colors[0][i];
      left.colors[1][i] = (colors[0][i] + colors[3][i]) * 0.5f;
      left.colors[2][i] = (colors[0][i] + colors[3][i]) * 0.5f;
      left.colors[3][i] = colors[3][i];

      right.colors[0][i] = (colors[0][i] + colors[3][i]) * 0.5f;
      right.colors[1][i] = colors[3][i];
      right.colors[2][i] = colors[2][i];
      right.colors[3][i] = (colors[1][i] + colors[2][i]) * 0.5f;
    }
  }

  void draw_to_bitmap(uint32_t* bitmap, int width, int height) const {
    MeshVertex v0 = {points[0][0][0], points[0][0][1], colors[0][0], colors[0][1], colors[0][2]};
    MeshVertex v1 = {points[3][0][0], points[3][0][1], colors[3][0], colors[3][1], colors[3][2]};
    MeshVertex v2 = {points[3][3][0], points[3][3][1], colors[2][0], colors[2][1], colors[2][2]};
    MeshVertex v3 = {points[0][3][0], points[0][3][1], colors[1][0], colors[1][1], colors[1][2]};

    draw_gouraud_triangle(bitmap, width, height, v0, v1, v2);
    draw_gouraud_triangle(bitmap, width, height, v0, v2, v3);
  }
};

// Recursively subdivide and draw a Bezier patch
static void draw_patch_recursive(uint32_t* bitmap, int width, int height,
                                  const BezierPatch& patch, int depth = 0) {
  const int max_depth = 8;

  if (patch.is_small() || depth >= max_depth) {
    patch.draw_to_bitmap(bitmap, width, height);
    return;
  }

  BezierPatch top, bottom;
  patch.subdivide_vertical(top, bottom);

  BezierPatch top_left, top_right;
  top.subdivide_horizontal(top_left, top_right);

  BezierPatch bottom_left, bottom_right;
  bottom.subdivide_horizontal(bottom_left, bottom_right);

  draw_patch_recursive(bitmap, width, height, top_left, depth + 1);
  draw_patch_recursive(bitmap, width, height, top_right, depth + 1);
  draw_patch_recursive(bitmap, width, height, bottom_left, depth + 1);
  draw_patch_recursive(bitmap, width, height, bottom_right, depth + 1);
}

// Rasterize function-based shading (Type 1) to a pixel buffer
// Pixels are in BL_FORMAT_PRGB32 (BGRA with premultiplied alpha)
static bool rasterize_function_shading(const Pdf& pdf, const Shading& shading,
                                        int width, int height, float scale,
                                        float page_height,
                                        std::vector<uint32_t>& pixels) {
  pixels.resize(width * height);

  // Get domain bounds
  double x_min = shading.domain.size() >= 1 ? shading.domain[0] : 0.0;
  double x_max = shading.domain.size() >= 2 ? shading.domain[1] : 1.0;
  double y_min = shading.domain.size() >= 3 ? shading.domain[2] : 0.0;
  double y_max = shading.domain.size() >= 4 ? shading.domain[3] : 1.0;

  // Get transformation matrix
  double a = shading.matrix.size() >= 1 ? shading.matrix[0] : 1.0;
  double b = shading.matrix.size() >= 2 ? shading.matrix[1] : 0.0;
  double c = shading.matrix.size() >= 3 ? shading.matrix[2] : 0.0;
  double d = shading.matrix.size() >= 4 ? shading.matrix[3] : 1.0;
  double e = shading.matrix.size() >= 5 ? shading.matrix[4] : 0.0;
  double f = shading.matrix.size() >= 6 ? shading.matrix[5] : 0.0;

  // Compute inverse matrix for device-to-domain transform
  double det = a * d - b * c;
  if (std::abs(det) < 1e-10) {
    // Degenerate matrix
    std::fill(pixels.begin(), pixels.end(), 0xFF808080u);
    return true;
  }

  double inv_a = d / det;
  double inv_b = -b / det;
  double inv_c = -c / det;
  double inv_d = a / det;
  double inv_e = (c * f - d * e) / det;
  double inv_f = (b * e - a * f) / det;

  // Rasterize each pixel
  for (int py = 0; py < height; ++py) {
    for (int px = 0; px < width; ++px) {
      // Convert pixel coords to user space (flip Y)
      double ux = px / scale;
      double uy = (height - 1 - py) / scale;

      // Apply inverse matrix to get domain coordinates
      double dx = inv_a * ux + inv_c * uy + inv_e;
      double dy = inv_b * ux + inv_d * uy + inv_f;

      // Check if in domain
      uint8_t r = 128, g = 128, bl = 128, alpha = 255;

      if (dx >= x_min && dx <= x_max && dy >= y_min && dy <= y_max) {
        // Evaluate function at (dx, dy)
        std::vector<double> outputs;
        if (evaluate_pdf_function(pdf, shading.function, {dx, dy}, outputs)) {
          if (outputs.size() >= 3) {
            r = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
            g = static_cast<uint8_t>(clamp14(outputs[1], 0.0, 1.0) * 255);
            bl = static_cast<uint8_t>(clamp14(outputs[2], 0.0, 1.0) * 255);
          } else if (outputs.size() == 1) {
            uint8_t gray = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
            r = g = bl = gray;
          }
        }
      } else if (!shading.background.empty()) {
        if (shading.background.size() >= 3) {
          r = static_cast<uint8_t>(clamp14(shading.background[0], 0.0, 1.0) * 255);
          g = static_cast<uint8_t>(clamp14(shading.background[1], 0.0, 1.0) * 255);
          bl = static_cast<uint8_t>(clamp14(shading.background[2], 0.0, 1.0) * 255);
        }
      } else {
        alpha = 0;
      }

      // PRGB32 format: BGRA with premultiplied alpha
      // For opaque pixels (alpha=255), premultiplied == straight
      if (alpha == 0) {
        pixels[py * width + px] = 0;
      } else {
        pixels[py * width + px] = (static_cast<uint32_t>(alpha) << 24) |
                                   (static_cast<uint32_t>(r) << 16) |
                                   (static_cast<uint32_t>(g) << 8) |
                                   static_cast<uint32_t>(bl);
      }
    }
  }

  return true;
}

bool Blend2DBackend::draw_shading(const std::string& shading_name) {
  if (!current_pdf_ || !current_page_) {
    return false;
  }

  // Look up shading from resources (Form XObject stack first, then page)
  Dictionary shading_resources = lookup_resource_type_dict("Shading");
  if (shading_resources.empty()) {
    return false;
  }

  auto shading_it = shading_resources.find(shading_name);
  if (shading_it == shading_resources.end()) {
    return false;
  }

  // Resolve shading reference
  Dictionary shading_dict;
  if (shading_it->second.type == Value::DICTIONARY) {
    shading_dict = shading_it->second.dict;
  } else if (shading_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      shading_it->second.ref_object_number,
                                      shading_it->second.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    shading_dict = resolved.value.dict;
  } else {
    return false;
  }

  // Parse the shading
  auto shading = parse_shading(*current_pdf_, shading_dict);
  if (!shading) {
    return false;
  }

  // Create gradient based on shading type
  if (shading->type == ShadingType::Axial && shading->coords.size() >= 4) {
    // Linear gradient
    float x0 = static_cast<float>(shading->coords[0]) * state_.scale;
    float y0 = (state_.page_height - static_cast<float>(shading->coords[1])) * state_.scale;
    float x1 = static_cast<float>(shading->coords[2]) * state_.scale;
    float y1 = (state_.page_height - static_cast<float>(shading->coords[3])) * state_.scale;

    BLGradient gradient(BLLinearGradientValues(x0, y0, x1, y1));

    // Extract color stops from function (supports Type 2 and Type 3 stitching)
    auto color_stops = extract_color_stops_from_function(*current_pdf_, shading->function);
    for (auto& s : color_stops) {
      gradient.add_stop(s.first, s.second);
    }

    // Fill page with gradient
    ctx_.set_fill_style(gradient);
    ctx_.fill_rect(0, 0, width_, height_);
    return true;
  }
  else if (shading->type == ShadingType::Radial && shading->coords.size() >= 6) {
    // Radial gradient
    float x0 = static_cast<float>(shading->coords[0]) * state_.scale;
    float y0 = (state_.page_height - static_cast<float>(shading->coords[1])) * state_.scale;
    float r0 = static_cast<float>(shading->coords[2]) * state_.scale;
    float x1 = static_cast<float>(shading->coords[3]) * state_.scale;
    float y1 = (state_.page_height - static_cast<float>(shading->coords[4])) * state_.scale;
    float r1 = static_cast<float>(shading->coords[5]) * state_.scale;

    BLGradient gradient(BLRadialGradientValues(x1, y1, x0, y0, r1, r0));

    // Extract color stops from function (supports Type 2 and Type 3 stitching)
    auto color_stops = extract_color_stops_from_function(*current_pdf_, shading->function);
    for (auto& s : color_stops) {
      gradient.add_stop(s.first, s.second);
    }

    ctx_.set_fill_style(gradient);
    ctx_.fill_rect(0, 0, width_, height_);
    return true;
  }
  else if (shading->type == ShadingType::FunctionBased) {
    // Type 1: Function-based shading
    // Evaluate function at each point in domain and rasterize to bitmap

    // Calculate raster dimensions
    float x = 0, y = 0;
    float w = state_.page_width * state_.scale;
    float h = state_.page_height * state_.scale;

    if (shading->bbox.size() >= 4) {
      x = static_cast<float>(shading->bbox[0]) * state_.scale;
      y = (state_.page_height - static_cast<float>(shading->bbox[3])) * state_.scale;
      w = (static_cast<float>(shading->bbox[2]) - static_cast<float>(shading->bbox[0])) * state_.scale;
      h = (static_cast<float>(shading->bbox[3]) - static_cast<float>(shading->bbox[1])) * state_.scale;
    }

    int raster_w = static_cast<int>(w);
    int raster_h = static_cast<int>(h);
    if (raster_w <= 0) raster_w = 256;
    if (raster_h <= 0) raster_h = 256;

    std::vector<uint32_t> pixels;
    if (rasterize_function_shading(*current_pdf_, *shading, raster_w, raster_h,
                                    state_.scale, state_.page_height, pixels)) {
      // Create BLImage from rasterized pixels
      BLImage shading_img;
      shading_img.create_from_data(raster_w, raster_h, BL_FORMAT_PRGB32,
                                    pixels.data(), raster_w * 4);

      // Blit to context at the correct position
      ctx_.blit_image(BLPoint(x, y), shading_img);
      return true;
    }

    return false;
  }
  else if (shading->type == ShadingType::FreeFormTriangleMesh ||
           shading->type == ShadingType::LatticeFormTriangleMesh) {
    // Type 4/5: Triangle mesh shadings

    if (shading->data_stream.empty() || shading->decode.size() < 4) {
      return false;
    }

    // Calculate bitmap dimensions
    float x = 0, y = 0;
    float w = state_.page_width * state_.scale;
    float h = state_.page_height * state_.scale;
    if (shading->bbox.size() >= 4) {
      x = static_cast<float>(shading->bbox[0]) * state_.scale;
      y = (state_.page_height - static_cast<float>(shading->bbox[3])) * state_.scale;
      w = (static_cast<float>(shading->bbox[2]) - static_cast<float>(shading->bbox[0])) * state_.scale;
      h = (static_cast<float>(shading->bbox[3]) - static_cast<float>(shading->bbox[1])) * state_.scale;
    }

    int bmp_width = static_cast<int>(w);
    int bmp_height = static_cast<int>(h);
    if (bmp_width <= 0 || bmp_height <= 0 || bmp_width > 4096 || bmp_height > 4096) {
      return false;
    }

    std::vector<uint32_t> bitmap(bmp_width * bmp_height, 0x00000000);

    // Parse decode array
    float x_min = static_cast<float>(shading->decode[0]);
    float x_max = static_cast<float>(shading->decode[1]);
    float y_min = static_cast<float>(shading->decode[2]);
    float y_max = static_cast<float>(shading->decode[3]);

    int num_components = 3;
    std::vector<float> color_min, color_max;
    for (size_t i = 4; i + 1 < shading->decode.size() && i < 4 + num_components * 2; i += 2) {
      color_min.push_back(static_cast<float>(shading->decode[i]));
      color_max.push_back(static_cast<float>(shading->decode[i + 1]));
    }

    BitStream bit_stream(shading->data_stream.data(), shading->data_stream.size());

    if (shading->type == ShadingType::FreeFormTriangleMesh) {
      MeshVertex triangle[3];
      int vertex_count = 0;

      while (!bit_stream.is_eof()) {
        uint32_t flag = 0;
        if (shading->bits_per_flag > 0 && bit_stream.bits_remaining() >= static_cast<size_t>(shading->bits_per_flag)) {
          flag = bit_stream.get_bits(shading->bits_per_flag) & 0x03;
        } else {
          break;
        }

        if (bit_stream.bits_remaining() < static_cast<size_t>(shading->bits_per_coordinate * 2)) break;
        uint32_t x_enc = bit_stream.get_bits(shading->bits_per_coordinate);
        uint32_t y_enc = bit_stream.get_bits(shading->bits_per_coordinate);

        MeshVertex vert;
        vert.x = decode_coord(x_enc, shading->bits_per_coordinate, x_min, x_max) * state_.scale;
        vert.y = (y_max - decode_coord(y_enc, shading->bits_per_coordinate, y_min, y_max) + y_min) * state_.scale;

        if (bit_stream.bits_remaining() < static_cast<size_t>(shading->bits_per_component * num_components)) break;
        vert.r = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                  shading->bits_per_component,
                                  color_min.size() > 0 ? color_min[0] : 0.0f,
                                  color_max.size() > 0 ? color_max[0] : 1.0f);
        vert.g = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                  shading->bits_per_component,
                                  color_min.size() > 1 ? color_min[1] : 0.0f,
                                  color_max.size() > 1 ? color_max[1] : 1.0f);
        vert.b = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                  shading->bits_per_component,
                                  color_min.size() > 2 ? color_min[2] : 0.0f,
                                  color_max.size() > 2 ? color_max[2] : 1.0f);

        bit_stream.byte_align();

        if (flag == 0) {
          triangle[0] = vert;
          vertex_count = 1;
        } else {
          if (vertex_count < 3) {
            triangle[vertex_count++] = vert;
          }

          if (vertex_count == 3) {
            draw_gouraud_triangle(bitmap.data(), bmp_width, bmp_height,
                                  triangle[0], triangle[1], triangle[2]);

            if (flag == 1) {
              triangle[0] = triangle[1];
              triangle[1] = triangle[2];
            } else if (flag == 2) {
              triangle[1] = triangle[2];
            }
            triangle[2] = vert;
          }
        }
      }
    }

    // Create BLImage from bitmap and blit
    BLImage mesh_img;
    mesh_img.create_from_data(bmp_width, bmp_height, BL_FORMAT_PRGB32,
                               bitmap.data(), bmp_width * 4);

    ctx_.blit_image(BLPoint(x, y), mesh_img);
    return true;
  }
  else if (shading->type == ShadingType::CoonsPatchMesh ||
           shading->type == ShadingType::TensorProductPatchMesh) {
    // Type 6/7: Coons patch / Tensor-product patch mesh shadings

    bool is_tensor = (shading->type == ShadingType::TensorProductPatchMesh);

    if (shading->data_stream.empty() || shading->decode.size() < 4) {
      return false;
    }

    // Calculate bitmap dimensions
    float x = 0, y = 0;
    float w = state_.page_width * state_.scale;
    float h = state_.page_height * state_.scale;
    if (shading->bbox.size() >= 4) {
      x = static_cast<float>(shading->bbox[0]) * state_.scale;
      y = (state_.page_height - static_cast<float>(shading->bbox[3])) * state_.scale;
      w = (static_cast<float>(shading->bbox[2]) - static_cast<float>(shading->bbox[0])) * state_.scale;
      h = (static_cast<float>(shading->bbox[3]) - static_cast<float>(shading->bbox[1])) * state_.scale;
    }

    int bmp_width = static_cast<int>(w);
    int bmp_height = static_cast<int>(h);
    if (bmp_width <= 0 || bmp_height <= 0 || bmp_width > 4096 || bmp_height > 4096) {
      return false;
    }

    std::vector<uint32_t> bitmap(bmp_width * bmp_height, 0x00000000);

    float x_min = static_cast<float>(shading->decode[0]);
    float x_max = static_cast<float>(shading->decode[1]);
    float y_min = static_cast<float>(shading->decode[2]);
    float y_max = static_cast<float>(shading->decode[3]);

    int num_components = 3;
    std::vector<float> color_min, color_max;
    for (size_t i = 4; i + 1 < shading->decode.size() && i < 4 + num_components * 2; i += 2) {
      color_min.push_back(static_cast<float>(shading->decode[i]));
      color_max.push_back(static_cast<float>(shading->decode[i + 1]));
    }

    BitStream bit_stream(shading->data_stream.data(), shading->data_stream.size());

    BezierPatch prev_patch;
    bool have_prev = false;

    while (!bit_stream.is_eof()) {
      uint32_t flag = 0;
      if (shading->bits_per_flag > 0 && bit_stream.bits_remaining() >= static_cast<size_t>(shading->bits_per_flag)) {
        flag = bit_stream.get_bits(shading->bits_per_flag) & 0x03;
      } else {
        break;
      }

      BezierPatch patch;

      int num_points_to_read = is_tensor ? 16 : 12;
      if (flag > 0 && have_prev) {
        num_points_to_read = is_tensor ? 12 : 8;
      }

      // Read control points
      for (int i = 0; i < 4 && i < num_points_to_read / 4; ++i) {
        for (int j = 0; j < 4; ++j) {
          if (bit_stream.bits_remaining() < static_cast<size_t>(shading->bits_per_coordinate * 2)) break;

          uint32_t x_enc = bit_stream.get_bits(shading->bits_per_coordinate);
          uint32_t y_enc = bit_stream.get_bits(shading->bits_per_coordinate);

          patch.points[i][j][0] = decode_coord(x_enc, shading->bits_per_coordinate,
                                                x_min, x_max) * state_.scale;
          patch.points[i][j][1] = (y_max - decode_coord(y_enc, shading->bits_per_coordinate,
                                                         y_min, y_max) + y_min) * state_.scale;
        }
      }

      // Read 4 corner colors
      for (int corner = 0; corner < 4; ++corner) {
        if (bit_stream.bits_remaining() < static_cast<size_t>(shading->bits_per_component * num_components)) break;

        patch.colors[corner][0] = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                                    shading->bits_per_component,
                                                    color_min.size() > 0 ? color_min[0] : 0.0f,
                                                    color_max.size() > 0 ? color_max[0] : 1.0f);
        patch.colors[corner][1] = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                                    shading->bits_per_component,
                                                    color_min.size() > 1 ? color_min[1] : 0.0f,
                                                    color_max.size() > 1 ? color_max[1] : 1.0f);
        patch.colors[corner][2] = decode_component(bit_stream.get_bits(shading->bits_per_component),
                                                    shading->bits_per_component,
                                                    color_min.size() > 2 ? color_min[2] : 0.0f,
                                                    color_max.size() > 2 ? color_max[2] : 1.0f);
      }

      bit_stream.byte_align();

      draw_patch_recursive(bitmap.data(), bmp_width, bmp_height, patch);

      prev_patch = patch;
      have_prev = true;
    }

    // Create BLImage from bitmap and blit
    BLImage mesh_img;
    mesh_img.create_from_data(bmp_width, bmp_height, BL_FORMAT_PRGB32,
                               bitmap.data(), bmp_width * 4);

    ctx_.blit_image(BLPoint(x, y), mesh_img);
    return true;
  }

  return false;
}

// Helper to check if a path is a simple rectangle
static bool is_rectangular_path(const BLPath& path, BLBox& out_rect) {
  // Get path info
  const BLPathView view = path.view();
  if (view.size < 4) return false;

  // Check for rectangle pattern: moveTo, lineTo, lineTo, lineTo, close (or 4 lines)
  // or moveTo followed by rect primitive
  const uint8_t* cmd = view.command_data;
  const BLPoint* pts = view.vertex_data;

  size_t pt_idx = 0;
  std::vector<BLPoint> corners;

  for (size_t i = 0; i < view.size && corners.size() < 5; i++) {
    switch (cmd[i]) {
      case BL_PATH_CMD_MOVE:
        corners.push_back(pts[pt_idx++]);
        break;
      case BL_PATH_CMD_ON:  // Line to
        corners.push_back(pts[pt_idx++]);
        break;
      case BL_PATH_CMD_CLOSE:
        break;
      default:
        // Has curves or other commands - not a simple rectangle
        return false;
    }
  }

  // Need exactly 4 corners for a rectangle
  if (corners.size() < 4 || corners.size() > 5) return false;

  // Check if it forms an axis-aligned rectangle
  // All x or y coords should match in pairs
  float x_vals[4] = {static_cast<float>(corners[0].x), static_cast<float>(corners[1].x),
                     static_cast<float>(corners[2].x), static_cast<float>(corners[3].x)};
  float y_vals[4] = {static_cast<float>(corners[0].y), static_cast<float>(corners[1].y),
                     static_cast<float>(corners[2].y), static_cast<float>(corners[3].y)};

  // Find min/max
  float min_x = x_vals[0], max_x = x_vals[0];
  float min_y = y_vals[0], max_y = y_vals[0];
  for (int i = 1; i < 4; i++) {
    if (x_vals[i] < min_x) min_x = x_vals[i];
    if (x_vals[i] > max_x) max_x = x_vals[i];
    if (y_vals[i] < min_y) min_y = y_vals[i];
    if (y_vals[i] > max_y) max_y = y_vals[i];
  }

  // Check that each corner is at a min/max combination (axis-aligned)
  int corners_at_edges = 0;
  const float eps = 0.01f;
  for (int i = 0; i < 4; i++) {
    bool at_x_edge = (std::abs(x_vals[i] - min_x) < eps) || (std::abs(x_vals[i] - max_x) < eps);
    bool at_y_edge = (std::abs(y_vals[i] - min_y) < eps) || (std::abs(y_vals[i] - max_y) < eps);
    if (at_x_edge && at_y_edge) corners_at_edges++;
  }

  if (corners_at_edges == 4) {
    out_rect.x0 = min_x;
    out_rect.y0 = min_y;
    out_rect.x1 = max_x;
    out_rect.y1 = max_y;
    return true;
  }

  return false;
}

bool Blend2DBackend::push_with_clip(BLPath& path, bool fill, bool stroke) {
  if (!initialized_) return false;
  BLBox rect_box;
  const bool is_rect_path = is_rectangular_path(path, rect_box);

  // Apply clipping path if set
  if (state_.has_clip && !state_.clip_path.is_empty()) {
    ctx_.save();

    // Check if the clip path is a simple rectangle (common case in PDFs)
    BLBox clip_rect;
    if (is_rectangular_path(state_.clip_path, clip_rect)) {
      // Use precise rectangular clipping
      ctx_.clip_to_rect(clip_rect.x0, clip_rect.y0,
                        clip_rect.x1 - clip_rect.x0, clip_rect.y1 - clip_rect.y0);
    } else {
      // For non-rectangular paths, use bounding box approximation
      // Note: Blend2D doesn't support arbitrary path clipping, so this is
      // an approximation. For precise clipping, consider using ThorVG backend.
      BLBox clip_box;
      state_.clip_path.get_bounding_box(&clip_box);
      ctx_.clip_to_rect(clip_box.x0, clip_box.y0,
                        clip_box.x1 - clip_box.x0, clip_box.y1 - clip_box.y0);
    }
  }

  // Apply blend mode
  ctx_.set_comp_op(static_cast<BLCompOp>(state_.blend_mode));

  if (fill) {
    uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);
    ctx_.set_fill_style(BLRgba32(state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha));
    if (is_rect_path) {
      ctx_.fill_rect(rect_box.x0, rect_box.y0, rect_box.x1 - rect_box.x0,
                     rect_box.y1 - rect_box.y0);
    } else {
      ctx_.fill_path(path);
    }
  }

  if (stroke) {
    // Set stroke style
    ctx_.set_stroke_width(state_.stroke_width * state_.scale);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    ctx_.set_stroke_style(BLRgba32(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha));

    // Line cap
    BLStrokeCap cap = BL_STROKE_CAP_BUTT;
    if (state_.line_cap == 1) cap = BL_STROKE_CAP_ROUND;
    else if (state_.line_cap == 2) cap = BL_STROKE_CAP_SQUARE;
    ctx_.set_stroke_caps(cap);

    // Line join
    BLStrokeJoin join = BL_STROKE_JOIN_MITER_CLIP;
    if (state_.line_join == 1) join = BL_STROKE_JOIN_ROUND;
    else if (state_.line_join == 2) join = BL_STROKE_JOIN_BEVEL;
    ctx_.set_stroke_join(join);
    ctx_.set_stroke_miter_limit(state_.miter_limit);

    // Dash pattern
    if (!state_.dash_pattern.empty()) {
      BLArray<double> dashes;
      for (float d : state_.dash_pattern) {
        dashes.append(static_cast<double>(d * state_.scale));
      }
      ctx_.set_stroke_dash_array(dashes);
      ctx_.set_stroke_dash_offset(static_cast<double>(state_.dash_phase * state_.scale));
    }

    if (is_rect_path) {
      ctx_.stroke_rect(rect_box.x0, rect_box.y0, rect_box.x1 - rect_box.x0,
                       rect_box.y1 - rect_box.y0);
    } else {
      ctx_.stroke_path(path);
    }
  }

  if (state_.has_clip) {
    ctx_.restore();
  }

  return true;
}

Blend2DRenderResult Blend2DBackend::render_page(const Pdf& pdf, const Page& page) {
  Blend2DRenderResult result;

  // Get page dimensions
  float page_width = 612.0f;
  float page_height = 792.0f;

  if (page.media_box.size() >= 4) {
    page_width = static_cast<float>(page.media_box[2] - page.media_box[0]);
    page_height = static_cast<float>(page.media_box[3] - page.media_box[1]);
  }

  // Calculate scale to fit in canvas
  float scale_x = static_cast<float>(width_) / page_width;
  float scale_y = static_cast<float>(height_) / page_height;
  float scale = std::min(scale_x, scale_y);

  // Initialize graphics state
  state_ = GraphicsState();
  state_.page_width = page_width;
  state_.page_height = page_height;
  state_.scale = scale;

  current_pdf_ = &pdf;
  current_page_ = &page;

  // Ensure page fonts are loaded
  page.ensure_fonts_loaded(pdf);

  std::vector<std::vector<uint8_t>> decoded_contents;
  decoded_contents.reserve(page.contents.size());
  size_t total_render_objects = 0;

  for (const auto& content_obj : page.contents) {
    Value resolved_obj = content_obj;
    uint32_t obj_num = 0;
    uint16_t gen_num = 0;

    if (content_obj.type == Value::REFERENCE) {
      obj_num = content_obj.ref_object_number;
      gen_num = content_obj.ref_generation_number;
      auto resolved = resolve_reference(pdf, obj_num, gen_num);
      if (!resolved.success) {
        continue;
      }
      resolved_obj = std::move(resolved.value);
    }

    if (resolved_obj.type != Value::STREAM) {
      continue;
    }

    auto decoded_result = decode_stream(pdf, resolved_obj, obj_num, gen_num);
    if (!decoded_result.success) {
      continue;
    }

    total_render_objects += count_render_objects(decoded_result.data);
    total_render_objects += scan_do_extra_weight(
        std::string_view(
            reinterpret_cast<const char*>(decoded_result.data.data()),
            decoded_result.data.size()),
        pdf, page.resources);
    decoded_contents.push_back(std::move(decoded_result.data));
  }

  begin_progress(progress_config_.callback, total_render_objects,
                 progress_config_.object_threshold,
                 progress_config_.percent_step);

  // Clear canvas
  begin_scene();

  // Process each content stream
  for (const auto& decoded_content : decoded_contents) {
    state_ = GraphicsState();  // Reset state
    state_.page_width = page_width;
    state_.page_height = page_height;
    state_.scale = scale;
    parse_pdf_content(decoded_content);
  }

  end_scene();
  finish_progress();

  current_pdf_ = nullptr;
  current_page_ = nullptr;

  return get_buffer();
}

bool Blend2DBackend::parse_pdf_content(const std::vector<uint8_t>& content_data) {
  if (content_data.empty()) return true;

  std::string content(content_data.begin(), content_data.end());
  std::vector<std::string> operands;
  std::vector<GraphicsState> state_stack;
  operands.reserve(16);
  state_stack.reserve(16);

  size_t pos = 0;
  while (pos < content.length()) {
    // Skip whitespace
    while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.length()) break;

    // Check for comment
    if (content[pos] == '%') {
      while (pos < content.length() && content[pos] != '\n' && content[pos] != '\r') {
        pos++;
      }
      continue;
    }

    // Parse token
    std::string token;
    bool is_string = false;

    if (content[pos] == '(') {
      // String literal
      is_string = true;
      int paren_depth = 1;
      pos++;
      while (pos < content.length() && paren_depth > 0) {
        if (content[pos] == '\\' && pos + 1 < content.length()) {
          token += content[pos];
          pos++;
          token += content[pos];
          pos++;
        } else if (content[pos] == '(') {
          paren_depth++;
          token += content[pos];
          pos++;
        } else if (content[pos] == ')') {
          paren_depth--;
          if (paren_depth > 0) {
            token += content[pos];
          }
          pos++;
        } else {
          token += content[pos];
          pos++;
        }
      }
    } else if (content[pos] == '<') {
      if (pos + 1 < content.length() && content[pos + 1] == '<') {
        // Dictionary start
        token = "<<";
        pos += 2;
      } else {
        // Hex string - collect hex characters and decode to binary
        pos++;
        std::string hex_str;
        while (pos < content.length() && content[pos] != '>') {
          if (!std::isspace(static_cast<unsigned char>(content[pos]))) {
            hex_str += content[pos];
          }
          pos++;
        }
        if (pos < content.length()) pos++;

        // Decode hex string to binary bytes
        // If odd number of digits, pad with 0 at end
        if (hex_str.length() % 2 != 0) {
          hex_str += '0';
        }
        token.clear();
        for (size_t i = 0; i + 1 < hex_str.length(); i += 2) {
          char hex_byte[3] = {hex_str[i], hex_str[i + 1], 0};
          char* end;
          unsigned long val = strtoul(hex_byte, &end, 16);
          token += static_cast<char>(val);
        }
        is_string = true;
      }
    } else if (content[pos] == '>') {
      if (pos + 1 < content.length() && content[pos + 1] == '>') {
        token = ">>";
        pos += 2;
      } else {
        pos++;
      }
    } else if (content[pos] == '[') {
      token = "[";
      pos++;
    } else if (content[pos] == ']') {
      token = "]";
      pos++;
    } else if (content[pos] == '{' || content[pos] == '}') {
      // PostScript procedure delimiters - skip (not used in PDF content streams)
      pos++;
      continue;
    } else if (content[pos] == '/') {
      // Name
      token = "/";
      pos++;
      while (pos < content.length() &&
             !std::isspace(static_cast<unsigned char>(content[pos])) &&
             content[pos] != '/' && content[pos] != '[' &&
             content[pos] != ']' && content[pos] != '(' &&
             content[pos] != ')' && content[pos] != '<' &&
             content[pos] != '>' && content[pos] != '{' &&
             content[pos] != '}') {
        token += content[pos];
        pos++;
      }
    } else {
      // Number or operator
      while (pos < content.length() &&
             !std::isspace(static_cast<unsigned char>(content[pos])) &&
             content[pos] != '/' && content[pos] != '[' &&
             content[pos] != ']' && content[pos] != '(' &&
             content[pos] != ')' && content[pos] != '<' &&
             content[pos] != '>' && content[pos] != '{' &&
             content[pos] != '}') {
        token += content[pos];
        pos++;
      }
    }

    if (token.empty()) continue;

    // Check if token is an operator or operand
    bool is_operator = false;
    if (!is_string && !token.empty()) {
      char first = token[0];
      if (token == "<<" || token == ">>" || token == "[" || token == "]") {
        // These are structural, add as operands
      } else if ((first >= 'a' && first <= 'z') ||
                 (first >= 'A' && first <= 'Z') ||
                 first == '\'' || first == '"') {
        is_operator = true;
      }
    }

    if (is_operator) {
      // Process operator
      // Graphics state operators
      if (token == "q") {
        state_stack.push_back(state_);
        ctx_.save();
      } else if (token == "Q") {
        if (!state_stack.empty()) {
          state_ = state_stack.back();
          state_stack.pop_back();
          ctx_.restore();
        }
      }
      // Path construction
      else if (token == "m") {  // moveto
        if (operands.size() >= 2) {
          float x = nanopdf::stof_or(operands[operands.size() - 2]) * state_.scale;
          float y = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.move_to(x, y);
          state_.current_x = x;
          state_.current_y = y;
          state_.in_path = true;
        }
      } else if (token == "l") {  // lineto
        if (operands.size() >= 2) {
          float x = nanopdf::stof_or(operands[operands.size() - 2]) * state_.scale;
          float y = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.line_to(x, y);
          state_.current_x = x;
          state_.current_y = y;
        }
      } else if (token == "c") {  // curveto
        if (operands.size() >= 6) {
          float x1 = nanopdf::stof_or(operands[operands.size() - 6]) * state_.scale;
          float y1 = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 5])) * state_.scale;
          float x2 = nanopdf::stof_or(operands[operands.size() - 4]) * state_.scale;
          float y2 = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 3])) * state_.scale;
          float x3 = nanopdf::stof_or(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(x1, y1, x2, y2, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "v") {  // curveto (first control point = current point)
        if (operands.size() >= 4) {
          float x2 = nanopdf::stof_or(operands[operands.size() - 4]) * state_.scale;
          float y2 = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 3])) * state_.scale;
          float x3 = nanopdf::stof_or(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(state_.current_x, state_.current_y, x2, y2, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "y") {  // curveto (last control point = end point)
        if (operands.size() >= 4) {
          float x1 = nanopdf::stof_or(operands[operands.size() - 4]) * state_.scale;
          float y1 = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 3])) * state_.scale;
          float x3 = nanopdf::stof_or(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(x1, y1, x3, y3, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "h") {  // closepath
        state_.current_path.close();
      } else if (token == "re") {  // rectangle
        if (operands.size() >= 4) {
          float x = nanopdf::stof_or(operands[operands.size() - 4]) * state_.scale;
          float y = (state_.page_height - nanopdf::stof_or(operands[operands.size() - 3])) * state_.scale;
          float w = nanopdf::stof_or(operands[operands.size() - 2]) * state_.scale;
          float h = nanopdf::stof_or(operands[operands.size() - 1]) * state_.scale;
          // PDF y is bottom-up, adjust for top-down
          y -= h;
          state_.current_path.add_rect(x, y, w, h);
          state_.in_path = true;
        }
      }
      // Path painting
      else if (token == "f" || token == "F" || token == "f*") {
        if (state_.in_path) {
          BLFillRule rule = (token == "f*") ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          ctx_.set_fill_rule(rule);
          push_with_clip(state_.current_path, true, false);
          state_.current_path.reset();
          state_.in_path = false;
          advance_progress();
        }
      } else if (token == "S") {  // stroke
        if (state_.in_path) {
          push_with_clip(state_.current_path, false, true);
          state_.current_path.reset();
          state_.in_path = false;
          advance_progress();
        }
      } else if (token == "s") {  // close and stroke
        if (state_.in_path) {
          state_.current_path.close();
          push_with_clip(state_.current_path, false, true);
          state_.current_path.reset();
          state_.in_path = false;
          advance_progress();
        }
      } else if (token == "B" || token == "B*") {  // fill and stroke
        if (state_.in_path) {
          BLFillRule rule = (token == "B*") ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          ctx_.set_fill_rule(rule);
          push_with_clip(state_.current_path, true, true);
          state_.current_path.reset();
          state_.in_path = false;
          advance_progress();
        }
      } else if (token == "b" || token == "b*") {  // close, fill and stroke
        if (state_.in_path) {
          state_.current_path.close();
          BLFillRule rule = (token == "b*") ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          ctx_.set_fill_rule(rule);
          push_with_clip(state_.current_path, true, true);
          state_.current_path.reset();
          state_.in_path = false;
          advance_progress();
        }
      } else if (token == "n") {  // end path
        state_.current_path.reset();
        state_.in_path = false;
      }
      // Clipping
      else if (token == "W" || token == "W*") {
        if (state_.in_path) {
          state_.has_clip = true;
          state_.clip_even_odd = (token == "W*");
          state_.clip_path = state_.current_path;
        }
      }
      // Color operators
      else if (token == "g") {  // gray fill
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
          state_.fill_r = state_.fill_g = state_.fill_b = gray;
        }
      } else if (token == "G") {  // gray stroke
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
          state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
        }
      } else if (token == "rg") {  // RGB fill
        if (operands.size() >= 3) {
          state_.fill_r = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
          state_.fill_g = static_cast<uint8_t>(nanopdf::stof_or(operands[1]) * 255);
          state_.fill_b = static_cast<uint8_t>(nanopdf::stof_or(operands[2]) * 255);
        }
      } else if (token == "RG") {  // RGB stroke
        if (operands.size() >= 3) {
          state_.stroke_r = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
          state_.stroke_g = static_cast<uint8_t>(nanopdf::stof_or(operands[1]) * 255);
          state_.stroke_b = static_cast<uint8_t>(nanopdf::stof_or(operands[2]) * 255);
        }
      } else if (token == "k") {  // CMYK fill
        if (operands.size() >= 4) {
          float c = nanopdf::stof_or(operands[0]);
          float m = nanopdf::stof_or(operands[1]);
          float y = nanopdf::stof_or(operands[2]);
          float k = nanopdf::stof_or(operands[3]);
          state_.fill_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.fill_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.fill_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      } else if (token == "K") {  // CMYK stroke
        if (operands.size() >= 4) {
          float c = nanopdf::stof_or(operands[0]);
          float m = nanopdf::stof_or(operands[1]);
          float y = nanopdf::stof_or(operands[2]);
          float k = nanopdf::stof_or(operands[3]);
          state_.stroke_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.stroke_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.stroke_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      }
      // Color space
      else if (token == "cs") {
        if (operands.size() >= 1) {
          std::string cs_name = operands[0];
          if (!cs_name.empty() && cs_name[0] == '/') {
            cs_name = cs_name.substr(1);
          }
          state_.fill_color_space = cs_name;
          state_.fill_pattern.clear();
        }
      } else if (token == "CS") {
        if (operands.size() >= 1) {
          std::string cs_name = operands[0];
          if (!cs_name.empty() && cs_name[0] == '/') {
            cs_name = cs_name.substr(1);
          }
          state_.stroke_color_space = cs_name;
          state_.stroke_pattern.clear();
        }
      } else if (token == "sc" || token == "scn") {
        // Check for pattern
        bool is_pattern = false;
        if (!operands.empty()) {
          const std::string& last = operands.back();
          if (!last.empty() && last[0] == '/') {
            state_.fill_pattern = last.substr(1);
            is_pattern = true;
          }
        }
        if (!is_pattern) {
          state_.fill_pattern.clear();
          if (operands.size() >= 3) {
            state_.fill_r = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
            state_.fill_g = static_cast<uint8_t>(nanopdf::stof_or(operands[1]) * 255);
            state_.fill_b = static_cast<uint8_t>(nanopdf::stof_or(operands[2]) * 255);
          } else if (operands.size() >= 1) {
            // Check if current color space is Separation
            bool handled = false;
            if (!state_.fill_color_space.empty() && current_pdf_ && current_page_) {
              const Value* cs_value = lookup_resource("ColorSpace", state_.fill_color_space);
              if (cs_value) {
                Value resolved_cs = *cs_value;
                if (resolved_cs.type == Value::REFERENCE) {
                  auto resolved = resolve_reference(*current_pdf_, resolved_cs.ref_object_number,
                                                    resolved_cs.ref_generation_number);
                  if (resolved.success) {
                    resolved_cs = resolved.value;
                  }
                }
                ColorSpace cs = parse_color_space(*current_pdf_, resolved_cs);
                if (cs.type == ColorSpaceType::Separation) {
                  double tint = nanopdf::stof_or(operands[0]);
                  if (evaluate_separation_color(*current_pdf_, cs, tint,
                                                state_.fill_r, state_.fill_g, state_.fill_b)) {
                    handled = true;
                  }
                }
              }
            }
            if (!handled) {
              // Fallback: treat as grayscale
              uint8_t gray = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
              state_.fill_r = state_.fill_g = state_.fill_b = gray;
            }
          }
        }
      } else if (token == "SC" || token == "SCN") {
        bool is_pattern = false;
        if (!operands.empty()) {
          const std::string& last = operands.back();
          if (!last.empty() && last[0] == '/') {
            state_.stroke_pattern = last.substr(1);
            is_pattern = true;
          }
        }
        if (!is_pattern) {
          state_.stroke_pattern.clear();
          if (operands.size() >= 3) {
            state_.stroke_r = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
            state_.stroke_g = static_cast<uint8_t>(nanopdf::stof_or(operands[1]) * 255);
            state_.stroke_b = static_cast<uint8_t>(nanopdf::stof_or(operands[2]) * 255);
          } else if (operands.size() >= 1) {
            // Check if current stroke color space is Separation
            bool handled = false;
            if (!state_.stroke_color_space.empty() && current_pdf_ && current_page_) {
              const Value* cs_value = lookup_resource("ColorSpace", state_.stroke_color_space);
              if (cs_value) {
                Value resolved_cs = *cs_value;
                if (resolved_cs.type == Value::REFERENCE) {
                  auto resolved = resolve_reference(*current_pdf_, resolved_cs.ref_object_number,
                                                    resolved_cs.ref_generation_number);
                  if (resolved.success) {
                    resolved_cs = resolved.value;
                  }
                }
                ColorSpace cs = parse_color_space(*current_pdf_, resolved_cs);
                if (cs.type == ColorSpaceType::Separation) {
                  double tint = nanopdf::stof_or(operands[0]);
                  if (evaluate_separation_color(*current_pdf_, cs, tint,
                                                state_.stroke_r, state_.stroke_g, state_.stroke_b)) {
                    handled = true;
                  }
                }
              }
            }
            if (!handled) {
              // Fallback: treat as grayscale
              uint8_t gray = static_cast<uint8_t>(nanopdf::stof_or(operands[0]) * 255);
              state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
            }
          }
        }
      }
      // Line style
      else if (token == "w") {
        if (operands.size() >= 1) {
          state_.stroke_width = nanopdf::stof_or(operands[0]);
        }
      } else if (token == "J") {
        if (operands.size() >= 1) {
          state_.line_cap = nanopdf::stoi_or(operands[0]);
        }
      } else if (token == "j") {
        if (operands.size() >= 1) {
          state_.line_join = nanopdf::stoi_or(operands[0]);
        }
      } else if (token == "M") {
        if (operands.size() >= 1) {
          state_.miter_limit = nanopdf::stof_or(operands[0]);
        }
      } else if (token == "d") {
        state_.dash_pattern.clear();
        state_.dash_phase = 0.0f;
        bool in_array = false;
        for (size_t i = 0; i < operands.size(); i++) {
          if (operands[i] == "[") {
            in_array = true;
          } else if (operands[i] == "]") {
            in_array = false;
          } else if (in_array) {
            state_.dash_pattern.push_back(nanopdf::stof_or(operands[i]));
          } else if (!in_array && i == operands.size() - 1) {
            state_.dash_phase = nanopdf::stof_or(operands[i]);
          }
        }
      }
      // Graphics state dictionary (gs operator)
      else if (token == "gs") {
        if (operands.size() >= 1 && current_pdf_ && current_page_) {
          std::string gs_name = operands[0];
          if (!gs_name.empty() && gs_name[0] == '/') {
            gs_name = gs_name.substr(1);
          }
          // Look up ExtGState from resources (Form XObject stack first, then page)
          Dictionary extgstate_dict = lookup_resource_type_dict("ExtGState");
          if (!extgstate_dict.empty()) {
            // Find the specific graphics state
            auto gs_it = extgstate_dict.find(gs_name);
            if (gs_it != extgstate_dict.end()) {
              Dictionary gs_dict;
              if (gs_it->second.type == Value::DICTIONARY) {
                gs_dict = gs_it->second.dict;
              } else if (gs_it->second.type == Value::REFERENCE) {
                auto resolved = resolve_reference(*current_pdf_,
                                                  gs_it->second.ref_object_number,
                                                  gs_it->second.ref_generation_number);
                if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                  gs_dict = resolved.value.dict;
                }
              }
              // Apply graphics state parameters
              // ca - non-stroking alpha (fill)
              auto ca_it = gs_dict.find("ca");
              if (ca_it != gs_dict.end() && ca_it->second.type == Value::NUMBER) {
                state_.fill_opacity = static_cast<float>(ca_it->second.number);
              }
              // CA - stroking alpha
              auto CA_it = gs_dict.find("CA");
              if (CA_it != gs_dict.end() && CA_it->second.type == Value::NUMBER) {
                state_.stroke_opacity = static_cast<float>(CA_it->second.number);
              }
              // BM - blend mode
              auto bm_it = gs_dict.find("BM");
              if (bm_it != gs_dict.end()) {
                std::string bm_name;
                if (bm_it->second.type == Value::NAME) {
                  bm_name = bm_it->second.name;
                } else if (bm_it->second.type == Value::ARRAY && !bm_it->second.array.empty()) {
                  if (bm_it->second.array[0].type == Value::NAME) {
                    bm_name = bm_it->second.array[0].name;
                  }
                }
                // Map PDF blend mode to Blend2D comp op
                if (bm_name == "Normal" || bm_name == "Compatible") {
                  state_.blend_mode = BL_COMP_OP_SRC_OVER;
                } else if (bm_name == "Multiply") {
                  state_.blend_mode = BL_COMP_OP_MULTIPLY;
                } else if (bm_name == "Screen") {
                  state_.blend_mode = BL_COMP_OP_SCREEN;
                } else if (bm_name == "Overlay") {
                  state_.blend_mode = BL_COMP_OP_OVERLAY;
                } else if (bm_name == "Darken") {
                  state_.blend_mode = BL_COMP_OP_DARKEN;
                } else if (bm_name == "Lighten") {
                  state_.blend_mode = BL_COMP_OP_LIGHTEN;
                } else if (bm_name == "ColorDodge") {
                  state_.blend_mode = BL_COMP_OP_COLOR_DODGE;
                } else if (bm_name == "ColorBurn") {
                  state_.blend_mode = BL_COMP_OP_COLOR_BURN;
                } else if (bm_name == "HardLight") {
                  state_.blend_mode = BL_COMP_OP_HARD_LIGHT;
                } else if (bm_name == "SoftLight") {
                  state_.blend_mode = BL_COMP_OP_SOFT_LIGHT;
                } else if (bm_name == "Difference") {
                  state_.blend_mode = BL_COMP_OP_DIFFERENCE;
                } else if (bm_name == "Exclusion") {
                  state_.blend_mode = BL_COMP_OP_EXCLUSION;
                } else if (bm_name == "Hue" || bm_name == "Saturation" ||
                           bm_name == "Color" || bm_name == "Luminosity") {
                  // Non-separable blend modes require HSL compositing which
                  // Blend2D doesn't support natively. Falling back to Normal.
#ifdef NANOPDF_DEBUG_PRINT
                  printf("DEBUG: Non-separable blend mode '%s' not supported by Blend2D, "
                         "falling back to Normal\n", bm_name.c_str());
#endif
                  state_.blend_mode = BL_COMP_OP_SRC_OVER;
                } else {
                  state_.blend_mode = BL_COMP_OP_SRC_OVER;  // Default
                }
                ctx_.set_comp_op(static_cast<BLCompOp>(state_.blend_mode));
              }
              // LW - line width
              auto lw_it = gs_dict.find("LW");
              if (lw_it != gs_dict.end() && lw_it->second.type == Value::NUMBER) {
                state_.stroke_width = static_cast<float>(lw_it->second.number);
              }
              // LC - line cap
              auto lc_it = gs_dict.find("LC");
              if (lc_it != gs_dict.end() && lc_it->second.type == Value::NUMBER) {
                state_.line_cap = static_cast<int>(lc_it->second.number);
              }
              // LJ - line join
              auto lj_it = gs_dict.find("LJ");
              if (lj_it != gs_dict.end() && lj_it->second.type == Value::NUMBER) {
                state_.line_join = static_cast<int>(lj_it->second.number);
              }
              // ML - miter limit
              auto ml_it = gs_dict.find("ML");
              if (ml_it != gs_dict.end() && ml_it->second.type == Value::NUMBER) {
                state_.miter_limit = static_cast<float>(ml_it->second.number);
              }
              // AIS - alpha is shape
              auto ais_it = gs_dict.find("AIS");
              if (ais_it != gs_dict.end() && ais_it->second.type == Value::BOOLEAN) {
                state_.alpha_is_shape = ais_it->second.boolean;
              }
              // TK - text knockout
              auto tk_it = gs_dict.find("TK");
              if (tk_it != gs_dict.end() && tk_it->second.type == Value::BOOLEAN) {
                state_.text_knockout = tk_it->second.boolean;
              }
              // OP - overprint for stroking
              auto op_stroke_it = gs_dict.find("OP");
              if (op_stroke_it != gs_dict.end() && op_stroke_it->second.type == Value::BOOLEAN) {
                state_.overprint_stroke = op_stroke_it->second.boolean;
              }
              // op - overprint for non-stroking (fill)
              auto op_fill_it = gs_dict.find("op");
              if (op_fill_it != gs_dict.end() && op_fill_it->second.type == Value::BOOLEAN) {
                state_.overprint_fill = op_fill_it->second.boolean;
              }
              // OPM - overprint mode
              auto opm_it = gs_dict.find("OPM");
              if (opm_it != gs_dict.end() && opm_it->second.type == Value::NUMBER) {
                state_.overprint_mode = static_cast<int>(opm_it->second.number);
              }
              // FL - flatness tolerance
              auto fl_it = gs_dict.find("FL");
              if (fl_it != gs_dict.end() && fl_it->second.type == Value::NUMBER) {
                state_.flatness = static_cast<float>(fl_it->second.number);
              }
              // SA - stroke adjustment
              auto sa_it = gs_dict.find("SA");
              if (sa_it != gs_dict.end() && sa_it->second.type == Value::BOOLEAN) {
                state_.stroke_adjustment = sa_it->second.boolean;
              }
              // SMask - soft mask
              auto smask_it = gs_dict.find("SMask");
              if (smask_it != gs_dict.end()) {
                if (smask_it->second.type == Value::NAME && smask_it->second.name == "None") {
                  // Clear soft mask
                  state_.has_soft_mask = false;
                  state_.soft_mask_type = 0;
                  state_.soft_mask_data.clear();
                } else if (smask_it->second.type == Value::DICTIONARY ||
                           smask_it->second.type == Value::REFERENCE) {
                  // Parse soft mask dictionary
                  Dictionary smask_dict;
                  if (smask_it->second.type == Value::DICTIONARY) {
                    smask_dict = smask_it->second.dict;
                  } else {
                    auto resolved = resolve_reference(*current_pdf_,
                                                      smask_it->second.ref_object_number,
                                                      smask_it->second.ref_generation_number);
                    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                      smask_dict = resolved.value.dict;
                    }
                  }
                  if (!smask_dict.empty()) {
                    state_.has_soft_mask = true;
                    // Get soft mask type (S entry)
                    auto s_it = smask_dict.find("S");
                    if (s_it != smask_dict.end() && s_it->second.type == Value::NAME) {
                      if (s_it->second.name == "Alpha") {
                        state_.soft_mask_type = 1;
                      } else if (s_it->second.name == "Luminosity") {
                        state_.soft_mask_type = 2;
                      }
                    }
                    // Get and render the G (transparency group) XObject
                    auto g_it = smask_dict.find("G");
                    if (g_it != smask_dict.end()) {
                      render_soft_mask_group(g_it->second, state_.soft_mask_type);
                    }
                  }
                }
              }
            }
          }
        }
      }
      // Text operators
      else if (token == "BT") {
        state_.in_text_block = true;
        state_.text_matrix.reset();
        state_.text_line_matrix.reset();
      } else if (token == "ET") {
        state_.in_text_block = false;
        // Merge accumulated text clip into main clip path
        if (state_.text_clip_active && !state_.text_clip_path.is_empty()) {
          state_.clip_path.add_path(state_.text_clip_path);
          state_.has_clip = true;
          state_.text_clip_path.reset();
          state_.text_clip_active = false;
        }
      } else if (token == "Tf") {
        if (operands.size() >= 2) {
          std::string font_name = operands[0];
          if (!font_name.empty() && font_name[0] == '/') {
            font_name = font_name.substr(1);
          }
          state_.font_size = nanopdf::stof_or(operands[operands.size() - 1]);
          current_font_name_ = font_name;
          current_font_ = nullptr;
          if (current_pdf_ && current_page_) {
            auto font_it = current_page_->fonts.find(font_name);
            if (font_it != current_page_->fonts.end()) {
              current_font_ = font_it->second.get();
              load_font(*current_pdf_, font_name, current_font_);
            } else {
              // Font not in page dictionary — attempt fallback using name hint
              NANOPDF_LOG_DEBUG("Blend2D", "Font '%s' not in page resources, trying fallback", font_name.c_str());
              load_fallback_font_with_hint(font_name, nullptr);
            }
          }
        }
      } else if (token == "Td") {
        if (operands.size() >= 2) {
          float tx = nanopdf::stof_or(operands[0]);
          float ty = nanopdf::stof_or(operands[1]);
          state_.text_matrix.e += tx * state_.text_matrix.a + ty * state_.text_matrix.c;
          state_.text_matrix.f += tx * state_.text_matrix.b + ty * state_.text_matrix.d;
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "TD") {
        if (operands.size() >= 2) {
          float tx = nanopdf::stof_or(operands[0]);
          float ty = nanopdf::stof_or(operands[1]);
          state_.text_leading = -ty;
          state_.text_matrix.e += tx * state_.text_matrix.a + ty * state_.text_matrix.c;
          state_.text_matrix.f += tx * state_.text_matrix.b + ty * state_.text_matrix.d;
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "Tm") {
        if (operands.size() >= 6) {
          state_.text_matrix.a = nanopdf::stof_or(operands[0]);
          state_.text_matrix.b = nanopdf::stof_or(operands[1]);
          state_.text_matrix.c = nanopdf::stof_or(operands[2]);
          state_.text_matrix.d = nanopdf::stof_or(operands[3]);
          state_.text_matrix.e = nanopdf::stof_or(operands[4]);
          state_.text_matrix.f = nanopdf::stof_or(operands[5]);
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "T*") {
        // Equivalent to: 0 -TL Td
        float leading = state_.text_leading;
        if (leading == 0) leading = state_.font_size;
        state_.text_matrix.e = state_.text_line_matrix.e + (-leading) * state_.text_line_matrix.c;
        state_.text_matrix.f = state_.text_line_matrix.f + (-leading) * state_.text_line_matrix.d;
        state_.text_line_matrix = state_.text_matrix;
      } else if (token == "TL") {
        if (operands.size() >= 1) {
          state_.text_leading = nanopdf::stof_or(operands[0]);
        }
      } else if (token == "Tc") {
        if (operands.size() >= 1) {
          state_.char_spacing = nanopdf::stof_or(operands[0]);
        }
      } else if (token == "Tw") {
        if (operands.size() >= 1) {
          state_.word_spacing = nanopdf::stof_or(operands[0]);
        }
      } else if (token == "Tz") {
        if (operands.size() >= 1) {
          state_.horiz_scaling = nanopdf::stof_or(operands[0]);
        }
      } else if (token == "Ts") {
        if (operands.size() >= 1) {
          state_.text_rise = nanopdf::stof_or(operands[0]);
        }
      } else if (token == "Tr") {
        if (operands.size() >= 1) {
          state_.text_render_mode = nanopdf::stoi_or(operands[0]);
        }
      } else if (token == "Tj") {
        // Show text with CMap-aware rendering
        if (!operands.empty() && state_.in_text_block) {
          std::string text = operands[0];
          float x = state_.text_matrix.e * state_.scale;
          float y = (state_.page_height - state_.text_matrix.f) * state_.scale;
          // Text matrix a component affects font scaling
          float effective_font_size = state_.font_size * state_.text_matrix.a;
          float font_size = effective_font_size * state_.scale;

          uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);

          float text_width = render_text_string(text, x, y, font_size,
              state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);

          // Update text matrix with actual rendered width (convert back to text space)
          state_.text_matrix.e += text_width / state_.scale;
          advance_progress();
        }
      } else if (token == "TJ") {
        // Show text with positioning (CMap-aware)
        if (state_.in_text_block) {
          float x = state_.text_matrix.e * state_.scale;
          float y = (state_.page_height - state_.text_matrix.f) * state_.scale;
          // Text matrix a component affects font scaling
          float effective_font_size = state_.font_size * state_.text_matrix.a;
          float font_size = effective_font_size * state_.scale;
          uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);

          float total_advance = 0.0f;
          for (const auto& op : operands) {
            if (op == "[" || op == "]") continue;

            float adjust = nanopdf::stof_or(op);
            // Negative values move right, positive values move left
              float pixel_adjust = (adjust / 1000.0f) * font_size;
            x -= pixel_adjust;
            total_advance -= pixel_adjust / state_.scale;
          }
          state_.text_matrix.e += total_advance;
          advance_progress();
        }
      } else if (token == "'" || token == "\"") {
        // ' = T* + Tj (move to next line, show text)
        // " = set Tw, Tc, then T* + Tj
        if (state_.in_text_block) {
          size_t text_idx = operands.size() - 1;
          if (token == "\"" && operands.size() >= 3) {
            state_.word_spacing = nanopdf::stof_or(operands[0]);
            state_.char_spacing = nanopdf::stof_or(operands[1]);
          }

          // T* - move to next line
          state_.text_matrix.e = state_.text_line_matrix.e;
          state_.text_matrix.f = state_.text_line_matrix.f - state_.text_leading;
          state_.text_line_matrix = state_.text_matrix;

          // Tj - show text
          if (!operands.empty()) {
            std::string text = operands[text_idx];
            float x = state_.text_matrix.e * state_.scale;
            float y = (state_.page_height - state_.text_matrix.f) * state_.scale;
            float effective_font_size = state_.font_size * state_.text_matrix.a;
            float font_size = effective_font_size * state_.scale;
            uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);

            float text_width = render_text_string(text, x, y, font_size,
                state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
            state_.text_matrix.e += text_width / state_.scale;
            advance_progress();
          }
        }
      }
      // Shading
      else if (token == "sh") {
        if (operands.size() >= 1) {
          std::string shading_name = operands[0];
          if (!shading_name.empty() && shading_name[0] == '/') {
            shading_name = shading_name.substr(1);
          }
          draw_shading(shading_name);
          advance_progress();
        }
      }
      // Inline image (BI ... ID data EI)
      else if (token == "BI") {
        parse_inline_image(content, pos);
        advance_progress();
        operands.clear();
        continue;  // Skip normal operand clearing
      }
      // XObject (Do operator for images/forms)
      else if (token == "Do") {
        if (operands.size() >= 1 && current_pdf_ && current_page_) {
          bool rendered_xobject = false;
          std::string xobj_name = operands[0];
          if (!xobj_name.empty() && xobj_name[0] == '/') {
            xobj_name = xobj_name.substr(1);
          }

          // Look up XObject from resources (Form XObject stack first, then page)
          Dictionary xobj_dict = lookup_resource_type_dict("XObject");
          if (!xobj_dict.empty()) {
            auto entry_it = xobj_dict.find(xobj_name);
            if (entry_it != xobj_dict.end()) {
              Value xobj_value;
              uint32_t xobj_obj_num = 0;
              uint16_t xobj_gen_num = 0;
              if (entry_it->second.type == Value::REFERENCE) {
                xobj_obj_num = entry_it->second.ref_object_number;
                xobj_gen_num = entry_it->second.ref_generation_number;
                auto resolved = resolve_reference(*current_pdf_,
                    xobj_obj_num, xobj_gen_num);
                if (resolved.success) {
                  xobj_value = std::move(resolved.value);
                }
              } else {
                xobj_value = entry_it->second;
              }

              if (xobj_value.type == Value::STREAM) {
                auto subtype_it = xobj_value.stream.dict.find("Subtype");
                if (subtype_it != xobj_value.stream.dict.end() &&
                    subtype_it->second.type == Value::NAME) {
                  if (subtype_it->second.name == "Image") {
                    // Cache key: same (obj_num, obj_gen) always produces
                    // the same decoded + color-converted pixels.
                    std::string img_cache_key = "img:" + std::to_string(xobj_obj_num) + ":" +
                                                std::to_string(xobj_gen_num);
                    RenderCacheEntry img_cached;
                    if (xobj_obj_num != 0 &&
                        RenderCache::instance().find(img_cache_key, img_cached) &&
                        img_cached.width > 0 && img_cached.height > 0) {
                      // Cache hit: use pre-converted ARGB pixels directly.
                      ImageXObject image;
                      image.width = static_cast<int>(img_cached.width);
                      image.height = static_cast<int>(img_cached.height);
                      image.data.resize(img_cached.data.size());
                      memcpy(image.data.data(), img_cached.data.data(),
                             img_cached.data.size());
                      image.color_space.type = ColorSpaceType::CacheARGB;
                      float img_x = state_.transform.e * state_.scale;
                      float img_y = state_.transform.f;
                      float img_w = state_.transform.a * state_.scale;
                      float img_h = state_.transform.d * state_.scale;
                      if (img_h < 0) { img_y += img_h; img_h = -img_h; }
                      img_y = (state_.page_height - img_y) * state_.scale - img_h;
                      draw_image(image, img_x, img_y, img_w, img_h,
                          state_.fill_r, state_.fill_g, state_.fill_b);
                    } else {
                      ImageXObject image = parse_image_xobject(*current_pdf_, xobj_value,
                          xobj_obj_num, xobj_gen_num);
                      float img_x = state_.transform.e * state_.scale;
                      float img_y = state_.transform.f;
                      float img_width = state_.transform.a * state_.scale;
                      float img_height = state_.transform.d * state_.scale;
                      if (img_height < 0) {
                        img_y += img_height;
                        img_height = -img_height;
                      }
                      img_y = (state_.page_height - img_y) * state_.scale - img_height;
                      draw_image(image, img_x, img_y, img_width, img_height,
                          state_.fill_r, state_.fill_g, state_.fill_b);
                      // Cache the result ARGB for future use.
                      if (xobj_obj_num != 0 && !last_image_argb_.empty()) {
                        size_t n = static_cast<size_t>(image.width) *
                                   static_cast<size_t>(image.height);
                        RenderCacheEntry entry;
                        entry.width = static_cast<uint32_t>(image.width);
                        entry.height = static_cast<uint32_t>(image.height);
                        entry.data.resize(n * sizeof(uint32_t));
                        memcpy(entry.data.data(), last_image_argb_.data(),
                               n * sizeof(uint32_t));
                        RenderCache::instance().store(img_cache_key, std::move(entry));
                        last_image_argb_.clear();
                      }
                    }
                    rendered_xobject = true;
                  } else if (subtype_it->second.name == "Form") {
                    // Form XObject - decode and parse its content stream
                    auto decoded = decode_stream(*current_pdf_, xobj_value,
                        xobj_obj_num, xobj_gen_num);
                    if (decoded.success && !decoded.data.empty()) {
                      GraphicsState saved_state = state_;

                      // Check for Form's Resources and push onto stack
                      auto resources_it = xobj_value.stream.dict.find("Resources");
                      bool has_form_resources = false;
                      if (resources_it != xobj_value.stream.dict.end()) {
                        Value form_resources = resources_it->second;
                        // Resolve reference if needed
                        if (form_resources.type == Value::REFERENCE) {
                          auto resolved = resolve_reference(*current_pdf_,
                              form_resources.ref_object_number,
                              form_resources.ref_generation_number);
                          if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                            form_resources_stack_.push_back(resolved.value.dict);
                            has_form_resources = true;
                          }
                        } else if (form_resources.type == Value::DICTIONARY) {
                          form_resources_stack_.push_back(form_resources.dict);
                          has_form_resources = true;
                        }
                      }

                      auto matrix_it = xobj_value.stream.dict.find("Matrix");
                      if (matrix_it != xobj_value.stream.dict.end() &&
                          matrix_it->second.type == Value::ARRAY &&
                          matrix_it->second.array.size() >= 6) {
                        GraphicsState::Matrix form_matrix;
                        form_matrix.a = static_cast<float>(matrix_it->second.array[0].number);
                        form_matrix.b = static_cast<float>(matrix_it->second.array[1].number);
                        form_matrix.c = static_cast<float>(matrix_it->second.array[2].number);
                        form_matrix.d = static_cast<float>(matrix_it->second.array[3].number);
                        form_matrix.e = static_cast<float>(matrix_it->second.array[4].number);
                        form_matrix.f = static_cast<float>(matrix_it->second.array[5].number);
                        state_.transform = state_.transform * form_matrix;
                      }

                      // Apply BBox clipping if present
                      bool has_bbox_clip = false;
                      auto bbox_it = xobj_value.stream.dict.find("BBox");
                      if (bbox_it != xobj_value.stream.dict.end() &&
                          bbox_it->second.type == Value::ARRAY &&
                          bbox_it->second.array.size() >= 4) {
                        float bx0 = static_cast<float>(bbox_it->second.array[0].number);
                        float by0 = static_cast<float>(bbox_it->second.array[1].number);
                        float bx1 = static_cast<float>(bbox_it->second.array[2].number);
                        float by1 = static_cast<float>(bbox_it->second.array[3].number);

                        // Transform BBox corners through the current CTM
                        float x0 = bx0, y0 = by0;
                        float x1 = bx1, y1 = by1;
                        state_.transform.transform(x0, y0);
                        state_.transform.transform(x1, y1);

                        // Convert to screen coordinates (Y-flip) and scale
                        float sx0 = x0 * state_.scale;
                        float sy0 = (state_.page_height - y0) * state_.scale;
                        float sx1 = x1 * state_.scale;
                        float sy1 = (state_.page_height - y1) * state_.scale;

                        // Normalize to get min/max
                        float clip_x = std::min(sx0, sx1);
                        float clip_y = std::min(sy0, sy1);
                        float clip_w = std::abs(sx1 - sx0);
                        float clip_h = std::abs(sy1 - sy0);

                        if (clip_w > 0 && clip_h > 0) {
                          ctx_.save();
                          ctx_.clip_to_rect(clip_x, clip_y, clip_w, clip_h);
                          has_bbox_clip = true;
                        }
                      }

                      // Parse and render Form content — sub-operators advance progress
                      // naturally, so no progress guard or form-level advance.
                      parse_pdf_content(decoded.data);
                      rendered_xobject = true;

                      // Restore BBox clipping
                      if (has_bbox_clip) {
                        ctx_.restore();
                      }

                      // Pop Form resources from stack
                      if (has_form_resources) {
                        form_resources_stack_.pop_back();
                      }

                      state_ = saved_state;
                    }
                  }
                }
              }
            }
          }
          if (rendered_xobject) {
            // Image: advance by pixel-area weight (pre-counted in scan_do_extra_weight).
            // Form: sub-operators advance progress naturally, no extra advance here.
            if (subtype_it->second.name == "Image") {
              int w = 0, h = 0;
              auto w_it = xobj_value.stream.dict.find("Width");
              auto h_it = xobj_value.stream.dict.find("Height");
              if (w_it != xobj_value.stream.dict.end() &&
                  w_it->second.type == Value::NUMBER)
                w = static_cast<int>(w_it->second.number);
              if (h_it != xobj_value.stream.dict.end() &&
                  h_it->second.type == Value::NUMBER)
                h = static_cast<int>(h_it->second.number);
              size_t img_weight = std::max<size_t>(1,
                  (static_cast<size_t>(w) * h) / 10000);
              advance_progress(img_weight);
            }
            // Form: no advance_progress() — sub-operators advance naturally.
          }
        }
      }
      // Transformation
      else if (token == "cm") {
        if (operands.size() >= 6) {
          GraphicsState::Matrix m;
          m.a = nanopdf::stof_or(operands[0]);
          m.b = nanopdf::stof_or(operands[1]);
          m.c = nanopdf::stof_or(operands[2]);
          m.d = nanopdf::stof_or(operands[3]);
          m.e = nanopdf::stof_or(operands[4]);
          m.f = nanopdf::stof_or(operands[5]);
          state_.transform = state_.transform * m;
        }
      } else if (token == "ri") {
        // Rendering intent
        if (!operands.empty()) {
          state_.rendering_intent = operands[0];
        }
      } else if (token == "i") {
        // Flatness tolerance from content stream
        if (!operands.empty()) {
          state_.flatness = nanopdf::stof_or(operands[0]);
        }
      }

      operands.clear();
    } else {
      // Add as operand
      if (is_string) {
        operands.push_back(token);
      } else {
        operands.push_back(token);
      }
    }
  }

  return true;
}

bool Blend2DBackend::parse_inline_image(const std::string& content, size_t& pos) {
  // Parse inline image dictionary (BI ... ID data EI)
  // pos should be right after "BI"

  std::map<std::string, std::string> dict;

  // Skip whitespace after BI
  while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
    pos++;
  }

  // Parse dictionary entries until we hit "ID"
  while (pos < content.length()) {
    // Skip whitespace
    while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }
    if (pos >= content.length()) break;

    // Check for ID (marks start of image data)
    if (content[pos] == 'I' && pos + 1 < content.length() && content[pos + 1] == 'D') {
      pos += 2;  // Skip "ID"
      // Skip single whitespace after ID
      if (pos < content.length() && (content[pos] == ' ' || content[pos] == '\n' || content[pos] == '\r')) {
        pos++;
      }
      break;
    }

    // Parse key (should start with /)
    std::string key;
    if (content[pos] == '/') {
      pos++;  // Skip /
      while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
             content[pos] != '/' && content[pos] != '[' &&
             content[pos] != ']' && content[pos] != '(' &&
             content[pos] != ')' && content[pos] != '<' &&
             content[pos] != '>' && content[pos] != '{' &&
             content[pos] != '}') {
        key += content[pos++];
      }
    } else {
      // Skip unknown token
      while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos]))) {
        pos++;
      }
      continue;
    }

    // Skip whitespace
    while (pos < content.length() && std::isspace(static_cast<unsigned char>(content[pos]))) {
      pos++;
    }

    // Parse value
    std::string value;
    if (pos < content.length()) {
      if (content[pos] == '/') {
        pos++;  // Skip /
        while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
               content[pos] != '/' && content[pos] != '[' &&
               content[pos] != ']' && content[pos] != '(' &&
               content[pos] != ')' && content[pos] != '<' &&
               content[pos] != '>' && content[pos] != '{' &&
               content[pos] != '}') {
          value += content[pos++];
        }
      } else if (content[pos] == '[') {
        // Array value
        value += content[pos++];
        while (pos < content.length() && content[pos] != ']') {
          value += content[pos++];
        }
        if (pos < content.length()) value += content[pos++];
      } else {
        // Numeric or other value
        while (pos < content.length() && !std::isspace(static_cast<unsigned char>(content[pos])) &&
               content[pos] != '/' && content[pos] != '[' &&
               content[pos] != ']' && content[pos] != '(' &&
               content[pos] != ')' && content[pos] != '<' &&
               content[pos] != '>' && content[pos] != '{' &&
               content[pos] != '}') {
          value += content[pos++];
        }
      }
    }

    if (!key.empty()) {
      dict[key] = value;
    }
  }

  // Get image properties using PDF abbreviations
  int width = 0, height = 0, bpc = 8;
  std::string cs = "G";  // Default grayscale
  std::string filter;

  // W or Width
  auto it = dict.find("W");
  if (it == dict.end()) it = dict.find("Width");
  if (it != dict.end()) width = nanopdf::stoi_or(it->second);

  // H or Height
  it = dict.find("H");
  if (it == dict.end()) it = dict.find("Height");
  if (it != dict.end()) height = nanopdf::stoi_or(it->second);

  // BPC or BitsPerComponent
  it = dict.find("BPC");
  if (it == dict.end()) it = dict.find("BitsPerComponent");
  if (it != dict.end()) bpc = nanopdf::stoi_or(it->second);

  // CS or ColorSpace
  it = dict.find("CS");
  if (it == dict.end()) it = dict.find("ColorSpace");
  if (it != dict.end()) cs = it->second;

  // F or Filter
  it = dict.find("F");
  if (it == dict.end()) it = dict.find("Filter");
  if (it != dict.end()) filter = it->second;

  if (width <= 0 || height <= 0) {
    // Skip to EI and return
    while (pos < content.length()) {
      if (content[pos] == 'E' && pos + 1 < content.length() && content[pos + 1] == 'I') {
        pos += 2;
        return false;
      }
      pos++;
    }
    return false;
  }

  // Determine components based on color space
  int components = 1;
  if (cs == "RGB" || cs == "DeviceRGB") components = 3;
  else if (cs == "CMYK" || cs == "DeviceCMYK") components = 4;
  else if (cs == "G" || cs == "DeviceGray") components = 1;

  // Calculate expected data size
  int row_bytes = (width * components * bpc + 7) / 8;
  int expected_size = row_bytes * height;

  // Read raw image data until EI
  std::vector<uint8_t> raw_data;
  raw_data.reserve(expected_size);

  size_t data_start = pos;
  while (pos < content.length()) {
    // Look for EI preceded by whitespace
    if (pos > data_start &&
        (content[pos - 1] == ' ' || content[pos - 1] == '\n' || content[pos - 1] == '\r') &&
        content[pos] == 'E' && pos + 1 < content.length() && content[pos + 1] == 'I') {
      break;
    }
    raw_data.push_back(static_cast<uint8_t>(content[pos]));
    pos++;
  }

  // Skip EI
  if (pos < content.length() && content[pos] == 'E') {
    pos += 2;  // Skip "EI"
  }

  // Decode if filtered
  std::vector<uint8_t> decoded_data;
  if (filter == "AHx" || filter == "ASCIIHexDecode") {
    // ASCII Hex decode
    decoded_data.reserve(raw_data.size() / 2);
    for (size_t i = 0; i + 1 < raw_data.size(); i += 2) {
      char hex[3] = {static_cast<char>(raw_data[i]), static_cast<char>(raw_data[i + 1]), 0};
      decoded_data.push_back(static_cast<uint8_t>(strtol(hex, nullptr, 16)));
    }
  } else if (filter == "A85" || filter == "ASCII85Decode") {
    // ASCII85 decode
    decoded_data.reserve(raw_data.size() * 4 / 5);
    uint32_t value = 0;
    int count = 0;
    for (size_t i = 0; i < raw_data.size(); ++i) {
      uint8_t c = raw_data[i];
      // Skip whitespace
      if (c <= ' ') continue;
      // Handle 'z' (represents four zero bytes)
      if (c == 'z' && count == 0) {
        decoded_data.push_back(0);
        decoded_data.push_back(0);
        decoded_data.push_back(0);
        decoded_data.push_back(0);
        continue;
      }
      // Check for end marker ~>
      if (c == '~') break;
      // Validate character range
      if (c < '!' || c > 'u') continue;
      // Accumulate value
      value = value * 85 + (c - '!');
      count++;
      if (count == 5) {
        decoded_data.push_back((value >> 24) & 0xFF);
        decoded_data.push_back((value >> 16) & 0xFF);
        decoded_data.push_back((value >> 8) & 0xFF);
        decoded_data.push_back(value & 0xFF);
        value = 0;
        count = 0;
      }
    }
    // Handle remaining bytes (partial group)
    if (count > 0) {
      for (int j = count; j < 5; j++) {
        value = value * 85 + 84;  // Pad with 'u'
      }
      if (count > 1) decoded_data.push_back((value >> 24) & 0xFF);
      if (count > 2) decoded_data.push_back((value >> 16) & 0xFF);
      if (count > 3) decoded_data.push_back((value >> 8) & 0xFF);
    }
  } else if (filter == "Fl" || filter == "FlateDecode" || filter == "LZW" || filter == "LZWDecode") {
    // Need to decompress - use nanopdf's decode functions
    if (current_pdf_) {
      // Create a temporary stream value for decoding
      Value stream_val;
      stream_val.type = Value::STREAM;
      stream_val.stream.data = raw_data;
      if (filter == "Fl" || filter == "FlateDecode") {
        stream_val.stream.dict["Filter"] = Value();
        stream_val.stream.dict["Filter"].type = Value::NAME;
        stream_val.stream.dict["Filter"].name = "FlateDecode";
      } else {
        stream_val.stream.dict["Filter"] = Value();
        stream_val.stream.dict["Filter"].type = Value::NAME;
        stream_val.stream.dict["Filter"].name = "LZWDecode";
      }
      auto result = decode_stream(*current_pdf_, stream_val);
      if (result.success) {
        decoded_data = result.data;
      } else {
        decoded_data = raw_data;
      }
    } else {
      decoded_data = raw_data;
    }
  } else {
    // No filter or unknown, use raw data
    decoded_data = raw_data;
  }

  // Build ImageXObject
  ImageXObject image;
  image.width = width;
  image.height = height;
  image.bits_per_component = bpc;
  image.data = decoded_data;

  if (cs == "RGB" || cs == "DeviceRGB") {
    image.color_space.type = ColorSpaceType::DeviceRGB;
  } else if (cs == "CMYK" || cs == "DeviceCMYK") {
    image.color_space.type = ColorSpaceType::DeviceCMYK;
  } else {
    image.color_space.type = ColorSpaceType::DeviceGray;
  }

  // Draw the image using CTM
  float img_x = state_.transform.e * state_.scale;
  float img_y = state_.transform.f;
  float img_width = state_.transform.a * state_.scale;
  float img_height = state_.transform.d * state_.scale;

  if (img_height < 0) {
    img_y += img_height;
    img_height = -img_height;
  }
  img_y = (state_.page_height - img_y) * state_.scale - img_height;

  draw_image(image, img_x, img_y, img_width, img_height,
                        state_.fill_r, state_.fill_g, state_.fill_b);

  return true;
}

bool Blend2DBackend::apply_pattern_fill(BLPath& path, const std::string& pattern_name, bool is_stroke) {
  if (!current_pdf_ || !current_page_) {
    return false;
  }

  // Look up pattern from resources (Form XObject stack first, then page)
  Dictionary pattern_resources = lookup_resource_type_dict("Pattern");
  if (pattern_resources.empty()) {
    return false;
  }

  // Look up the specific pattern
  auto pattern_it = pattern_resources.find(pattern_name);
  if (pattern_it == pattern_resources.end()) {
    return false;
  }

  // Resolve pattern reference if needed
  Dictionary pattern_dict;
  if (pattern_it->second.type == Value::DICTIONARY) {
    pattern_dict = pattern_it->second.dict;
  } else if (pattern_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      pattern_it->second.ref_object_number,
                                      pattern_it->second.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    pattern_dict = resolved.value.dict;
  } else {
    return false;
  }

  // Parse the pattern
  auto pattern = parse_pattern(*current_pdf_, pattern_dict);
  if (!pattern) {
    return false;
  }

  if (pattern->type == PatternType::Shading && pattern->shading) {
    // Shading pattern - apply as gradient fill
    auto shading = pattern->shading.get();

    if (shading->type == ShadingType::Axial && shading->coords.size() >= 4) {
      // Linear gradient
      float x0 = static_cast<float>(shading->coords[0]);
      float y0 = static_cast<float>(shading->coords[1]);
      float x1 = static_cast<float>(shading->coords[2]);
      float y1 = static_cast<float>(shading->coords[3]);

      // Transform through pattern matrix
      if (pattern->matrix.size() >= 6) {
        float a = static_cast<float>(pattern->matrix[0]);
        float b = static_cast<float>(pattern->matrix[1]);
        float c = static_cast<float>(pattern->matrix[2]);
        float d = static_cast<float>(pattern->matrix[3]);
        float e = static_cast<float>(pattern->matrix[4]);
        float f = static_cast<float>(pattern->matrix[5]);

        float new_x0 = a * x0 + c * y0 + e;
        float new_y0 = b * x0 + d * y0 + f;
        float new_x1 = a * x1 + c * y1 + e;
        float new_y1 = b * x1 + d * y1 + f;

        x0 = new_x0; y0 = new_y0;
        x1 = new_x1; y1 = new_y1;
      }

      // Apply page scale and Y-flip
      x0 *= state_.scale;
      y0 = (state_.page_height - y0) * state_.scale;
      x1 *= state_.scale;
      y1 = (state_.page_height - y1) * state_.scale;

      BLGradient gradient(BLLinearGradientValues(x0, y0, x1, y1));

      // Extract color stops from function (supports Type 2 and Type 3 stitching)
      auto color_stops = extract_color_stops_from_function(*current_pdf_, shading->function);
      for (auto& s : color_stops) {
        gradient.add_stop(s.first, s.second);
      }

      if (is_stroke) {
        ctx_.set_stroke_style(gradient);
      } else {
        ctx_.set_fill_style(gradient);
      }
      return true;
    }
    else if (shading->type == ShadingType::Radial && shading->coords.size() >= 6) {
      // Radial gradient
      float x0 = static_cast<float>(shading->coords[0]);
      float y0 = static_cast<float>(shading->coords[1]);
      float r0 = static_cast<float>(shading->coords[2]);
      float x1 = static_cast<float>(shading->coords[3]);
      float y1 = static_cast<float>(shading->coords[4]);
      float r1 = static_cast<float>(shading->coords[5]);

      // Transform through pattern matrix
      if (pattern->matrix.size() >= 6) {
        float a = static_cast<float>(pattern->matrix[0]);
        float b = static_cast<float>(pattern->matrix[1]);
        float c = static_cast<float>(pattern->matrix[2]);
        float d = static_cast<float>(pattern->matrix[3]);
        float e = static_cast<float>(pattern->matrix[4]);
        float f = static_cast<float>(pattern->matrix[5]);

        float new_x0 = a * x0 + c * y0 + e;
        float new_y0 = b * x0 + d * y0 + f;
        float new_x1 = a * x1 + c * y1 + e;
        float new_y1 = b * x1 + d * y1 + f;

        float scale_factor = std::sqrt(std::abs(a * d - b * c));
        r0 *= scale_factor;
        r1 *= scale_factor;

        x0 = new_x0; y0 = new_y0;
        x1 = new_x1; y1 = new_y1;
      }

      // Apply page scale and Y-flip
      x0 *= state_.scale;
      y0 = (state_.page_height - y0) * state_.scale;
      r0 *= state_.scale;
      x1 *= state_.scale;
      y1 = (state_.page_height - y1) * state_.scale;
      r1 *= state_.scale;

      BLGradient gradient(BLRadialGradientValues(x1, y1, x0, y0, r1, r0));

      // Extract color stops from function (supports Type 2 and Type 3 stitching)
      auto color_stops = extract_color_stops_from_function(*current_pdf_, shading->function);
      for (auto& s : color_stops) {
        gradient.add_stop(s.first, s.second);
      }

      if (is_stroke) {
        ctx_.set_stroke_style(gradient);
      } else {
        ctx_.set_fill_style(gradient);
      }
      return true;
    }
  }
  else if (pattern->type == PatternType::Tiling && pattern->tiling) {
    // Tiling pattern - render tile content stream to an image and create BLPattern
    auto& tiling = *pattern->tiling;

    // Calculate tile dimensions from bbox
    float tile_width = 1.0f, tile_height = 1.0f;
    float bbox_x0 = 0.0f, bbox_y0 = 0.0f;
    if (tiling.bbox.size() >= 4) {
      bbox_x0 = static_cast<float>(tiling.bbox[0]);
      bbox_y0 = static_cast<float>(tiling.bbox[1]);
      tile_width = static_cast<float>(tiling.bbox[2] - tiling.bbox[0]);
      tile_height = static_cast<float>(tiling.bbox[3] - tiling.bbox[1]);
    }

    // Use XStep/YStep for repeat interval if available (may differ from bbox)
    float step_width = (tiling.x_step > 0) ? static_cast<float>(tiling.x_step) : tile_width;
    float step_height = (tiling.y_step > 0) ? static_cast<float>(std::fabs(tiling.y_step)) : tile_height;

    if (tile_width <= 0 || tile_height <= 0 || tiling.content_stream.empty()) {
      // No content to render - fallback
      if (is_stroke) {
        ctx_.set_stroke_style(BLRgba32(128, 128, 128, 255));
      } else {
        ctx_.set_fill_style(BLRgba32(200, 200, 200, 255));
      }
      return true;
    }

    // Scale tile dimensions - use step size for image dimensions (repeat interval)
    float pattern_scale = state_.scale;
    uint32_t img_width = static_cast<uint32_t>(std::ceil(step_width * pattern_scale));
    uint32_t img_height = static_cast<uint32_t>(std::ceil(step_height * pattern_scale));

    if (img_width == 0) img_width = 1;
    if (img_height == 0) img_height = 1;
    if (img_width > 512) img_width = 512;
    if (img_height > 512) img_height = 512;

    // Try render cache for pre-rendered tile.
    uint64_t content_hash = fnv1a64(tiling.content_stream.data(),
                                    tiling.content_stream.size());
    int paint_type_int = static_cast<int>(tiling.paint_type);
    uintptr_t ptr_key = reinterpret_cast<uintptr_t>(pattern->tiling.get());
    std::string tile_cache_key = "tile:" + std::to_string(ptr_key) + ":" +
                                 std::to_string(content_hash) + ":" +
                                 std::to_string(img_width) + "x" +
                                 std::to_string(img_height) + ":" +
                                 std::to_string(paint_type_int);

    BLImage rendered_tile;
    RenderCacheEntry tile_cached;
    bool tile_cached_found = false;
    if (RenderCache::instance().find(tile_cache_key, tile_cached) &&
        tile_cached.width == img_width &&
        tile_cached.height == img_height) {
      // Cache hit — reconstruct BLImage from cached bytes.
      rendered_tile.create(img_width, img_height, BL_FORMAT_PRGB32);
      BLImageData tile_data;
      rendered_tile.get_data(&tile_data);
      memcpy(tile_data.pixel_data, tile_cached.data.data(),
             tile_cached.data.size());
      tile_cached_found = true;
    }

    if (!tile_cached_found) {
    // Create tile image and context
    BLImage tile_image(img_width, img_height, BL_FORMAT_PRGB32);
    BLContext tile_ctx(tile_image);

    // Clear with transparent (colored) or white (uncolored)
    if (tiling.paint_type == TilingPaintType::ColoredTiles) {
      tile_ctx.set_comp_op(BL_COMP_OP_CLEAR);
      tile_ctx.fill_all();
      tile_ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
    } else {
      tile_ctx.set_fill_style(BLRgba32(255, 255, 255, 255));
      tile_ctx.fill_all();
    }

    // Save current rendering state
    BLImage saved_image = std::move(image_);
    BLContext saved_ctx = std::move(ctx_);
    uint32_t saved_width = width_;
    uint32_t saved_height = height_;
    GraphicsState saved_state = state_;

    // Switch to tile canvas
    image_ = std::move(tile_image);
    ctx_ = std::move(tile_ctx);
    width_ = img_width;
    height_ = img_height;

    // Reset graphics state for tile rendering
    state_ = GraphicsState();
    state_.page_width = step_width;
    state_.page_height = step_height;
    state_.scale = pattern_scale;

    // Apply BBox origin offset (translate for non-zero BBox origin)
    if (bbox_x0 != 0.0f || bbox_y0 != 0.0f) {
      state_.transform.e = -bbox_x0 * pattern_scale;
      state_.transform.f = -bbox_y0 * pattern_scale;
    }

    // Push pattern resources if available
    if (!tiling.resources.empty()) {
      form_resources_stack_.push_back(tiling.resources);
    }

    // Parse and render the tile content stream
    bool progress_enabled = progress_.enabled;
    progress_.enabled = false;
    parse_pdf_content(tiling.content_stream);
    progress_.enabled = progress_enabled;

    // Pop pattern resources
    if (!tiling.resources.empty() && !form_resources_stack_.empty()) {
      form_resources_stack_.pop_back();
    }

    // Retrieve rendered tile
    ctx_.end();
    rendered_tile = std::move(image_);

    // Restore original state
    image_ = std::move(saved_image);
    ctx_ = std::move(saved_ctx);
    width_ = saved_width;
    height_ = saved_height;
    state_ = saved_state;

    // Store rendered tile in render cache for reuse across pages/shapes.
    {
      BLImageData cache_data;
      rendered_tile.get_data(&cache_data);
      RenderCacheEntry entry;
      entry.width = img_width;
      entry.height = img_height;
      entry.data.resize(img_width * img_height * 4);
      memcpy(entry.data.data(), cache_data.pixel_data, entry.data.size());
      RenderCache::instance().store(tile_cache_key, std::move(entry));
    }
    }  // end if (!tile_cached_found)

    // For uncolored tiles: convert rendered content to alpha mask and apply current fill color
    if (tiling.paint_type == TilingPaintType::UncoloredTiles) {
      BLImageData tile_data;
      rendered_tile.get_data(&tile_data);
      uint8_t* pixels = static_cast<uint8_t*>(tile_data.pixel_data);

      uint8_t pattern_r = is_stroke ? state_.stroke_r : state_.fill_r;
      uint8_t pattern_g = is_stroke ? state_.stroke_g : state_.fill_g;
      uint8_t pattern_b = is_stroke ? state_.stroke_b : state_.fill_b;

      for (uint32_t py = 0; py < img_height; ++py) {
        for (uint32_t px = 0; px < img_width; ++px) {
          size_t idx = py * tile_data.stride + px * 4;
          // PRGB32 is BGRA order
          uint8_t b_val = pixels[idx + 0];
          uint8_t g_val = pixels[idx + 1];
          uint8_t r_val = pixels[idx + 2];
          // Convert RGB to intensity, invert (white bg → 0 alpha, black content → 255 alpha)
          uint8_t intensity = static_cast<uint8_t>(255 - ((r_val + g_val + b_val) / 3));
          // Apply fill color with intensity as alpha (premultiplied)
          pixels[idx + 0] = static_cast<uint8_t>((pattern_b * intensity) / 255);
          pixels[idx + 1] = static_cast<uint8_t>((pattern_g * intensity) / 255);
          pixels[idx + 2] = static_cast<uint8_t>((pattern_r * intensity) / 255);
          pixels[idx + 3] = intensity;
        }
      }
    }

    // Create repeating pattern - Blend2D handles tiling natively
    BLPattern bl_pattern(rendered_tile, BL_EXTEND_MODE_REPEAT);

    // Apply pattern matrix
    if (pattern->matrix.size() >= 6) {
      BLMatrix2D mat(
        pattern->matrix[0], pattern->matrix[1],
        pattern->matrix[2], pattern->matrix[3],
        pattern->matrix[4] * state_.scale,
        (state_.page_height - pattern->matrix[5]) * state_.scale
      );
      bl_pattern.set_transform(mat);
    }

    if (is_stroke) {
      ctx_.set_stroke_style(bl_pattern);
    } else {
      ctx_.set_fill_style(bl_pattern);
    }
    return true;
  }

  // Fallback to placeholder color
  if (is_stroke) {
    ctx_.set_stroke_style(BLRgba32(128, 128, 128, 255));
  } else {
    ctx_.set_fill_style(BLRgba32(200, 200, 200, 255));
  }
  return true;
}

bool Blend2DBackend::render_soft_mask_group(const Value& group_xobject, int mask_type) {
  if (!current_pdf_) return false;

  // Get the XObject stream - capture obj/gen numbers for decryption
  Value xobject_value = group_xobject;
  uint32_t smask_obj_num = 0;
  uint16_t smask_gen_num = 0;
  if (xobject_value.type == Value::REFERENCE) {
    smask_obj_num = xobject_value.ref_object_number;
    smask_gen_num = xobject_value.ref_generation_number;
    auto resolved = resolve_reference(*current_pdf_,
                                      smask_obj_num, smask_gen_num);
    if (!resolved.success) return false;
    xobject_value = resolved.value;
  }

  if (xobject_value.type != Value::STREAM) return false;

  // Check that it's a Form XObject
  auto subtype_it = xobject_value.stream.dict.find("Subtype");
  if (subtype_it == xobject_value.stream.dict.end() ||
      subtype_it->second.type != Value::NAME ||
      subtype_it->second.name != "Form") {
    return false;
  }

  // Get dimensions from BBox
  float bbox[4] = {0, 0, 100, 100};  // Default
  auto bbox_it = xobject_value.stream.dict.find("BBox");
  if (bbox_it != xobject_value.stream.dict.end() && bbox_it->second.type == Value::ARRAY) {
    const auto& arr = bbox_it->second.array;
    for (size_t i = 0; i < 4 && i < arr.size(); ++i) {
      if (arr[i].type == Value::NUMBER) {
        bbox[i] = static_cast<float>(arr[i].number);
      }
    }
  }

  uint32_t mask_width = static_cast<uint32_t>(std::abs(bbox[2] - bbox[0]) * state_.scale);
  uint32_t mask_height = static_cast<uint32_t>(std::abs(bbox[3] - bbox[1]) * state_.scale);

  if (mask_width == 0) mask_width = width_;
  if (mask_height == 0) mask_height = height_;

  // Decode the XObject stream content
  auto decoded = decode_stream(*current_pdf_, xobject_value,
                               smask_obj_num, smask_gen_num);
  if (!decoded.success) {
    return false;
  }

  // Create a temporary image for rendering the soft mask
  BLImage mask_image(mask_width, mask_height, BL_FORMAT_PRGB32);
  BLContext mask_ctx(mask_image);

  // Clear with white (for luminosity) or transparent (for alpha)
  if (mask_type == 2) {  // Luminosity
    mask_ctx.set_fill_style(BLRgba32(255, 255, 255, 255));
  } else {  // Alpha
    mask_ctx.set_fill_style(BLRgba32(0, 0, 0, 0));
  }
  mask_ctx.fill_all();

  // Save current rendering context state
  BLImage saved_image = std::move(image_);
  BLContext saved_ctx = std::move(ctx_);
  uint32_t saved_width = width_;
  uint32_t saved_height = height_;
  GraphicsState saved_state = state_;

  // Switch to mask rendering context
  image_ = std::move(mask_image);
  ctx_ = std::move(mask_ctx);
  width_ = mask_width;
  height_ = mask_height;

  // Reset graphics state for mask rendering
  state_ = GraphicsState();
  state_.page_width = std::abs(bbox[2] - bbox[0]);
  state_.page_height = std::abs(bbox[3] - bbox[1]);
  state_.scale = static_cast<float>(mask_width) / state_.page_width;

  // Parse and render the XObject content to the mask context
  bool progress_enabled = progress_.enabled;
  progress_.enabled = false;
  parse_pdf_content(decoded.data);
  progress_.enabled = progress_enabled;

  // Flush rendering
  ctx_.flush(BL_CONTEXT_FLUSH_SYNC);

  // Get the rendered pixels from mask image
  BLImageData data;
  image_.get_data(&data);

  // Convert to grayscale mask values
  std::vector<uint8_t> mask_data(mask_width * mask_height);
  const uint8_t* pixels = static_cast<const uint8_t*>(data.pixel_data);

  for (uint32_t y = 0; y < mask_height; ++y) {
    for (uint32_t x = 0; x < mask_width; ++x) {
      size_t src_idx = (y * data.stride) + (x * 4);
      size_t dst_idx = y * mask_width + x;

      if (mask_type == 2) {  // Luminosity mask
        // Convert RGB to luminance using standard coefficients
        float r = pixels[src_idx + 2] / 255.0f;
        float g = pixels[src_idx + 1] / 255.0f;
        float b = pixels[src_idx + 0] / 255.0f;
        float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        mask_data[dst_idx] = static_cast<uint8_t>(luminance * 255.0f);
      } else {  // Alpha mask
        mask_data[dst_idx] = pixels[src_idx + 3];  // Alpha channel
      }
    }
  }

  // End mask context before restoring
  ctx_.end();

  // Restore original rendering context
  image_ = std::move(saved_image);
  ctx_ = std::move(saved_ctx);
  width_ = saved_width;
  height_ = saved_height;

  // Restore graphics state but keep extracted mask data
  state_ = saved_state;
  state_.soft_mask_width = mask_width;
  state_.soft_mask_height = mask_height;
  state_.soft_mask_data = std::move(mask_data);

  return true;
}

void Blend2DBackend::apply_soft_mask_to_context() {
  if (!state_.has_soft_mask || state_.soft_mask_data.empty()) {
    return;
  }

  // Blend2D doesn't have direct soft mask support like PDF
  // We simulate by adjusting the global alpha based on mask values
  // This is a simplified approximation

  // For proper implementation, we would need to:
  // 1. Render to an offscreen buffer
  // 2. Apply the soft mask as a per-pixel alpha multiplier
  // 3. Composite the result to the main image

  // For now, calculate average mask value as a global alpha approximation
  if (state_.soft_mask_width > 0 && state_.soft_mask_height > 0) {
    uint64_t sum = 0;
    for (size_t i = 0; i < state_.soft_mask_data.size(); ++i) {
      sum += state_.soft_mask_data[i];
    }
    float avg_alpha = static_cast<float>(sum) / (state_.soft_mask_data.size() * 255.0f);

    // Multiply current opacity by mask average
    state_.fill_opacity *= avg_alpha;
    state_.stroke_opacity *= avg_alpha;
  }
}

}  // namespace nanopdf

#endif // NANOPDF_USE_BLEND2D
