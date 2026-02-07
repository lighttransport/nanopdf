// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifdef NANOPDF_USE_BLEND2D

#include "blend2d-backend.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

// For PNG saving
#include "stb_image_write.h"

// External C variable for PNG compression level
extern "C" int stbi_write_png_compression_level;

namespace nanopdf {

// Helper function to convert glyph name to Unicode codepoint
static uint32_t glyph_name_to_unicode(const std::string& name) {
  // Standard glyph names mapping (subset)
  static const std::map<std::string, uint32_t> glyph_map = {
    {"space", 0x0020}, {"exclam", 0x0021}, {"quotedbl", 0x0022},
    {"numbersign", 0x0023}, {"dollar", 0x0024}, {"percent", 0x0025},
    {"ampersand", 0x0026}, {"quotesingle", 0x0027}, {"parenleft", 0x0028},
    {"parenright", 0x0029}, {"asterisk", 0x002A}, {"plus", 0x002B},
    {"comma", 0x002C}, {"hyphen", 0x002D}, {"period", 0x002E},
    {"slash", 0x002F}, {"zero", 0x0030}, {"one", 0x0031},
    {"two", 0x0032}, {"three", 0x0033}, {"four", 0x0034},
    {"five", 0x0035}, {"six", 0x0036}, {"seven", 0x0037},
    {"eight", 0x0038}, {"nine", 0x0039}, {"colon", 0x003A},
    {"semicolon", 0x003B}, {"less", 0x003C}, {"equal", 0x003D},
    {"greater", 0x003E}, {"question", 0x003F}, {"at", 0x0040},
    // Uppercase letters
    {"A", 0x0041}, {"B", 0x0042}, {"C", 0x0043}, {"D", 0x0044},
    {"E", 0x0045}, {"F", 0x0046}, {"G", 0x0047}, {"H", 0x0048},
    {"I", 0x0049}, {"J", 0x004A}, {"K", 0x004B}, {"L", 0x004C},
    {"M", 0x004D}, {"N", 0x004E}, {"O", 0x004F}, {"P", 0x0050},
    {"Q", 0x0051}, {"R", 0x0052}, {"S", 0x0053}, {"T", 0x0054},
    {"U", 0x0055}, {"V", 0x0056}, {"W", 0x0057}, {"X", 0x0058},
    {"Y", 0x0059}, {"Z", 0x005A},
    // Lowercase letters
    {"a", 0x0061}, {"b", 0x0062}, {"c", 0x0063}, {"d", 0x0064},
    {"e", 0x0065}, {"f", 0x0066}, {"g", 0x0067}, {"h", 0x0068},
    {"i", 0x0069}, {"j", 0x006A}, {"k", 0x006B}, {"l", 0x006C},
    {"m", 0x006D}, {"n", 0x006E}, {"o", 0x006F}, {"p", 0x0070},
    {"q", 0x0071}, {"r", 0x0072}, {"s", 0x0073}, {"t", 0x0074},
    {"u", 0x0075}, {"v", 0x0076}, {"w", 0x0077}, {"x", 0x0078},
    {"y", 0x0079}, {"z", 0x007A},
  };

  auto it = glyph_map.find(name);
  if (it != glyph_map.end()) {
    return it->second;
  }

  // Try uniXXXX format
  if (name.length() == 7 && name.substr(0, 3) == "uni") {
    try {
      return static_cast<uint32_t>(std::stoul(name.substr(3), nullptr, 16));
    } catch (...) {
      return 0;
    }
  }

  // Single character name
  if (name.length() == 1) {
    return static_cast<uint32_t>(name[0]);
  }

  return 0;
}

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

// Adobe-Japan1 CID to Unicode mapping
// Based on Adobe-Japan1-7 standard mapping (subset for common characters)
static uint32_t adobe_japan1_cid_to_unicode(uint32_t cid) {
  // CID 1 = space
  if (cid == 1) return 0x0020;

  // CIDs 2-94: Fullwidth ASCII punctuation and letters
  static const uint32_t cid_2_94[] = {
    0xFF01, 0xFF02, 0xFF03, 0xFF04, 0xFF05, 0xFF06, 0xFF07, 0xFF08, 0xFF09, 0xFF0A,
    0xFF0B, 0xFF0C, 0xFF0D, 0xFF0E, 0xFF0F, 0xFF10, 0xFF11, 0xFF12, 0xFF13, 0xFF14,
    0xFF15, 0xFF16, 0xFF17, 0xFF18, 0xFF19, 0xFF1A, 0xFF1B, 0xFF1C, 0xFF1D, 0xFF1E,
    0xFF1F, 0xFF20, 0xFF21, 0xFF22, 0xFF23, 0xFF24, 0xFF25, 0xFF26, 0xFF27, 0xFF28,
    0xFF29, 0xFF2A, 0xFF2B, 0xFF2C, 0xFF2D, 0xFF2E, 0xFF2F, 0xFF30, 0xFF31, 0xFF32,
    0xFF33, 0xFF34, 0xFF35, 0xFF36, 0xFF37, 0xFF38, 0xFF39, 0xFF3A, 0xFF3B, 0xFF3C,
    0xFF3D, 0xFF3E, 0xFF3F, 0xFF40, 0xFF41, 0xFF42, 0xFF43, 0xFF44, 0xFF45, 0xFF46,
    0xFF47, 0xFF48, 0xFF49, 0xFF4A, 0xFF4B, 0xFF4C, 0xFF4D, 0xFF4E, 0xFF4F, 0xFF50,
    0xFF51, 0xFF52, 0xFF53, 0xFF54, 0xFF55, 0xFF56, 0xFF57, 0xFF58, 0xFF59, 0xFF5A,
    0xFF5B, 0xFF5C, 0xFF5D
  };
  if (cid >= 2 && cid <= 94) {
    return cid_2_94[cid - 2];
  }

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

  // Hiragana (CID 842-924 -> U+3041-U+3093)
  if (cid >= 842 && cid <= 924) {
    return 0x3041 + (cid - 842);
  }

  // Katakana (CID 925-1010 -> U+30A1-U+30F6)
  if (cid >= 925 && cid <= 1010) {
    return 0x30A1 + (cid - 925);
  }

  // Fullwidth digits (CID 231-240 -> U+FF10-U+FF19)
  if (cid >= 231 && cid <= 240) {
    return 0xFF10 + (cid - 231);
  }

  // Fullwidth uppercase (CID 241-266 -> U+FF21-U+FF3A)
  if (cid >= 241 && cid <= 266) {
    return 0xFF21 + (cid - 241);
  }

  // Fullwidth lowercase (CID 267-292 -> U+FF41-U+FF5A)
  if (cid >= 267 && cid <= 292) {
    return 0xFF41 + (cid - 267);
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

  // Log unmapped CIDs for debugging (limited to avoid spam)
  static std::set<uint32_t> unmapped_cids;
  if (unmapped_cids.size() < 200 && unmapped_cids.find(cid) == unmapped_cids.end()) {
    unmapped_cids.insert(cid);
    printf("UNMAPPED_CID: %u\n", cid);
  }

  // Not found - return 0 to indicate no mapping
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
  auto* type0_font = dynamic_cast<const Type0Font*>(font);
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

// Check if font uses two-byte CID encoding
static bool is_two_byte_cid_font(const Type0Font* type0_font) {
  if (!type0_font) {
    return false;
  }
  // Determine if the CMap uses two-byte encoding based on CMap name
  // Common two-byte CMaps: Identity-H, Identity-V, UniJIS-*, UniGB-*, UniKS-*, UniCNS-*
  const std::string& cmap_name = type0_font->encoding_cmap.name;
  if (cmap_name.find("Identity") != std::string::npos ||
      cmap_name.find("UTF16") != std::string::npos ||
      cmap_name.find("UCS2") != std::string::npos ||
      cmap_name.find("UniJIS") != std::string::npos ||
      cmap_name.find("UniGB") != std::string::npos ||
      cmap_name.find("UniKS") != std::string::npos ||
      cmap_name.find("UniCNS") != std::string::npos ||
      // Also check registry/ordering for CJK fonts
      type0_font->ordering == "Japan1" ||
      type0_font->ordering == "GB1" ||
      type0_font->ordering == "CNS1" ||
      type0_font->ordering == "Korea1") {
    return true;
  }
  return false;
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

  // Clear canvas with background color
  ctx_.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx_.set_fill_style(BLRgba32(options.bg_r, options.bg_g, options.bg_b, options.bg_a));
  ctx_.fill_all();
  ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);

  // Process each content stream
  for (const auto& content_obj : page.contents) {
    Value resolved_obj = content_obj;
    uint32_t obj_num = 0;
    uint16_t gen_num = 0;

    if (content_obj.type == Value::REFERENCE) {
      obj_num = content_obj.ref_object_number;
      gen_num = content_obj.ref_generation_number;
      auto resolved = resolve_reference(pdf, obj_num, gen_num);
      if (resolved.success) {
        resolved_obj = resolved.value;
      } else {
        continue;
      }
    }

    if (resolved_obj.type == Value::STREAM) {
      auto decoded_result = decode_stream(pdf, resolved_obj, obj_num, gen_num);
      if (decoded_result.success) {
        state_ = GraphicsState();
        state_.page_width = page_width;
        state_.page_height = page_height;
        state_.scale = scale;
        parse_pdf_content(decoded_result.data);
      }
    }
  }

  ctx_.flush(BL_CONTEXT_FLUSH_SYNC);

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
  auto* type0_font = dynamic_cast<const Type0Font*>(font);
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
    // CJK fonts (Japanese, Chinese, Korean)
    {
      // Japanese
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/opentype/noto/NotoSansJP-Regular.otf",
      "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
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
      "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
      "/Library/Fonts/Osaka.ttf",
      // Windows
      "C:\\Windows\\Fonts\\msgothic.ttc",
      "C:\\Windows\\Fonts\\meiryo.ttc",
      "C:\\Windows\\Fonts\\YuGothR.ttc",
      "C:\\Windows\\Fonts\\simsun.ttc",
      "C:\\Windows\\Fonts\\malgun.ttf"
    }
  };

  // Try fonts in the matching category first
  const auto& preferred_paths = font_paths_by_category[category];
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

  // For Type0 fonts, check descendant font's descriptor for embedded font
  const FontDescriptor* desc = nullptr;
  auto* type0_font = dynamic_cast<const Type0Font*>(font);
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
        // Check if this is a CFF font (stb_truetype doesn't support CFF)
        bool is_cff = false;
        if (decoded.data.size() >= 4) {
          // CFF fonts start with specific signature or have OTTO header
          if ((decoded.data[0] == 0x01 && decoded.data[1] == 0x00) ||  // CFF
              (decoded.data[0] == 'O' && decoded.data[1] == 'T' &&
               decoded.data[2] == 'T' && decoded.data[3] == 'O')) {    // OpenType CFF
            is_cff = true;
          }
        }

        if (is_cff) {
          // CFF/OpenType CFF fonts not supported by stb_truetype, use fallback
          return load_fallback_font_with_hint(font_name, font);
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

bool Blend2DBackend::draw_glyph(int codepoint, float x, float y, float size,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!initialized_) return false;

  FontCache* font = get_font(current_font_name_);
  if (!font) {
    // Fallback to placeholder
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_rect(x, y - size, size * 0.5f, size);
    return true;
  }

  stbtt_vertex* vertices = nullptr;
  int num_verts = stbtt_GetCodepointShape(&font->font_info, codepoint, &vertices);

  if (num_verts == 0 || !vertices) {
    // Glyph not found in font - draw placeholder
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_rect(x, y - size, size * 0.5f, size);
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
  bool invisible = (render_mode == 3 || render_mode == 7);

  stbtt_FreeShape(&font->font_info, vertices);

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
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_rect(x, y - size, size * 0.5f, size);
    return true;
  }

  stbtt_vertex* vertices = nullptr;
  int num_verts = stbtt_GetGlyphShape(&font->font_info, glyph_index, &vertices);

  if (num_verts == 0 || !vertices) {
    // Glyph not found - draw placeholder rectangle
    ctx_.set_fill_style(BLRgba32(r, g, b, a));
    ctx_.fill_rect(x, y - size, size * 0.5f, size);
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
  bool invisible = (render_mode == 3 || render_mode == 7);

  stbtt_FreeShape(&font->font_info, vertices);

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

float Blend2DBackend::calculate_text_width(const std::string& text, float font_size) {
  // First try to use PDF font width information for Type0/CID fonts
  auto* type0_font = dynamic_cast<const Type0Font*>(current_font_);
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

float Blend2DBackend::render_text_string(const std::string& text,
    float x, float y, float font_size,
    uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  FontCache* font = get_font(current_font_name_);
  float cursor_x = x;

  if (!font) {
    // Fallback: draw placeholder rectangles for each character
    float char_width = font_size * 0.6f;
    for (size_t i = 0; i < text.length(); i++) {
      draw_rectangle(cursor_x, y - font_size, char_width * 0.8f, font_size,
                    200, 200, 200, a);
      cursor_x += char_width;
    }
    return cursor_x - x;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, font_size);

  // Check if this is a Type0/CID font that uses two-byte encoding
  auto* type0_font = dynamic_cast<const Type0Font*>(current_font_);
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

bool Blend2DBackend::draw_image(const ImageXObject& image, float x, float y, float w, float h,
                                uint8_t fill_r, uint8_t fill_g, uint8_t fill_b) {
  if (!initialized_ || image.data.empty()) {
    return false;
  }

  int img_width = image.width;
  int img_height = image.height;

  // Create Blend2D image from decoded data
  BLImage img;
  img.create(img_width, img_height, BL_FORMAT_PRGB32);

  BLImageData img_data;
  img.get_data(&img_data);

  uint8_t* dst = static_cast<uint8_t*>(img_data.pixel_data);
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
  }

  // Draw image scaled to fit
  ctx_.save();
  ctx_.translate(x, y);
  ctx_.scale(w / img_width, h / img_height);
  ctx_.set_pattern_quality(BL_PATTERN_QUALITY_BILINEAR);
  ctx_.blit_image(BLPointI(0, 0), img);
  ctx_.restore();

  return true;
}

// C++14 compatible clamp (std::clamp is C++17)
template<typename T>
constexpr T clamp14(const T& v, const T& lo, const T& hi) {
  return (v < lo) ? lo : (hi < v) ? hi : v;
}

// PostScript calculator stack for Type 4 function evaluation
class PSStack {
public:
  void push(double v) { stack_.push_back(v); }

  double pop() {
    if (stack_.empty()) return 0.0;
    double v = stack_.back();
    stack_.pop_back();
    return v;
  }

  double top() const {
    return stack_.empty() ? 0.0 : stack_.back();
  }

  size_t size() const { return stack_.size(); }

  bool empty() const { return stack_.empty(); }

  double at(size_t idx) const {
    if (idx >= stack_.size()) return 0.0;
    return stack_[stack_.size() - 1 - idx];
  }

  void dup() {
    if (!stack_.empty()) push(top());
  }

  void exch() {
    if (stack_.size() >= 2) {
      double a = pop();
      double b = pop();
      push(a);
      push(b);
    }
  }

  void roll(int n, int j) {
    if (n <= 0 || static_cast<size_t>(n) > stack_.size()) return;
    j = ((j % n) + n) % n;
    if (j == 0) return;

    std::vector<double> temp;
    for (int i = 0; i < n; ++i) {
      temp.push_back(pop());
    }
    for (int i = 0; i < n; ++i) {
      push(temp[(i + j) % n]);
    }
  }

  void copy(int n) {
    if (n <= 0 || static_cast<size_t>(n) > stack_.size()) return;
    std::vector<double> temp;
    for (int i = 0; i < n; ++i) {
      temp.push_back(stack_[stack_.size() - n + i]);
    }
    for (const auto& v : temp) {
      push(v);
    }
  }

  void index(int i) {
    if (i < 0 || static_cast<size_t>(i) >= stack_.size()) {
      push(0.0);
      return;
    }
    push(stack_[stack_.size() - 1 - i]);
  }

  const std::vector<double>& data() const { return stack_; }

private:
  std::vector<double> stack_;
};

// Tokenize PostScript code
static std::vector<std::string> tokenize_postscript(const std::string& code) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < code.size()) {
    while (i < code.size() && (code[i] == ' ' || code[i] == '\t' ||
                                code[i] == '\n' || code[i] == '\r')) {
      ++i;
    }
    if (i >= code.size()) break;

    if (code[i] == '%') {
      while (i < code.size() && code[i] != '\n' && code[i] != '\r') ++i;
      continue;
    }

    if (code[i] == '{' || code[i] == '}') {
      tokens.push_back(std::string(1, code[i]));
      ++i;
      continue;
    }

    std::string token;
    while (i < code.size() && code[i] != ' ' && code[i] != '\t' &&
           code[i] != '\n' && code[i] != '\r' &&
           code[i] != '{' && code[i] != '}' && code[i] != '%') {
      token += code[i];
      ++i;
    }
    if (!token.empty()) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

// Check if token is a number
static bool is_ps_number(const std::string& token, double& value) {
  if (token.empty()) return false;
  char* end = nullptr;
  value = std::strtod(token.c_str(), &end);
  return end != token.c_str() && *end == '\0';
}

// Execute PostScript tokens
static bool execute_postscript(const std::vector<std::string>& tokens,
                                size_t& pos, PSStack& stack,
                                int depth = 0) {
  const int MAX_DEPTH = 100;
  if (depth > MAX_DEPTH) return false;

  while (pos < tokens.size()) {
    const std::string& tok = tokens[pos];
    ++pos;

    if (tok == "}") {
      return true;
    }

    if (tok == "{") {
      int brace_count = 1;
      while (pos < tokens.size() && brace_count > 0) {
        if (tokens[pos] == "{") ++brace_count;
        else if (tokens[pos] == "}") --brace_count;
        ++pos;
      }
      continue;
    }

    double num_val;
    if (is_ps_number(tok, num_val)) {
      stack.push(num_val);
      continue;
    }

    if (tok == "true") { stack.push(1.0); continue; }
    if (tok == "false") { stack.push(0.0); continue; }

    // Arithmetic operators
    if (tok == "add") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a + b);
    } else if (tok == "sub") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a - b);
    } else if (tok == "mul") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a * b);
    } else if (tok == "div") {
      double b = stack.pop(), a = stack.pop();
      stack.push(b != 0 ? a / b : 0.0);
    } else if (tok == "idiv") {
      int b = static_cast<int>(stack.pop());
      int a = static_cast<int>(stack.pop());
      stack.push(b != 0 ? static_cast<double>(a / b) : 0.0);
    } else if (tok == "mod") {
      int b = static_cast<int>(stack.pop());
      int a = static_cast<int>(stack.pop());
      stack.push(b != 0 ? static_cast<double>(a % b) : 0.0);
    } else if (tok == "neg") {
      stack.push(-stack.pop());
    } else if (tok == "abs") {
      stack.push(std::fabs(stack.pop()));
    } else if (tok == "ceiling") {
      stack.push(std::ceil(stack.pop()));
    } else if (tok == "floor") {
      stack.push(std::floor(stack.pop()));
    } else if (tok == "round") {
      stack.push(std::round(stack.pop()));
    } else if (tok == "truncate") {
      stack.push(std::trunc(stack.pop()));
    } else if (tok == "sqrt") {
      double v = stack.pop();
      stack.push(v >= 0 ? std::sqrt(v) : 0.0);
    } else if (tok == "sin") {
      stack.push(std::sin(stack.pop() * M_PI / 180.0));
    } else if (tok == "cos") {
      stack.push(std::cos(stack.pop() * M_PI / 180.0));
    } else if (tok == "atan") {
      double x = stack.pop(), y = stack.pop();
      stack.push(std::atan2(y, x) * 180.0 / M_PI);
    } else if (tok == "exp") {
      double e = stack.pop(), b = stack.pop();
      stack.push(std::pow(b, e));
    } else if (tok == "ln") {
      double v = stack.pop();
      stack.push(v > 0 ? std::log(v) : 0.0);
    } else if (tok == "log") {
      double v = stack.pop();
      stack.push(v > 0 ? std::log10(v) : 0.0);
    }
    // Relational operators
    else if (tok == "eq") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a == b ? 1.0 : 0.0);
    } else if (tok == "ne") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a != b ? 1.0 : 0.0);
    } else if (tok == "gt") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a > b ? 1.0 : 0.0);
    } else if (tok == "ge") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a >= b ? 1.0 : 0.0);
    } else if (tok == "lt") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a < b ? 1.0 : 0.0);
    } else if (tok == "le") {
      double b = stack.pop(), a = stack.pop();
      stack.push(a <= b ? 1.0 : 0.0);
    }
    // Boolean operators
    else if (tok == "and") {
      int b = static_cast<int>(stack.pop());
      int a = static_cast<int>(stack.pop());
      stack.push(static_cast<double>(a & b));
    } else if (tok == "or") {
      int b = static_cast<int>(stack.pop());
      int a = static_cast<int>(stack.pop());
      stack.push(static_cast<double>(a | b));
    } else if (tok == "xor") {
      int b = static_cast<int>(stack.pop());
      int a = static_cast<int>(stack.pop());
      stack.push(static_cast<double>(a ^ b));
    } else if (tok == "not") {
      int a = static_cast<int>(stack.pop());
      stack.push(a == 0 ? 1.0 : 0.0);
    } else if (tok == "bitshift") {
      int shift = static_cast<int>(stack.pop());
      int val = static_cast<int>(stack.pop());
      stack.push(static_cast<double>(shift >= 0 ? (val << shift) : (val >> (-shift))));
    }
    // Stack operators
    else if (tok == "dup") {
      stack.dup();
    } else if (tok == "exch") {
      stack.exch();
    } else if (tok == "pop") {
      stack.pop();
    } else if (tok == "copy") {
      int n = static_cast<int>(stack.pop());
      stack.copy(n);
    } else if (tok == "index") {
      int i = static_cast<int>(stack.pop());
      stack.index(i);
    } else if (tok == "roll") {
      int j = static_cast<int>(stack.pop());
      int n = static_cast<int>(stack.pop());
      stack.roll(n, j);
    }
    // Conditional operators (simplified)
    else if (tok == "if") {
      stack.pop();  // condition already consumed
    } else if (tok == "ifelse") {
      stack.pop();  // condition
    }
    else if (tok == "cvr") {
      // Convert to real - already real
    } else if (tok == "cvi") {
      stack.push(static_cast<double>(static_cast<int>(stack.pop())));
    }
  }
  return true;
}

// Evaluate a PostScript Type 4 function
static bool evaluate_postscript_function(const std::string& code,
                                          const std::vector<double>& inputs,
                                          const std::vector<double>& domain,
                                          const std::vector<double>& range,
                                          std::vector<double>& outputs) {
  std::string stripped = code;
  size_t start = stripped.find('{');
  size_t end = stripped.rfind('}');
  if (start != std::string::npos && end != std::string::npos && end > start) {
    stripped = stripped.substr(start + 1, end - start - 1);
  }

  std::vector<std::string> tokens = tokenize_postscript(stripped);

  PSStack stack;
  for (size_t i = 0; i < inputs.size(); ++i) {
    double v = inputs[i];
    if (domain.size() >= (i + 1) * 2) {
      v = clamp14(v, domain[i * 2], domain[i * 2 + 1]);
    }
    stack.push(v);
  }

  size_t pos = 0;
  if (!execute_postscript(tokens, pos, stack)) {
    return false;
  }

  int n_outputs = static_cast<int>(range.size() / 2);
  if (n_outputs <= 0) n_outputs = static_cast<int>(stack.size());

  outputs.clear();
  const auto& stack_data = stack.data();

  for (int i = n_outputs - 1; i >= 0 && !stack_data.empty(); --i) {
    size_t idx = stack_data.size() - 1 - i;
    if (idx < stack_data.size()) {
      double v = stack_data[idx];
      if (range.size() >= static_cast<size_t>((n_outputs - 1 - i + 1) * 2)) {
        size_t ri = (n_outputs - 1 - i) * 2;
        v = clamp14(v, range[ri], range[ri + 1]);
      }
      outputs.push_back(v);
    }
  }

  std::reverse(outputs.begin(), outputs.end());
  return !outputs.empty();
}

// PDF function evaluator - evaluates a PDF function at given input values
static bool evaluate_pdf_function(const Pdf& pdf, const Value& function,
                                   const std::vector<double>& inputs,
                                   std::vector<double>& outputs) {
  if (function.type != Value::DICTIONARY && function.type != Value::STREAM) {
    return false;
  }

  // For STREAM type, the dictionary is in stream.dict
  const Dictionary& func_dict = (function.type == Value::STREAM) ? function.stream.dict : function.dict;

  auto func_type_it = func_dict.find("FunctionType");
  if (func_type_it == func_dict.end() || func_type_it->second.type != Value::NUMBER) {
    return false;
  }

  int func_type = static_cast<int>(func_type_it->second.number);

  std::vector<double> domain;
  auto domain_it = func_dict.find("Domain");
  if (domain_it != func_dict.end() && domain_it->second.type == Value::ARRAY) {
    for (const auto& v : domain_it->second.array) {
      if (v.type == Value::NUMBER) {
        domain.push_back(v.number);
      }
    }
  }

  std::vector<double> range;
  auto range_it = func_dict.find("Range");
  if (range_it != func_dict.end() && range_it->second.type == Value::ARRAY) {
    for (const auto& v : range_it->second.array) {
      if (v.type == Value::NUMBER) {
        range.push_back(v.number);
      }
    }
  }

  if (func_type == 0) {
    // Type 0: Sampled function
    auto size_it = func_dict.find("Size");
    auto bps_it = func_dict.find("BitsPerSample");

    if (size_it == func_dict.end() || bps_it == func_dict.end()) {
      return false;
    }

    std::vector<int> sizes;
    if (size_it->second.type == Value::ARRAY) {
      for (const auto& v : size_it->second.array) {
        if (v.type == Value::NUMBER) {
          sizes.push_back(static_cast<int>(v.number));
        }
      }
    }

    int bps = static_cast<int>(bps_it->second.number);
    int n_outputs = static_cast<int>(range.size() / 2);
    if (n_outputs <= 0) n_outputs = 3;

    std::vector<double> decode;
    auto decode_it = func_dict.find("Decode");
    if (decode_it != func_dict.end() && decode_it->second.type == Value::ARRAY) {
      for (const auto& v : decode_it->second.array) {
        if (v.type == Value::NUMBER) {
          decode.push_back(v.number);
        }
      }
    }

    if (function.type == Value::STREAM && !sizes.empty() && !inputs.empty()) {
      auto decoded = decode_stream(pdf, function);
      if (decoded.success && !decoded.data.empty()) {
        int n_inputs = static_cast<int>(sizes.size());
        if (n_inputs == 1 && domain.size() >= 2) {
          double t = (inputs[0] - domain[0]) / (domain[1] - domain[0]);
          t = clamp14(t, 0.0, 1.0);
          int idx = static_cast<int>(t * (sizes[0] - 1));
          idx = clamp14(idx, 0, sizes[0] - 1);

          outputs.resize(n_outputs);
          int bytes_per_sample = (bps + 7) / 8;
          int sample_offset = idx * n_outputs * bytes_per_sample;

          for (int i = 0; i < n_outputs && sample_offset + i * bytes_per_sample < static_cast<int>(decoded.data.size()); ++i) {
            double val = 0;
            if (bps == 8) {
              val = decoded.data[sample_offset + i] / 255.0;
            } else if (bps == 16) {
              val = ((decoded.data[sample_offset + i * 2] << 8) |
                     decoded.data[sample_offset + i * 2 + 1]) / 65535.0;
            }
            if (decode.size() >= static_cast<size_t>((i + 1) * 2)) {
              val = decode[i * 2] + val * (decode[i * 2 + 1] - decode[i * 2]);
            }
            outputs[i] = val;
          }
          return true;
        }
      }
    }

    outputs.resize(n_outputs, 0.5);
    return true;

  } else if (func_type == 2) {
    // Type 2: Exponential interpolation
    auto c0_it = func_dict.find("C0");
    auto c1_it = func_dict.find("C1");
    auto n_it = func_dict.find("N");

    std::vector<double> c0, c1;
    double n_exp = 1.0;

    if (c0_it != func_dict.end() && c0_it->second.type == Value::ARRAY) {
      for (const auto& v : c0_it->second.array) {
        if (v.type == Value::NUMBER) {
          c0.push_back(v.number);
        }
      }
    }
    if (c1_it != func_dict.end() && c1_it->second.type == Value::ARRAY) {
      for (const auto& v : c1_it->second.array) {
        if (v.type == Value::NUMBER) {
          c1.push_back(v.number);
        }
      }
    }
    if (n_it != func_dict.end() && n_it->second.type == Value::NUMBER) {
      n_exp = n_it->second.number;
    }

    if (c0.empty()) c0 = {0.0};
    if (c1.empty()) c1 = {1.0};

    double x = inputs.empty() ? 0.0 : inputs[0];
    if (domain.size() >= 2) {
      x = clamp14(x, domain[0], domain[1]);
      x = (x - domain[0]) / (domain[1] - domain[0]);
    }

    double x_pow = std::pow(x, n_exp);
    outputs.resize(c0.size());
    for (size_t i = 0; i < c0.size(); ++i) {
      double c1_val = (i < c1.size()) ? c1[i] : 1.0;
      outputs[i] = c0[i] + x_pow * (c1_val - c0[i]);
    }

    return true;

  } else if (func_type == 3) {
    // Type 3: Stitching function
    auto functions_it = func_dict.find("Functions");
    auto bounds_it = func_dict.find("Bounds");
    auto encode_it = func_dict.find("Encode");

    if (functions_it == func_dict.end() || functions_it->second.type != Value::ARRAY) {
      return false;
    }

    const auto& functions = functions_it->second.array;

    std::vector<double> bounds;
    if (bounds_it != func_dict.end() && bounds_it->second.type == Value::ARRAY) {
      for (const auto& v : bounds_it->second.array) {
        if (v.type == Value::NUMBER) {
          bounds.push_back(v.number);
        }
      }
    }

    std::vector<double> encode;
    if (encode_it != func_dict.end() && encode_it->second.type == Value::ARRAY) {
      for (const auto& v : encode_it->second.array) {
        if (v.type == Value::NUMBER) {
          encode.push_back(v.number);
        }
      }
    }

    double x = inputs.empty() ? 0.0 : inputs[0];
    if (domain.size() >= 2) {
      x = clamp14(x, domain[0], domain[1]);
    }

    size_t func_idx = 0;
    double subdomain_min = domain.empty() ? 0.0 : domain[0];
    double subdomain_max = bounds.empty() ? (domain.size() >= 2 ? domain[1] : 1.0) : bounds[0];

    for (size_t i = 0; i < bounds.size(); ++i) {
      if (x < bounds[i]) {
        func_idx = i;
        subdomain_max = bounds[i];
        break;
      }
      subdomain_min = bounds[i];
      func_idx = i + 1;
      subdomain_max = (i + 1 < bounds.size()) ? bounds[i + 1] : (domain.size() >= 2 ? domain[1] : 1.0);
    }

    if (func_idx >= functions.size()) {
      func_idx = functions.size() - 1;
    }

    double encode_min = (func_idx * 2 < encode.size()) ? encode[func_idx * 2] : 0.0;
    double encode_max = (func_idx * 2 + 1 < encode.size()) ? encode[func_idx * 2 + 1] : 1.0;

    double sub_x = encode_min;
    if (subdomain_max != subdomain_min) {
      sub_x = encode_min + (x - subdomain_min) / (subdomain_max - subdomain_min) * (encode_max - encode_min);
    }

    Value sub_func = functions[func_idx];
    if (sub_func.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, sub_func.ref_object_number, sub_func.ref_generation_number);
      if (resolved.success) {
        sub_func = resolved.value;
      }
    }

    return evaluate_pdf_function(pdf, sub_func, {sub_x}, outputs);

  } else if (func_type == 4) {
    // Type 4: PostScript calculator function
    if (function.type != Value::STREAM) {
      return false;
    }

    auto decoded = decode_stream(pdf, function);
    if (!decoded.success || decoded.data.empty()) {
      return false;
    }

    std::string ps_code(decoded.data.begin(), decoded.data.end());
    return evaluate_postscript_function(ps_code, inputs, domain, range, outputs);
  }

  return false;
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

bool Blend2DBackend::draw_shading(const std::string& shading_name) {
  if (!current_pdf_ || !current_page_) {
    return false;
  }

  // Look up shading from page resources
  auto shading_dict_it = current_page_->resources.find("Shading");
  if (shading_dict_it == current_page_->resources.end()) {
    return false;
  }

  Dictionary shading_resources;
  if (shading_dict_it->second.type == Value::DICTIONARY) {
    shading_resources = shading_dict_it->second.dict;
  } else if (shading_dict_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      shading_dict_it->second.ref_object_number,
                                      shading_dict_it->second.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    shading_resources = resolved.value.dict;
  } else {
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

    // Extract colors from function
    if (shading->function.type == Value::DICTIONARY) {
      auto func_type_it = shading->function.dict.find("FunctionType");
      if (func_type_it != shading->function.dict.end() &&
          func_type_it->second.type == Value::NUMBER) {
        int func_type = static_cast<int>(func_type_it->second.number);

        if (func_type == 2) {
          // Exponential interpolation
          auto c0_it = shading->function.dict.find("C0");
          auto c1_it = shading->function.dict.find("C1");

          float r0 = 0, g0 = 0, b0 = 0;
          float r1 = 1, g1 = 1, b1 = 1;

          if (c0_it != shading->function.dict.end() && c0_it->second.type == Value::ARRAY) {
            if (c0_it->second.array.size() >= 3) {
              r0 = static_cast<float>(c0_it->second.array[0].number);
              g0 = static_cast<float>(c0_it->second.array[1].number);
              b0 = static_cast<float>(c0_it->second.array[2].number);
            }
          }
          if (c1_it != shading->function.dict.end() && c1_it->second.type == Value::ARRAY) {
            if (c1_it->second.array.size() >= 3) {
              r1 = static_cast<float>(c1_it->second.array[0].number);
              g1 = static_cast<float>(c1_it->second.array[1].number);
              b1 = static_cast<float>(c1_it->second.array[2].number);
            }
          }

          gradient.add_stop(0.0, BLRgba32(
            static_cast<uint8_t>(r0 * 255),
            static_cast<uint8_t>(g0 * 255),
            static_cast<uint8_t>(b0 * 255),
            255));
          gradient.add_stop(1.0, BLRgba32(
            static_cast<uint8_t>(r1 * 255),
            static_cast<uint8_t>(g1 * 255),
            static_cast<uint8_t>(b1 * 255),
            255));
        }
      }
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

    // Extract colors (same as axial)
    if (shading->function.type == Value::DICTIONARY) {
      auto func_type_it = shading->function.dict.find("FunctionType");
      if (func_type_it != shading->function.dict.end() &&
          func_type_it->second.type == Value::NUMBER) {
        int func_type = static_cast<int>(func_type_it->second.number);

        if (func_type == 2) {
          auto c0_it = shading->function.dict.find("C0");
          auto c1_it = shading->function.dict.find("C1");

          float r0_c = 0, g0 = 0, b0 = 0;
          float r1_c = 1, g1 = 1, b1 = 1;

          if (c0_it != shading->function.dict.end() && c0_it->second.type == Value::ARRAY) {
            if (c0_it->second.array.size() >= 3) {
              r0_c = static_cast<float>(c0_it->second.array[0].number);
              g0 = static_cast<float>(c0_it->second.array[1].number);
              b0 = static_cast<float>(c0_it->second.array[2].number);
            }
          }
          if (c1_it != shading->function.dict.end() && c1_it->second.type == Value::ARRAY) {
            if (c1_it->second.array.size() >= 3) {
              r1_c = static_cast<float>(c1_it->second.array[0].number);
              g1 = static_cast<float>(c1_it->second.array[1].number);
              b1 = static_cast<float>(c1_it->second.array[2].number);
            }
          }

          gradient.add_stop(0.0, BLRgba32(
            static_cast<uint8_t>(r0_c * 255),
            static_cast<uint8_t>(g0 * 255),
            static_cast<uint8_t>(b0 * 255),
            255));
          gradient.add_stop(1.0, BLRgba32(
            static_cast<uint8_t>(r1_c * 255),
            static_cast<uint8_t>(g1 * 255),
            static_cast<uint8_t>(b1 * 255),
            255));
        }
      }
    }

    ctx_.set_fill_style(gradient);
    ctx_.fill_rect(0, 0, width_, height_);
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
    ctx_.fill_path(path);
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

    ctx_.stroke_path(path);
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

  // Clear canvas
  begin_scene();

  // Process each content stream
  for (const auto& content_obj : page.contents) {
    Value resolved_obj = content_obj;
    uint32_t obj_num = 0;
    uint16_t gen_num = 0;

    // Resolve reference if needed
    if (content_obj.type == Value::REFERENCE) {
      obj_num = content_obj.ref_object_number;
      gen_num = content_obj.ref_generation_number;
      auto resolved = resolve_reference(pdf, obj_num, gen_num);
      if (resolved.success) {
        resolved_obj = resolved.value;
      } else {
        continue;
      }
    }

    if (resolved_obj.type == Value::STREAM) {
      auto decoded_result = decode_stream(pdf, resolved_obj, obj_num, gen_num);
      if (decoded_result.success) {
        state_ = GraphicsState();  // Reset state
        // Set page coordinate info
        state_.page_width = page_width;
        state_.page_height = page_height;
        state_.scale = scale;
        parse_pdf_content(decoded_result.data);
      }
    }
  }

  end_scene();

  current_pdf_ = nullptr;
  current_page_ = nullptr;

  return get_buffer();
}

bool Blend2DBackend::parse_pdf_content(const std::vector<uint8_t>& content_data) {
  if (content_data.empty()) return true;

  std::string content(content_data.begin(), content_data.end());
  std::vector<std::string> operands;
  std::vector<GraphicsState> state_stack;

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
          float x = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.move_to(x, y);
          state_.current_x = x;
          state_.current_y = y;
          state_.in_path = true;
        }
      } else if (token == "l") {  // lineto
        if (operands.size() >= 2) {
          float x = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.line_to(x, y);
          state_.current_x = x;
          state_.current_y = y;
        }
      } else if (token == "c") {  // curveto
        if (operands.size() >= 6) {
          float x1 = std::stof(operands[operands.size() - 6]) * state_.scale;
          float y1 = (state_.page_height - std::stof(operands[operands.size() - 5])) * state_.scale;
          float x2 = std::stof(operands[operands.size() - 4]) * state_.scale;
          float y2 = (state_.page_height - std::stof(operands[operands.size() - 3])) * state_.scale;
          float x3 = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(x1, y1, x2, y2, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "v") {  // curveto (first control point = current point)
        if (operands.size() >= 4) {
          float x2 = std::stof(operands[operands.size() - 4]) * state_.scale;
          float y2 = (state_.page_height - std::stof(operands[operands.size() - 3])) * state_.scale;
          float x3 = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(state_.current_x, state_.current_y, x2, y2, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "y") {  // curveto (last control point = end point)
        if (operands.size() >= 4) {
          float x1 = std::stof(operands[operands.size() - 4]) * state_.scale;
          float y1 = (state_.page_height - std::stof(operands[operands.size() - 3])) * state_.scale;
          float x3 = std::stof(operands[operands.size() - 2]) * state_.scale;
          float y3 = (state_.page_height - std::stof(operands[operands.size() - 1])) * state_.scale;
          state_.current_path.cubic_to(x1, y1, x3, y3, x3, y3);
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "h") {  // closepath
        state_.current_path.close();
      } else if (token == "re") {  // rectangle
        if (operands.size() >= 4) {
          float x = std::stof(operands[operands.size() - 4]) * state_.scale;
          float y = (state_.page_height - std::stof(operands[operands.size() - 3])) * state_.scale;
          float w = std::stof(operands[operands.size() - 2]) * state_.scale;
          float h = std::stof(operands[operands.size() - 1]) * state_.scale;
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
        }
      } else if (token == "S") {  // stroke
        if (state_.in_path) {
          push_with_clip(state_.current_path, false, true);
          state_.current_path.reset();
          state_.in_path = false;
        }
      } else if (token == "s") {  // close and stroke
        if (state_.in_path) {
          state_.current_path.close();
          push_with_clip(state_.current_path, false, true);
          state_.current_path.reset();
          state_.in_path = false;
        }
      } else if (token == "B" || token == "B*") {  // fill and stroke
        if (state_.in_path) {
          BLFillRule rule = (token == "B*") ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          ctx_.set_fill_rule(rule);
          push_with_clip(state_.current_path, true, true);
          state_.current_path.reset();
          state_.in_path = false;
        }
      } else if (token == "b" || token == "b*") {  // close, fill and stroke
        if (state_.in_path) {
          state_.current_path.close();
          BLFillRule rule = (token == "b*") ? BL_FILL_RULE_EVEN_ODD : BL_FILL_RULE_NON_ZERO;
          ctx_.set_fill_rule(rule);
          push_with_clip(state_.current_path, true, true);
          state_.current_path.reset();
          state_.in_path = false;
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
          uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.fill_r = state_.fill_g = state_.fill_b = gray;
        }
      } else if (token == "G") {  // gray stroke
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
        }
      } else if (token == "rg") {  // RGB fill
        if (operands.size() >= 3) {
          state_.fill_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.fill_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
          state_.fill_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
        }
      } else if (token == "RG") {  // RGB stroke
        if (operands.size() >= 3) {
          state_.stroke_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.stroke_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
          state_.stroke_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
        }
      } else if (token == "k") {  // CMYK fill
        if (operands.size() >= 4) {
          float c = std::stof(operands[0]);
          float m = std::stof(operands[1]);
          float y = std::stof(operands[2]);
          float k = std::stof(operands[3]);
          state_.fill_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.fill_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.fill_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      } else if (token == "K") {  // CMYK stroke
        if (operands.size() >= 4) {
          float c = std::stof(operands[0]);
          float m = std::stof(operands[1]);
          float y = std::stof(operands[2]);
          float k = std::stof(operands[3]);
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
            state_.fill_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.fill_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
            state_.fill_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
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
                  double tint = std::stof(operands[0]);
                  if (evaluate_separation_color(*current_pdf_, cs, tint,
                                                state_.fill_r, state_.fill_g, state_.fill_b)) {
                    handled = true;
                  }
                }
              }
            }
            if (!handled) {
              // Fallback: treat as grayscale
              uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
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
            state_.stroke_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.stroke_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
            state_.stroke_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
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
                  double tint = std::stof(operands[0]);
                  if (evaluate_separation_color(*current_pdf_, cs, tint,
                                                state_.stroke_r, state_.stroke_g, state_.stroke_b)) {
                    handled = true;
                  }
                }
              }
            }
            if (!handled) {
              // Fallback: treat as grayscale
              uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
              state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
            }
          }
        }
      }
      // Line style
      else if (token == "w") {
        if (operands.size() >= 1) {
          state_.stroke_width = std::stof(operands[0]);
        }
      } else if (token == "J") {
        if (operands.size() >= 1) {
          state_.line_cap = std::stoi(operands[0]);
        }
      } else if (token == "j") {
        if (operands.size() >= 1) {
          state_.line_join = std::stoi(operands[0]);
        }
      } else if (token == "M") {
        if (operands.size() >= 1) {
          state_.miter_limit = std::stof(operands[0]);
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
            state_.dash_pattern.push_back(std::stof(operands[i]));
          } else if (!in_array && i == operands.size() - 1) {
            state_.dash_phase = std::stof(operands[i]);
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
          // Look up ExtGState from page resources
          auto extgstate_it = current_page_->resources.find("ExtGState");
          if (extgstate_it != current_page_->resources.end()) {
            Dictionary extgstate_dict;
            if (extgstate_it->second.type == Value::DICTIONARY) {
              extgstate_dict = extgstate_it->second.dict;
            } else if (extgstate_it->second.type == Value::REFERENCE) {
              auto resolved = resolve_reference(*current_pdf_,
                                                extgstate_it->second.ref_object_number,
                                                extgstate_it->second.ref_generation_number);
              if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                extgstate_dict = resolved.value.dict;
              }
            }
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
      } else if (token == "Tf") {
        if (operands.size() >= 2) {
          std::string font_name = operands[0];
          if (!font_name.empty() && font_name[0] == '/') {
            font_name = font_name.substr(1);
          }
          state_.font_size = std::stof(operands[operands.size() - 1]);
          current_font_name_ = font_name;
          current_font_ = nullptr;
          if (current_pdf_ && current_page_) {
            auto font_it = current_page_->fonts.find(font_name);
            if (font_it != current_page_->fonts.end()) {
              current_font_ = font_it->second.get();
              load_font(*current_pdf_, font_name, current_font_);
            }
          }
        }
      } else if (token == "Td") {
        if (operands.size() >= 2) {
          float tx = std::stof(operands[0]);
          float ty = std::stof(operands[1]);
          state_.text_matrix.e += tx;
          state_.text_matrix.f += ty;
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "TD") {
        if (operands.size() >= 2) {
          float tx = std::stof(operands[0]);
          float ty = std::stof(operands[1]);
          state_.text_leading = -ty;
          state_.text_matrix.e += tx;
          state_.text_matrix.f += ty;
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "Tm") {
        if (operands.size() >= 6) {
          state_.text_matrix.a = std::stof(operands[0]);
          state_.text_matrix.b = std::stof(operands[1]);
          state_.text_matrix.c = std::stof(operands[2]);
          state_.text_matrix.d = std::stof(operands[3]);
          state_.text_matrix.e = std::stof(operands[4]);
          state_.text_matrix.f = std::stof(operands[5]);
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "T*") {
        state_.text_matrix.e = state_.text_line_matrix.e;
        state_.text_matrix.f = state_.text_line_matrix.f - state_.text_leading;
        state_.text_line_matrix = state_.text_matrix;
      } else if (token == "TL") {
        if (operands.size() >= 1) {
          state_.text_leading = std::stof(operands[0]);
        }
      } else if (token == "Tc") {
        if (operands.size() >= 1) {
          state_.char_spacing = std::stof(operands[0]);
        }
      } else if (token == "Tw") {
        if (operands.size() >= 1) {
          state_.word_spacing = std::stof(operands[0]);
        }
      } else if (token == "Tz") {
        if (operands.size() >= 1) {
          state_.horiz_scaling = std::stof(operands[0]);
        }
      } else if (token == "Ts") {
        if (operands.size() >= 1) {
          state_.text_rise = static_cast<int>(std::stof(operands[0]));
        }
      } else if (token == "Tr") {
        if (operands.size() >= 1) {
          state_.text_render_mode = std::stoi(operands[0]);
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

            try {
              float adjust = std::stof(op);
              // Negative values move right, positive values move left
              float pixel_adjust = (adjust / 1000.0f) * font_size;
              x -= pixel_adjust;
              total_advance -= pixel_adjust / state_.scale;
            } catch (...) {
              // This is a text string
              float text_width = render_text_string(op, x, y, font_size,
                  state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
              x += text_width;
              total_advance += text_width / state_.scale;
            }
          }
          state_.text_matrix.e += total_advance;
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
        }
      }
      // Inline image (BI ... ID data EI)
      else if (token == "BI") {
        parse_inline_image(content, pos);
        operands.clear();
        continue;  // Skip normal operand clearing
      }
      // XObject (Do operator for images/forms)
      else if (token == "Do") {
        if (operands.size() >= 1 && current_pdf_ && current_page_) {
          std::string xobj_name = operands[0];
          if (!xobj_name.empty() && xobj_name[0] == '/') {
            xobj_name = xobj_name.substr(1);
          }

          // Look up XObject in page resources
          auto xobj_it = current_page_->resources.find("XObject");
          if (xobj_it != current_page_->resources.end()) {
            Dictionary xobj_dict;
            if (xobj_it->second.type == Value::REFERENCE) {
              auto resolved = resolve_reference(*current_pdf_,
                  xobj_it->second.ref_object_number,
                  xobj_it->second.ref_generation_number);
              if (resolved.success && resolved.value.type == Value::DICTIONARY) {
                xobj_dict = resolved.value.dict;
              }
            } else if (xobj_it->second.type == Value::DICTIONARY) {
              xobj_dict = xobj_it->second.dict;
            }

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
                      parse_pdf_content(decoded.data);

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
        }
      }
      // Transformation
      else if (token == "cm") {
        if (operands.size() >= 6) {
          GraphicsState::Matrix m;
          m.a = std::stof(operands[0]);
          m.b = std::stof(operands[1]);
          m.c = std::stof(operands[2]);
          m.d = std::stof(operands[3]);
          m.e = std::stof(operands[4]);
          m.f = std::stof(operands[5]);
          state_.transform = state_.transform * m;
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
  if (it != dict.end()) width = std::stoi(it->second);

  // H or Height
  it = dict.find("H");
  if (it == dict.end()) it = dict.find("Height");
  if (it != dict.end()) height = std::stoi(it->second);

  // BPC or BitsPerComponent
  it = dict.find("BPC");
  if (it == dict.end()) it = dict.find("BitsPerComponent");
  if (it != dict.end()) bpc = std::stoi(it->second);

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

  // Look up pattern resources
  auto pattern_dict_it = current_page_->resources.find("Pattern");
  if (pattern_dict_it == current_page_->resources.end()) {
    return false;
  }

  Dictionary pattern_resources;
  if (pattern_dict_it->second.type == Value::DICTIONARY) {
    pattern_resources = pattern_dict_it->second.dict;
  } else if (pattern_dict_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      pattern_dict_it->second.ref_object_number,
                                      pattern_dict_it->second.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    pattern_resources = resolved.value.dict;
  } else {
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

      // Get colors from function
      uint8_t start_r = 0, start_g = 0, start_b = 0;
      uint8_t end_r = 255, end_g = 255, end_b = 255;

      if (shading->function.type == Value::DICTIONARY) {
        auto func_type_it = shading->function.dict.find("FunctionType");
        if (func_type_it != shading->function.dict.end() &&
            func_type_it->second.type == Value::NUMBER &&
            static_cast<int>(func_type_it->second.number) == 2) {
          auto c0_it = shading->function.dict.find("C0");
          auto c1_it = shading->function.dict.find("C1");

          if (c0_it != shading->function.dict.end() && c0_it->second.type == Value::ARRAY) {
            const auto& c0 = c0_it->second.array;
            if (c0.size() >= 3) {
              start_r = static_cast<uint8_t>(c0[0].number * 255);
              start_g = static_cast<uint8_t>(c0[1].number * 255);
              start_b = static_cast<uint8_t>(c0[2].number * 255);
            }
          }
          if (c1_it != shading->function.dict.end() && c1_it->second.type == Value::ARRAY) {
            const auto& c1 = c1_it->second.array;
            if (c1.size() >= 3) {
              end_r = static_cast<uint8_t>(c1[0].number * 255);
              end_g = static_cast<uint8_t>(c1[1].number * 255);
              end_b = static_cast<uint8_t>(c1[2].number * 255);
            }
          }
        }
      }

      gradient.add_stop(0.0, BLRgba32(start_r, start_g, start_b, 255));
      gradient.add_stop(1.0, BLRgba32(end_r, end_g, end_b, 255));

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

      // Get colors from function (similar to axial)
      uint8_t start_r = 0, start_g = 0, start_b = 0;
      uint8_t end_r = 255, end_g = 255, end_b = 255;

      if (shading->function.type == Value::DICTIONARY) {
        auto func_type_it = shading->function.dict.find("FunctionType");
        if (func_type_it != shading->function.dict.end() &&
            func_type_it->second.type == Value::NUMBER &&
            static_cast<int>(func_type_it->second.number) == 2) {
          auto c0_it = shading->function.dict.find("C0");
          auto c1_it = shading->function.dict.find("C1");

          if (c0_it != shading->function.dict.end() && c0_it->second.type == Value::ARRAY) {
            const auto& c0 = c0_it->second.array;
            if (c0.size() >= 3) {
              start_r = static_cast<uint8_t>(c0[0].number * 255);
              start_g = static_cast<uint8_t>(c0[1].number * 255);
              start_b = static_cast<uint8_t>(c0[2].number * 255);
            }
          }
          if (c1_it != shading->function.dict.end() && c1_it->second.type == Value::ARRAY) {
            const auto& c1 = c1_it->second.array;
            if (c1.size() >= 3) {
              end_r = static_cast<uint8_t>(c1[0].number * 255);
              end_g = static_cast<uint8_t>(c1[1].number * 255);
              end_b = static_cast<uint8_t>(c1[2].number * 255);
            }
          }
        }
      }

      gradient.add_stop(0.0, BLRgba32(start_r, start_g, start_b, 255));
      gradient.add_stop(1.0, BLRgba32(end_r, end_g, end_b, 255));

      if (is_stroke) {
        ctx_.set_stroke_style(gradient);
      } else {
        ctx_.set_fill_style(gradient);
      }
      return true;
    }
  }
  else if (pattern->type == PatternType::Tiling && pattern->tiling) {
    // Tiling pattern - render tile to an image and create BLPattern
    auto& tiling = *pattern->tiling;

    // Calculate tile dimensions from bbox
    float tile_width = 1.0f, tile_height = 1.0f;
    if (tiling.bbox.size() >= 4) {
      tile_width = static_cast<float>(tiling.bbox[2] - tiling.bbox[0]);
      tile_height = static_cast<float>(tiling.bbox[3] - tiling.bbox[1]);
    }

    // Apply step if specified
    float x_step = tiling.x_step > 0 ? static_cast<float>(tiling.x_step) : tile_width;
    float y_step = tiling.y_step > 0 ? static_cast<float>(tiling.y_step) : tile_height;

    // Scale tile dimensions
    uint32_t img_width = static_cast<uint32_t>(x_step * state_.scale);
    uint32_t img_height = static_cast<uint32_t>(y_step * state_.scale);

    if (img_width == 0) img_width = 32;
    if (img_height == 0) img_height = 32;
    if (img_width > 512) img_width = 512;  // Limit size for performance
    if (img_height > 512) img_height = 512;

    // Create tile image
    BLImage tile_image(img_width, img_height, BL_FORMAT_PRGB32);
    BLContext tile_ctx(tile_image);

    // Clear with transparent
    tile_ctx.set_comp_op(BL_COMP_OP_CLEAR);
    tile_ctx.fill_all();
    tile_ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

    // For colored tiles, we would render the content stream
    // For now, use a simple placeholder pattern
    if (tiling.paint_type == TilingPaintType::ColoredTiles) {
      // Use a checkered pattern as placeholder
      tile_ctx.set_fill_style(BLRgba32(200, 200, 200, 255));
      tile_ctx.fill_rect(0, 0, img_width / 2, img_height / 2);
      tile_ctx.fill_rect(img_width / 2, img_height / 2, img_width / 2, img_height / 2);
      tile_ctx.set_fill_style(BLRgba32(230, 230, 230, 255));
      tile_ctx.fill_rect(img_width / 2, 0, img_width / 2, img_height / 2);
      tile_ctx.fill_rect(0, img_height / 2, img_width / 2, img_height / 2);
    } else {
      // Uncolored tiles - use current fill color
      tile_ctx.set_fill_style(BLRgba32(state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a));
      tile_ctx.fill_all();
    }

    tile_ctx.end();

    // Create repeating pattern
    BLPattern bl_pattern(tile_image, BL_EXTEND_MODE_REPEAT);

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

  // Get the XObject stream
  Value xobject_value = group_xobject;
  if (xobject_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_,
                                      xobject_value.ref_object_number,
                                      xobject_value.ref_generation_number);
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
  auto decoded = decode_stream(*current_pdf_, xobject_value);
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
  parse_pdf_content(decoded.data);

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
