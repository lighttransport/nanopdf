// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#ifdef NANOPDF_USE_THORVG

#include "thorvg-backend.hh"
#include <cmath>
#include <fstream>
#include <sstream>

#include "nanopdf-log.hh"
#include "color-transform.hh"

// For PNG saving - implementation in stb_image_write_impl.cc
#include "stb_image_write.h"

// External C variable for PNG compression level
extern "C" int stbi_write_png_compression_level;

namespace nanopdf {

// C++14 compatible clamp (std::clamp is C++17)
template<typename T>
constexpr T clamp14(const T& v, const T& lo, const T& hi) {
  return (v < lo) ? lo : (hi < v) ? hi : v;
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
    0xFF33, 0xFF34, 0xFF35, 0xFF36, 0xFF37, 0xFF38, 0xFF39, 0xFF3A, 0xFF3B, 0xFFE5,
    0xFF3D, 0xFF3E, 0xFF3F, 0xFF40, 0xFF41, 0xFF42, 0xFF43, 0xFF44, 0xFF45, 0xFF46,
    0xFF47, 0xFF48, 0xFF49, 0xFF4A, 0xFF4B, 0xFF4C, 0xFF4D, 0xFF4E, 0xFF4F, 0xFF50,
    0xFF51, 0xFF52, 0xFF53, 0xFF54, 0xFF55, 0xFF56, 0xFF57, 0xFF58, 0xFF59, 0xFF5A,
    0xFF5B, 0xFF5C, 0xFF5D
  };
  if (cid >= 2 && cid <= 94) {
    return cid_2_94[cid - 2];
  }

  // Hiragana (CID 842-924 -> U+3041-U+3093)
  if (cid >= 842 && cid <= 924) {
    return 0x3041 + (cid - 842);
  }

  // Katakana (CID 925-1010 -> U+30A1-U+30F6)
  if (cid >= 925 && cid <= 1010) {
    return 0x30A1 + (cid - 925);
  }

  // Common punctuation and symbols
  static const std::map<uint32_t, uint32_t> punct_map = {
    {631, 0x3001},  // 、 IDEOGRAPHIC COMMA
    {632, 0x3002},  // 。 IDEOGRAPHIC FULL STOP
    {633, 0xFF0C},  // ， FULLWIDTH COMMA
    {634, 0xFF0E},  // ． FULLWIDTH FULL STOP
    {635, 0x30FB},  // ・ KATAKANA MIDDLE DOT
    {636, 0xFF1A},  // ： FULLWIDTH COLON
    {637, 0xFF1B},  // ； FULLWIDTH SEMICOLON
    {638, 0x3059},  // す
    {639, 0xFF1F},  // ？ FULLWIDTH QUESTION MARK
    {640, 0xFF01},  // ！ FULLWIDTH EXCLAMATION MARK
    {674, 0x306E},  // の
    {675, 0x306F},  // は
    {676, 0x3070},  // ば
    {677, 0x3071},  // ぱ
    {678, 0x3072},  // ひ
    {720, 0x300C},  // 「 LEFT CORNER BRACKET
    {721, 0x300D},  // 」 RIGHT CORNER BRACKET
    {722, 0x300E},  // 『 LEFT WHITE CORNER BRACKET
    {723, 0x300F},  // 』 RIGHT WHITE CORNER BRACKET
    {724, 0xFF08},  // （ FULLWIDTH LEFT PARENTHESIS
    {725, 0xFF09},  // ） FULLWIDTH RIGHT PARENTHESIS
    {783, 0x30C8},  // ト
    {803, 0x30E9},  // ラ
  };

  auto punct_it = punct_map.find(cid);
  if (punct_it != punct_map.end()) {
    return punct_it->second;
  }

  // Common Kanji mappings (subset of most frequent characters)
  static const std::map<uint32_t, uint32_t> kanji_map = {
    // Numbers/counting
    {1866, 0x4E00}, // 一
    {1867, 0x4E8C}, // 二
    {1868, 0x4E09}, // 三
    {1869, 0x56DB}, // 四
    {1870, 0x4E94}, // 五
    {1871, 0x516D}, // 六
    {1872, 0x4E03}, // 七
    {1873, 0x516B}, // 八
    {1874, 0x4E5D}, // 九
    {1875, 0x5341}, // 十
    {1876, 0x767E}, // 百
    {1877, 0x5343}, // 千
    {1878, 0x4E07}, // 万
    {1879, 0x5104}, // 億
    // Common characters
    {1908, 0x5927}, // 大
    {1909, 0x5C0F}, // 小
    {1910, 0x4E2D}, // 中
    {1911, 0x9AD8}, // 高
    {1912, 0x4F4E}, // 低
    {1913, 0x65B0}, // 新
    {1914, 0x53E4}, // 古
    {1915, 0x826F}, // 良
    {1916, 0x60AA}, // 悪
    {1917, 0x591A}, // 多
    {1918, 0x5C11}, // 少
    {1919, 0x5168}, // 全
    {1920, 0x534A}, // 半
    {1921, 0x6700}, // 最
    {1922, 0x521D}, // 初
    {1923, 0x5F8C}, // 後
    {1924, 0x524D}, // 前
    {1925, 0x6B21}, // 次
    {1926, 0x4ECA}, // 今
    {1927, 0x6628}, // 昨
    {1928, 0x660E}, // 明
    {1929, 0x65E5}, // 日
    {1930, 0x6708}, // 月
    {1931, 0x5E74}, // 年
    {1932, 0x6642}, // 時
    {1933, 0x5206}, // 分
    {1934, 0x79D2}, // 秒
    {1935, 0x9031}, // 週
    {1936, 0x66DC}, // 曜
    {1937, 0x706B}, // 火
    {1938, 0x6C34}, // 水
    {1939, 0x6728}, // 木
    {1940, 0x91D1}, // 金
    {1941, 0x571F}, // 土
    {1942, 0x65E5}, // 日
    {1943, 0x6625}, // 春
    {1944, 0x590F}, // 夏
    {1945, 0x79CB}, // 秋
    {1946, 0x51AC}, // 冬
    {1947, 0x6771}, // 東
    {1948, 0x897F}, // 西
    {1949, 0x5357}, // 南
    {1950, 0x5317}, // 北
    {1951, 0x4E0A}, // 上
    {1952, 0x8A00}, // 言
    {1953, 0x4E0B}, // 下
    {1954, 0x5DE6}, // 左
    {1955, 0x53F3}, // 右
    {1956, 0x5185}, // 内
    {1957, 0x5916}, // 外
    // More common kanji
    {2028, 0x4EBA}, // 人
    {2029, 0x5B50}, // 子
    {2030, 0x5973}, // 女
    {2031, 0x7537}, // 男
    {2032, 0x7236}, // 父
    {2033, 0x6BCD}, // 母
    {2034, 0x5144}, // 兄
    {2035, 0x59C9}, // 姉
    {2036, 0x5F1F}, // 弟
    {2037, 0x59B9}, // 妹
    {2038, 0x592B}, // 夫
    {2039, 0x59BB}, // 妻
    {2040, 0x53CB}, // 友
    {2041, 0x5148}, // 先
    {2042, 0x751F}, // 生
    {2043, 0x5B66}, // 学
    {2044, 0x6821}, // 校
    {2045, 0x793E}, // 社
    {2046, 0x4F1A}, // 会
    {2047, 0x56FD}, // 国
    {2048, 0x5E02}, // 市
    {2049, 0x753A}, // 町
    {2050, 0x6751}, // 村
    {2051, 0x5C71}, // 山
    {2052, 0x5DDD}, // 川
    {2053, 0x6D77}, // 海
    {2054, 0x7A7A}, // 空
    {2055, 0x5730}, // 地
    {2056, 0x5929}, // 天
    {2057, 0x98A8}, // 風
    {2058, 0x96E8}, // 雨
    {2059, 0x96EA}, // 雪
    {2060, 0x82B1}, // 花
    {2061, 0x8349}, // 草
    {2062, 0x6728}, // 木
    {2063, 0x68EE}, // 森
    {2064, 0x6797}, // 林
    // Language/expression kanji
    {2248, 0x80FD}, // 能
    {2269, 0x660E}, // 明
    // More kanji
    {2890, 0x8A9E}, // 語
    {2956, 0x529B}, // 力
    {3592, 0x8A66}, // 試
    {3824, 0x65E5}, // 日
    {4011, 0x672C}, // 本
    {4782, 0x8A9E}, // 語
    // Additional common kanji
    {2100, 0x898B}, // 見
    {2101, 0x805E}, // 聞
    {2102, 0x8AAD}, // 読
    {2103, 0x66F8}, // 書
    {2104, 0x8A71}, // 話
    {2105, 0x601D}, // 思
    {2106, 0x8003}, // 考
    {2107, 0x77E5}, // 知
    {2108, 0x4F7F}, // 使
    {2109, 0x4F5C}, // 作
    {2110, 0x52D5}, // 動
    {2111, 0x6B62}, // 止
    {2112, 0x884C}, // 行
    {2113, 0x6765}, // 来
    {2114, 0x5E30}, // 帰
    {2115, 0x5165}, // 入
    {2116, 0x51FA}, // 出
    {2117, 0x7ACB}, // 立
    {2118, 0x5EA7}, // 座
    {2119, 0x98DF}, // 食
    {2120, 0x98F2}, // 飲
    {2121, 0x7720}, // 睡
    {2122, 0x8D77}, // 起
    {2123, 0x5BC4}, // 寄
    {2124, 0x4E57}, // 乗
    {2125, 0x964D}, // 降
    {2126, 0x8D70}, // 走
    {2127, 0x6B69}, // 歩
    {2128, 0x6CF3}, // 泳
    {2129, 0x98DB}, // 飛
    {2130, 0x843D}, // 落
    // Common verbs and adjectives
    {2150, 0x3042}, // あ (hiragana a - sometimes mapped here)
    {2151, 0x3044}, // い
    {2152, 0x3046}, // う
    {2153, 0x3048}, // え
    {2154, 0x304A}, // お
    // Additional important kanji
    {2197, 0x793A}, // 示
    {2198, 0x4F1D}, // 伝
    {2199, 0x5831}, // 報
    {2200, 0x77E5}, // 知
  };

  auto it = kanji_map.find(cid);
  if (it != kanji_map.end()) {
    return it->second;
  }

  // Not found - return 0 to indicate no mapping
  return 0;
}

// Common Adobe glyph name to Unicode mapping
static uint32_t glyph_name_to_unicode(const std::string& name) {
  // Most common glyph names - add more as needed
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
    {"A", 0x0041}, {"B", 0x0042}, {"C", 0x0043}, {"D", 0x0044},
    {"E", 0x0045}, {"F", 0x0046}, {"G", 0x0047}, {"H", 0x0048},
    {"I", 0x0049}, {"J", 0x004A}, {"K", 0x004B}, {"L", 0x004C},
    {"M", 0x004D}, {"N", 0x004E}, {"O", 0x004F}, {"P", 0x0050},
    {"Q", 0x0051}, {"R", 0x0052}, {"S", 0x0053}, {"T", 0x0054},
    {"U", 0x0055}, {"V", 0x0056}, {"W", 0x0057}, {"X", 0x0058},
    {"Y", 0x0059}, {"Z", 0x005A}, {"bracketleft", 0x005B},
    {"backslash", 0x005C}, {"bracketright", 0x005D}, {"asciicircum", 0x005E},
    {"underscore", 0x005F}, {"grave", 0x0060},
    {"a", 0x0061}, {"b", 0x0062}, {"c", 0x0063}, {"d", 0x0064},
    {"e", 0x0065}, {"f", 0x0066}, {"g", 0x0067}, {"h", 0x0068},
    {"i", 0x0069}, {"j", 0x006A}, {"k", 0x006B}, {"l", 0x006C},
    {"m", 0x006D}, {"n", 0x006E}, {"o", 0x006F}, {"p", 0x0070},
    {"q", 0x0071}, {"r", 0x0072}, {"s", 0x0073}, {"t", 0x0074},
    {"u", 0x0075}, {"v", 0x0076}, {"w", 0x0077}, {"x", 0x0078},
    {"y", 0x0079}, {"z", 0x007A}, {"braceleft", 0x007B},
    {"bar", 0x007C}, {"braceright", 0x007D}, {"asciitilde", 0x007E},
    // Extended Latin
    {"exclamdown", 0x00A1}, {"cent", 0x00A2}, {"sterling", 0x00A3},
    {"currency", 0x00A4}, {"yen", 0x00A5}, {"brokenbar", 0x00A6},
    {"section", 0x00A7}, {"dieresis", 0x00A8}, {"copyright", 0x00A9},
    {"ordfeminine", 0x00AA}, {"guillemotleft", 0x00AB},
    {"logicalnot", 0x00AC}, {"registered", 0x00AE}, {"macron", 0x00AF},
    {"degree", 0x00B0}, {"plusminus", 0x00B1}, {"twosuperior", 0x00B2},
    {"threesuperior", 0x00B3}, {"acute", 0x00B4}, {"mu", 0x00B5},
    {"paragraph", 0x00B6}, {"periodcentered", 0x00B7}, {"cedilla", 0x00B8},
    {"onesuperior", 0x00B9}, {"ordmasculine", 0x00BA},
    {"guillemotright", 0x00BB}, {"onequarter", 0x00BC},
    {"onehalf", 0x00BD}, {"threequarters", 0x00BE}, {"questiondown", 0x00BF},
    // Ligatures and special
    {"fi", 0xFB01}, {"fl", 0xFB02}, {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04},
    {"endash", 0x2013}, {"emdash", 0x2014},
    {"quoteleft", 0x2018}, {"quoteright", 0x2019},
    {"quotedblleft", 0x201C}, {"quotedblright", 0x201D},
    {"bullet", 0x2022}, {"ellipsis", 0x2026},
    {"dagger", 0x2020}, {"daggerdbl", 0x2021},
    {"perthousand", 0x2030}, {"trademark", 0x2122},
    // Accented letters
    {"Agrave", 0x00C0}, {"Aacute", 0x00C1}, {"Acircumflex", 0x00C2},
    {"Atilde", 0x00C3}, {"Adieresis", 0x00C4}, {"Aring", 0x00C5},
    {"AE", 0x00C6}, {"Ccedilla", 0x00C7}, {"Egrave", 0x00C8},
    {"Eacute", 0x00C9}, {"Ecircumflex", 0x00CA}, {"Edieresis", 0x00CB},
    {"Igrave", 0x00CC}, {"Iacute", 0x00CD}, {"Icircumflex", 0x00CE},
    {"Idieresis", 0x00CF}, {"Eth", 0x00D0}, {"Ntilde", 0x00D1},
    {"Ograve", 0x00D2}, {"Oacute", 0x00D3}, {"Ocircumflex", 0x00D4},
    {"Otilde", 0x00D5}, {"Odieresis", 0x00D6}, {"multiply", 0x00D7},
    {"Oslash", 0x00D8}, {"Ugrave", 0x00D9}, {"Uacute", 0x00DA},
    {"Ucircumflex", 0x00DB}, {"Udieresis", 0x00DC}, {"Yacute", 0x00DD},
    {"Thorn", 0x00DE}, {"germandbls", 0x00DF},
    {"agrave", 0x00E0}, {"aacute", 0x00E1}, {"acircumflex", 0x00E2},
    {"atilde", 0x00E3}, {"adieresis", 0x00E4}, {"aring", 0x00E5},
    {"ae", 0x00E6}, {"ccedilla", 0x00E7}, {"egrave", 0x00E8},
    {"eacute", 0x00E9}, {"ecircumflex", 0x00EA}, {"edieresis", 0x00EB},
    {"igrave", 0x00EC}, {"iacute", 0x00ED}, {"icircumflex", 0x00EE},
    {"idieresis", 0x00EF}, {"eth", 0x00F0}, {"ntilde", 0x00F1},
    {"ograve", 0x00F2}, {"oacute", 0x00F3}, {"ocircumflex", 0x00F4},
    {"otilde", 0x00F5}, {"odieresis", 0x00F6}, {"divide", 0x00F7},
    {"oslash", 0x00F8}, {"ugrave", 0x00F9}, {"uacute", 0x00FA},
    {"ucircumflex", 0x00FB}, {"udieresis", 0x00FC}, {"yacute", 0x00FD},
    {"thorn", 0x00FE}, {"ydieresis", 0x00FF},
    // More special characters
    {"Euro", 0x20AC}, {"florin", 0x0192},
    {"OE", 0x0152}, {"oe", 0x0153},
    {"Scaron", 0x0160}, {"scaron", 0x0161},
    {"Ydieresis", 0x0178}, {"Zcaron", 0x017D}, {"zcaron", 0x017E},
    {"circumflex", 0x02C6}, {"tilde", 0x02DC},
    {"dotlessi", 0x0131}, {"Lslash", 0x0141}, {"lslash", 0x0142},
    // Math and symbols
    {"minus", 0x2212}, {"fraction", 0x2044},
    {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A},
  };

  auto it = glyph_map.find(name);
  if (it != glyph_map.end()) {
    return it->second;
  }

  // Try uniXXXX format (e.g., "uni0041" = 'A')
  if (name.length() == 7 && name.substr(0, 3) == "uni") {
    try {
      return static_cast<uint32_t>(std::stoul(name.substr(3), nullptr, 16));
    } catch (...) {}
  }

  return 0;  // Not found
}

// ========================================================================
// Mesh Shading Support - Based on PDFium implementation
// Copyright 2016 The PDFium Authors
// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com
// ========================================================================

// BitStream reader for reading packed bits from mesh data
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

  void skip_bits(uint32_t num_bits) {
    bit_pos_ += num_bits;
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
static void draw_gouraud_triangle(uint32_t* bitmap, int width, int height,
                                   const MeshVertex& v0, const MeshVertex& v1, const MeshVertex& v2) {
  // Find bounding box
  float min_y = std::min({v0.y, v1.y, v2.y});
  float max_y = std::max({v0.y, v1.y, v2.y});

  if (min_y == max_y) return;  // Degenerate triangle

  int min_yi = std::max(static_cast<int>(std::floor(min_y)), 0);
  int max_yi = std::min(static_cast<int>(std::ceil(max_y)), height - 1);

  // For each scanline
  for (int y = min_yi; y <= max_yi; ++y) {
    float fy = static_cast<float>(y);

    // Find intersections with triangle edges
    struct Intersection {
      float x;
      float r, g, b;
    };
    std::vector<Intersection> intersections;

    // Check each edge
    const MeshVertex* edges[3][2] = {{&v0, &v1}, {&v1, &v2}, {&v2, &v0}};

    for (int i = 0; i < 3; ++i) {
      const MeshVertex* a = edges[i][0];
      const MeshVertex* b = edges[i][1];

      if (a->y == b->y) continue;  // Horizontal edge

      // Check if scanline intersects this edge
      bool intersects = (a->y < b->y) ? (fy >= a->y && fy <= b->y) : (fy >= b->y && fy <= a->y);
      if (!intersects) continue;

      // Interpolate along edge
      float t = (fy - a->y) / (b->y - a->y);
      Intersection isect;
      isect.x = a->x + t * (b->x - a->x);
      isect.r = a->r + t * (b->r - a->r);
      isect.g = a->g + t * (b->g - a->g);
      isect.b = a->b + t * (b->b - a->b);
      intersections.push_back(isect);
    }

    if (intersections.size() != 2) continue;  // Should have exactly 2 intersections

    // Sort intersections by x
    if (intersections[0].x > intersections[1].x) {
      std::swap(intersections[0], intersections[1]);
    }

    int x_start = std::max(static_cast<int>(std::floor(intersections[0].x)), 0);
    int x_end = std::min(static_cast<int>(std::ceil(intersections[1].x)), width - 1);

    if (x_start >= x_end) continue;

    // Interpolate color across scanline
    float dx = intersections[1].x - intersections[0].x;
    if (dx == 0) continue;

    uint32_t* row = bitmap + y * width;
    for (int x = x_start; x <= x_end; ++x) {
      float t = (x - intersections[0].x) / dx;
      float r = intersections[0].r + t * (intersections[1].r - intersections[0].r);
      float g = intersections[0].g + t * (intersections[1].g - intersections[0].g);
      float b = intersections[0].b + t * (intersections[1].b - intersections[0].b);

      uint8_t ri = static_cast<uint8_t>(clamp14(r * 255.0f, 0.0f, 255.0f));
      uint8_t gi = static_cast<uint8_t>(clamp14(g * 255.0f, 0.0f, 255.0f));
      uint8_t bi = static_cast<uint8_t>(clamp14(b * 255.0f, 0.0f, 255.0f));

      row[x] = 0xFF000000 | (bi << 16) | (gi << 8) | ri;  // BGRA format
    }
  }
}

// Cubic Bezier patch for Coons/Tensor-product patch meshes
struct BezierPatch {
  float points[4][4][2];  // 4x4 grid of control points (x, y)
  float colors[4][3];     // 4 corner colors (r, g, b)

  bool is_small() const {
    // Check if patch is smaller than 2x2 pixels
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

  // Subdivide patch vertically (split into top and bottom)
  void subdivide_vertical(BezierPatch& top, BezierPatch& bottom) const {
    for (int x = 0; x < 4; ++x) {
      // DeCasteljau subdivision for each row
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

    // Interpolate colors
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

  // Subdivide patch horizontally (split into left and right)
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

    // Interpolate colors
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

  // Draw patch as filled polygon (for small patches)
  void draw_to_bitmap(uint32_t* bitmap, int width, int height) const {
    // Get average color
    float r = (colors[0][0] + colors[1][0] + colors[2][0] + colors[3][0]) * 0.25f;
    float g = (colors[0][1] + colors[1][1] + colors[2][1] + colors[3][1]) * 0.25f;
    float b = (colors[0][2] + colors[1][2] + colors[2][2] + colors[3][2]) * 0.25f;

    // Create triangles from patch boundary (corners)
    MeshVertex v0 = {points[0][0][0], points[0][0][1], colors[0][0], colors[0][1], colors[0][2]};
    MeshVertex v1 = {points[3][0][0], points[3][0][1], colors[3][0], colors[3][1], colors[3][2]};
    MeshVertex v2 = {points[3][3][0], points[3][3][1], colors[2][0], colors[2][1], colors[2][2]};
    MeshVertex v3 = {points[0][3][0], points[0][3][1], colors[1][0], colors[1][1], colors[1][2]};

    // Draw as two triangles
    draw_gouraud_triangle(bitmap, width, height, v0, v1, v2);
    draw_gouraud_triangle(bitmap, width, height, v0, v2, v3);
  }
};

// Recursively subdivide and draw a Bezier patch
static void draw_patch_recursive(uint32_t* bitmap, int width, int height,
                                  const BezierPatch& patch, int depth = 0) {
  const int max_depth = 8;  // Maximum subdivision depth

  if (patch.is_small() || depth >= max_depth) {
    // Small enough - draw it
    patch.draw_to_bitmap(bitmap, width, height);
    return;
  }

  // Subdivide into 4 patches
  BezierPatch top, bottom;
  patch.subdivide_vertical(top, bottom);

  BezierPatch top_left, top_right;
  top.subdivide_horizontal(top_left, top_right);

  BezierPatch bottom_left, bottom_right;
  bottom.subdivide_horizontal(bottom_left, bottom_right);

  // Recursively draw sub-patches
  draw_patch_recursive(bitmap, width, height, top_left, depth + 1);
  draw_patch_recursive(bitmap, width, height, top_right, depth + 1);
  draw_patch_recursive(bitmap, width, height, bottom_left, depth + 1);
  draw_patch_recursive(bitmap, width, height, bottom_right, depth + 1);
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
      // Unknown RGB profile - try to use full ICC profile parsing
      color::IccProfileInfo full_profile;
      if (!color_space.icc_profile_data.empty()) {
        full_profile = color::parse_icc_profile(
            color_space.icc_profile_data.data(),
            color_space.icc_profile_data.size());
      }

      if (full_profile.valid && full_profile.is_matrix_profile) {
        // Use TRC curves and colorant matrix for accurate conversion
        for (int i = 0; i < width * height; i++) {
          int src_idx = i * 3;
          if (src_idx + 2 < static_cast<int>(src_data.size())) {
            float components[3] = {
                src_data[src_idx] / 255.0f,
                src_data[src_idx + 1] / 255.0f,
                src_data[src_idx + 2] / 255.0f
            };
            color::RGB rgb = color::iccbased_to_rgb(components, 3, full_profile);
            dst_data[i * 3] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, rgb.r)) * 255);
            dst_data[i * 3 + 1] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, rgb.g)) * 255);
            dst_data[i * 3 + 2] = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, rgb.b)) * 255);
          }
        }
      } else {
        // Fallback - direct copy assuming sRGB-like
        for (int i = 0; i < width * height; i++) {
          int src_idx = i * 3;
          if (src_idx + 2 < static_cast<int>(src_data.size())) {
            dst_data[i * 3] = src_data[src_idx];
            dst_data[i * 3 + 1] = src_data[src_idx + 1];
            dst_data[i * 3 + 2] = src_data[src_idx + 2];
          }
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

ThorVGBackend::ThorVGBackend() {
  // Initialize ThorVG engine (v1.0+ API - no canvas engine param)
  if (tvg::Initializer::init(0) != tvg::Result::Success) {
    NANOPDF_LOG_ERROR("ThorVG", "Failed to initialize ThorVG engine");
  }
}

ThorVGBackend::~ThorVGBackend() {
  // Clean up canvas (will delete scene if pushed)
  if (canvas_) {
    delete canvas_;
    canvas_ = nullptr;
  }
  // Terminate ThorVG engine (v1.0+ API - no canvas engine param)
  tvg::Initializer::term();
}

bool ThorVGBackend::initialize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;

  // Create buffer for rendering
  buffer_.resize(width * height);

  // Create software canvas (v1.0+ API returns raw pointer)
  canvas_ = tvg::SwCanvas::gen();
  if (!canvas_) {
    return false;
  }

  // Set target buffer (v1.0+ API uses tvg::ColorSpace enum)
  if (canvas_->target(reinterpret_cast<uint32_t*>(buffer_.data()),
                     width, width, height,
                     tvg::ColorSpace::ABGR8888) != tvg::Result::Success) {
    return false;
  }

  initialized_ = true;
  return true;
}

bool ThorVGBackend::begin_scene() {
  if (!initialized_) {
    return false;
  }

  // Create a new scene (v1.0+ API returns raw pointer)
  scene_ = tvg::Scene::gen();
  if (!scene_) {
    return false;
  }

  return true;
}

bool ThorVGBackend::end_scene() {
  if (!initialized_ || !scene_) {
    return false;
  }

  // Push scene to canvas (v1.0+ API takes raw Paint*, canvas owns it)
  if (canvas_->add(scene_) != tvg::Result::Success) {
    // Don't delete scene_ - ThorVG manages memory
    scene_ = nullptr;
    return false;
  }
  scene_ = nullptr;  // canvas owns it now

  // Draw the canvas (with clear=true to clear the buffer first)
  if (canvas_->draw(true) != tvg::Result::Success) {
    return false;
  }

  // Sync drawing
  if (canvas_->sync() != tvg::Result::Success) {
    return false;
  }

  return true;
}

bool ThorVGBackend::draw_rectangle(float x, float y, float width, float height,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Append rectangle
  shape->appendRect(x, y, width, height, 0, 0);

  // Set fill color
  shape->fill(r, g, b, a);

  // Add to scene (v1.0+ API takes raw Paint*, scene owns it after push)
  if (scene_->add(shape) != tvg::Result::Success) {
    // ThorVG manages memory - don't delete
    return false;
  }

  return true;
}

bool ThorVGBackend::draw_circle(float cx, float cy, float radius,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Append circle
  shape->appendCircle(cx, cy, radius, radius);

  // Set fill color
  shape->fill(r, g, b, a);

  // Add to scene (v1.0+ API takes raw Paint*, scene owns it after push)
  if (scene_->add(shape) != tvg::Result::Success) {
    return false;
  }

  return true;
}

bool ThorVGBackend::draw_path(const std::vector<tvg::PathCommand>& cmds,
                             const std::vector<tvg::Point>& pts,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Append path
  shape->appendPath(cmds.data(), cmds.size(), pts.data(), pts.size());

  // Set fill color
  shape->fill(r, g, b, a);

  // Add to scene with clipping if active
  return push_with_clip(shape);
}

// Render a Type 3 font glyph by executing its CharProc content stream
bool ThorVGBackend::render_type3_glyph(const Type3Font* type3_font, const std::string& glyph_name,
                                        float x, float y, float size,
                                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!type3_font || !current_pdf_ || glyph_name.empty()) {
    return false;
  }

  // Look up the glyph procedure in CharProcs
  auto proc_it = type3_font->char_procs.find(glyph_name);
  if (proc_it == type3_font->char_procs.end()) {
    // Glyph not found - draw placeholder rectangle
    auto shape = tvg::Shape::gen();
    if (shape) {
      float glyph_width = size * 0.5f;
      shape->appendRect(x, y - size, glyph_width, size, 0, 0);
      shape->fill(r, g, b, static_cast<uint8_t>(a * 0.3f));  // Semi-transparent
      push_with_clip(shape);
    }
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
  // Type 3 fonts use font_matrix to scale glyph space to text space
  // Default is [0.001, 0, 0, 0.001, 0, 0] which scales 1000 units to 1 point
  double fm_a = type3_font->font_matrix.size() >= 1 ? type3_font->font_matrix[0] : 0.001;
  double fm_b = type3_font->font_matrix.size() >= 2 ? type3_font->font_matrix[1] : 0;
  double fm_c = type3_font->font_matrix.size() >= 3 ? type3_font->font_matrix[2] : 0;
  double fm_d = type3_font->font_matrix.size() >= 4 ? type3_font->font_matrix[3] : 0.001;

  // Save current state
  GraphicsState saved_state = state_;

  // Set up graphics state for glyph rendering
  // Apply font matrix and size scaling
  float glyph_scale = size * static_cast<float>(fm_a) * state_.scale;

  // Offset to glyph position
  state_.transform.e = x;
  state_.transform.f = y;

  // Apply font matrix scaling
  state_.transform.a = glyph_scale;
  state_.transform.d = -glyph_scale;  // Flip Y for glyph space

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
  // Type 3 glyphs use d0/d1 operators for metrics, then standard path operators
  parse_pdf_content(decoded.data);

  // Pop resources
  if (!type3_font->resources.empty() && !form_resources_stack_.empty()) {
    form_resources_stack_.pop_back();
  }

  // Restore state
  state_ = saved_state;

  return true;
}

bool ThorVGBackend::draw_text(float x, float y, const std::string& text, float size,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  // Check for Type 3 font (user-defined glyphs)
  auto* type3_font = dynamic_cast<const Type3Font*>(current_font_);
  if (type3_font) {
    // Type 3 fonts: render each glyph using its CharProc content stream
    float cursor_x = x;

    for (size_t i = 0; i < text.length(); ++i) {
      uint8_t char_code = static_cast<uint8_t>(text[i]);

      // Get glyph name from encoding
      std::string glyph_name;
      if (char_code < type3_font->encoding.size()) {
        glyph_name = type3_font->encoding[char_code];
      }

      if (glyph_name.empty() || glyph_name == ".notdef") {
        // No glyph - draw placeholder and advance
        auto shape = tvg::Shape::gen();
        if (shape) {
          float glyph_width = size * 0.5f;
          shape->appendRect(cursor_x, y - size, glyph_width, size, 0, 0);
          shape->fill(r, g, b, static_cast<uint8_t>(a * 0.2f));
          push_with_clip(shape);
        }
        cursor_x += size * 0.6f;
        continue;
      }

      // Render the Type 3 glyph
      render_type3_glyph(type3_font, glyph_name, cursor_x, y, size, r, g, b, a);

      // Get advance width from Widths array if available
      float advance = size * 0.6f;  // Default advance
      if (char_code < type3_font->widths.size()) {
        // Type 3 widths are in glyph space, apply font matrix
        double fm_a = type3_font->font_matrix.size() >= 1 ? type3_font->font_matrix[0] : 0.001;
        advance = static_cast<float>(type3_font->widths[char_code] * fm_a * size);
      }

      cursor_x += advance;
    }
    return true;
  }

  // Check if we have a loaded font with glyph data
  FontCache* font = get_font(current_font_name_);

  if (font) {
    // Render each character using glyph outlines
    float cursor_x = x;
    float scale = stbtt_ScaleForPixelHeight(&font->font_info, size);

    // Check if this is a Type0/CID font that uses two-byte encoding
    auto* type0_font = dynamic_cast<const Type0Font*>(current_font_);
    bool is_two_byte_cid = false;
    if (type0_font) {
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
        is_two_byte_cid = true;
      }
    }

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
          draw_glyph_by_index(gid, cursor_x, y, size, r, g, b, a);

          // Get advance width for this glyph
          int advance_width, left_bearing;
          stbtt_GetGlyphHMetrics(&font->font_info, gid, &advance_width, &left_bearing);
          cursor_x += advance_width * scale;
          i += bytes_consumed;
          continue;
        }
      }

      // Try ToUnicode CMap for Type0 fonts
      if (type0_font && !type0_font->to_unicode_cmap.code_to_unicode.empty()) {
        uint32_t unicode = type0_font->to_unicode_cmap.map_code_to_unicode(char_code);
        if (unicode != char_code || type0_font->to_unicode_cmap.code_to_unicode.count(char_code)) {
          // Found a mapping, use it
          int advance_width, left_bearing;
          stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(unicode), &advance_width, &left_bearing);
          draw_glyph(static_cast<int>(unicode), cursor_x, y, size, r, g, b, a);
          cursor_x += advance_width * scale;
          i += bytes_consumed;
          continue;
        }
      }

      // Map character code to Unicode using encoding tables and glyph names
      uint32_t codepoint = map_char_to_unicode(char_code, current_font_);

      // Get glyph metrics for advance width
      int advance_width, left_bearing;
      stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(codepoint), &advance_width, &left_bearing);

      // Draw the glyph
      draw_glyph(static_cast<int>(codepoint), cursor_x, y, size, r, g, b, a);

      // Advance cursor
      cursor_x += advance_width * scale;

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
  } else {
    // Fallback: draw a placeholder rectangle for text
    float text_width = text.length() * size * 0.5f;
    float text_height = size;

    auto shape = tvg::Shape::gen();
    if (!shape) {
      return false;
    }

    // Draw filled rectangle as text placeholder
    shape->appendRect(x, y - text_height, text_width, text_height, 0, 0);
    shape->fill(r, g, b, a);

    if (scene_->add(shape) != tvg::Result::Success) {
      return false;
    }
  }

  return true;
}

bool ThorVGBackend::draw_line(float x1, float y1, float x2, float y2, float stroke_width,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Create line path
  shape->moveTo(x1, y1);
  shape->lineTo(x2, y2);

  // Set stroke (v1.0+ API uses separate methods)
  shape->strokeWidth(stroke_width);
  shape->strokeFill(r, g, b, a);
  shape->strokeCap(tvg::StrokeCap::Round);

  // Add to scene (v1.0+ API takes raw Paint*, scene owns it after push)
  if (scene_->add(shape) != tvg::Result::Success) {
    return false;
  }

  return true;
}

// Helper to load a TTF file from disk
static bool load_font_file(const std::string& path, std::vector<uint8_t>& data) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) return false;

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  data.resize(size);
  if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
    return false;
  }
  return true;
}

// Helper to determine font category from name
// Helper to determine font category from name
// Returns: 0=sans-serif, 1=monospace, 2=serif, 3=symbol, 4=CJK
static int get_thorvg_font_category(const std::string& font_name) {
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

bool ThorVGBackend::load_fallback_font(const std::string& font_name) {
  return load_fallback_font_with_hint(font_name, nullptr);
}

bool ThorVGBackend::load_fallback_font_with_hint(const std::string& font_name, const BaseFont* font) {
  // Determine font category for better substitution
  int category = get_thorvg_font_category(font_name);

  // Override category to CJK if font structure indicates CJK
  if (is_cjk_font(font)) {
    category = 4;  // CJK
  }

  // Font paths organized by category
  // Category 0: Sans-serif, 1: Monospace, 2: Serif, 3: Symbol, 4: CJK
  static const char* sans_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/Library/Fonts/Arial.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
    nullptr
  };

  static const char* mono_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
    "/System/Library/Fonts/Courier.ttc",
    "C:\\Windows\\Fonts\\cour.ttf",
    nullptr
  };

  static const char* serif_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
    "/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
    "/System/Library/Fonts/Times.ttc",
    "C:\\Windows\\Fonts\\times.ttf",
    nullptr
  };

  static const char* symbol_fonts[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/System/Library/Fonts/Symbol.ttf",
    nullptr
  };

  // CJK fonts (Japanese, Chinese, Korean)
  static const char* cjk_fonts[] = {
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
    "/Library/Fonts/Osaka.ttf",
    // Windows
    "C:\\Windows\\Fonts\\msgothic.ttc",
    "C:\\Windows\\Fonts\\meiryo.ttc",
    "C:\\Windows\\Fonts\\YuGothR.ttc",
    "C:\\Windows\\Fonts\\simsun.ttc",
    "C:\\Windows\\Fonts\\malgun.ttf",
    nullptr
  };

  static const char** font_lists[] = { sans_fonts, mono_fonts, serif_fonts, symbol_fonts, cjk_fonts };

  // Try fonts in the matching category first
  const char** font_list = font_lists[category];
  for (const char** path = font_list; *path; ++path) {
    FontCache cache;
    if (load_font_file(*path, cache.font_data)) {
      int font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
      if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
        cache.initialized = true;
        font_cache_[font_name] = std::move(cache);
        NANOPDF_LOG_DEBUG("ThorVG", "Using fallback font: %s for '%s' (cat=%d)", *path, font_name.c_str(), category);
        return true;
      }
    }
  }

  // If category-specific fonts not found, try sans-serif as ultimate fallback
  if (category != 0) {
    for (const char** path = sans_fonts; *path; ++path) {
      FontCache cache;
      if (load_font_file(*path, cache.font_data)) {
        int font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
        if (stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
          cache.initialized = true;
          font_cache_[font_name] = std::move(cache);
          NANOPDF_LOG_DEBUG("ThorVG", "Using fallback font: %s for '%s' (fallback)", *path, font_name.c_str());
          return true;
        }
      }
    }
  }

  NANOPDF_LOG_WARN("ThorVG", "Font '%s' - no fallback font found", font_name.c_str());
  return false;
}

bool ThorVGBackend::load_font(const Pdf& pdf, const std::string& font_name, const BaseFont* font) {
  if (font_cache_.count(font_name)) {
    return font_cache_[font_name].initialized;
  }

  // Check if font has embedded data
  if (!font || !font->descriptor) {
    // Try to load a fallback system font
    return load_fallback_font_with_hint(font_name, font);
  }
  if (font->descriptor->font_file.type == Value::UNDEFINED ||
      font->descriptor->font_file.type == Value::NULL_OBJ) {
    // Try fallback for fonts with descriptors but no embedded file
    return load_fallback_font_with_hint(font_name, font);
  }

  // Resolve and decode font file stream
  Value font_file_val = font->descriptor->font_file;
  if (font_file_val.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, font_file_val.ref_object_number,
                                      font_file_val.ref_generation_number);
    if (!resolved.success) {
      return load_fallback_font_with_hint(font_name, font);
    }
    font_file_val = resolved.value;
  }

  if (font_file_val.type != Value::STREAM) {
    return load_fallback_font_with_hint(font_name, font);
  }

  // Decode the font stream
  auto decoded = decode_stream(pdf, font_file_val);
  if (!decoded.success || decoded.data.empty()) {
    return load_fallback_font_with_hint(font_name, font);
  }

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

  // Initialize stb_truetype with the font data
  FontCache cache;
  cache.font_data = std::move(decoded.data);

  int font_offset = stbtt_GetFontOffsetForIndex(cache.font_data.data(), 0);
  if (!stbtt_InitFont(&cache.font_info, cache.font_data.data(), font_offset)) {
    return load_fallback_font_with_hint(font_name, font);
  }

  cache.initialized = true;
  font_cache_[font_name] = std::move(cache);
  NANOPDF_LOG_INFO("ThorVG", "Loaded embedded font '%s' (%zu bytes)", font_name.c_str(), cache.font_data.size());
  return true;
}

ThorVGBackend::FontCache* ThorVGBackend::get_font(const std::string& font_name) {
  auto it = font_cache_.find(font_name);
  if (it != font_cache_.end() && it->second.initialized) {
    return &it->second;
  }
  return nullptr;
}

float ThorVGBackend::calculate_text_width(const std::string& text, float font_size) {
  // First try to use PDF font width information if available
  auto* type0_font = dynamic_cast<const Type0Font*>(current_font_);
  if (type0_font) {
    // Determine if the CMap uses two-byte encoding
    bool is_two_byte_cid = false;
    const std::string& cmap_name = type0_font->encoding_cmap.name;
    if (cmap_name.find("Identity") != std::string::npos ||
        cmap_name.find("UTF16") != std::string::npos ||
        cmap_name.find("UCS2") != std::string::npos ||
        cmap_name.find("UniJIS") != std::string::npos ||
        cmap_name.find("UniGB") != std::string::npos ||
        cmap_name.find("UniKS") != std::string::npos ||
        cmap_name.find("UniCNS") != std::string::npos ||
        type0_font->ordering == "Japan1" ||
        type0_font->ordering == "GB1" ||
        type0_font->ordering == "CNS1" ||
        type0_font->ordering == "Korea1") {
      is_two_byte_cid = true;
    }

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
    // Last resort: approximate width
    return text.length() * font_size * 0.5f;
  }

  float scale = stbtt_ScaleForPixelHeight(&font->font_info, font_size);
  float width = 0.0f;

  for (size_t i = 0; i < text.length(); i++) {
    uint32_t char_code = static_cast<unsigned char>(text[i]);

    // For Type0/CID fonts with CIDToGIDMap, use glyph index
    if (type0_font && !type0_font->cid_to_gid_map.empty()) {
      if (char_code < type0_font->cid_to_gid_map.size()) {
        int gid = type0_font->cid_to_gid_map[char_code];
        int advance_width, left_bearing;
        stbtt_GetGlyphHMetrics(&font->font_info, gid, &advance_width, &left_bearing);
        width += advance_width * scale;
        continue;
      }
    }

    // Map to Unicode and get metrics
    uint32_t codepoint = map_char_to_unicode(char_code, current_font_);
    int advance_width, left_bearing;
    stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(codepoint), &advance_width, &left_bearing);
    width += advance_width * scale;

    // Add kerning
    if (i + 1 < text.length()) {
      uint32_t next_char_code = static_cast<unsigned char>(text[i + 1]);
      uint32_t next_codepoint = map_char_to_unicode(next_char_code, current_font_);
      int kern = stbtt_GetCodepointKernAdvance(&font->font_info,
                                                static_cast<int>(codepoint),
                                                static_cast<int>(next_codepoint));
      width += kern * scale;
    }
  }

  // Convert from pixel width back to text space units
  if (scale > 0) {
    return width / scale;
  }
  return width;
}

bool ThorVGBackend::push_with_clip(tvg::Shape* shape) {
  if (!scene_ || !shape) {
    return false;
  }

  // Apply blend mode
  if (state_.blend_mode != 0) {
    shape->blend(static_cast<tvg::BlendMethod>(state_.blend_mode));
  }

  // If there's a clipping path, apply it
  if (state_.has_clip && !state_.clip_commands.empty()) {
    // Create a clipper shape from the clipping path
    auto clipper = tvg::Shape::gen();

    // The clip_points are already in canvas coordinates (Y-flip and scale were
    // applied when the path was constructed in parse_pdf_content)
    clipper->appendPath(state_.clip_commands.data(), state_.clip_commands.size(),
                        state_.clip_points.data(), state_.clip_points.size());

    // Set fill rule for clipping
    if (state_.clip_even_odd) {
      clipper->fillRule(tvg::FillRule::EvenOdd);
    } else {
      clipper->fillRule(tvg::FillRule::NonZero);
    }

    // Apply clip to the shape (clip() takes ownership of clipper)
    if (shape->clip(clipper) != tvg::Result::Success) {
      // Clipping failed, release clipper and push shape without clip
      tvg::Paint::rel(clipper);
      scene_->add(shape);
      return true;
    }
  }

  scene_->add(shape);
  return true;
}

// Forward declaration for PDF function evaluator
static bool evaluate_pdf_function(const Pdf& pdf, const Value& function,
                                   const std::vector<double>& inputs,
                                   std::vector<double>& outputs);

bool ThorVGBackend::draw_image(const ImageXObject& image, float x, float y, float width, float height,
                               uint8_t fill_r, uint8_t fill_g, uint8_t fill_b) {
  NANOPDF_LOG_DEBUG("ThorVG", "draw_image: %dx%d at (%.1f,%.1f) size %.1fx%.1f, data=%zu bytes",
                    image.width, image.height, x, y, width, height, image.data.size());

  if (!scene_ || image.data.empty()) {
    NANOPDF_LOG_WARN("ThorVG", "draw_image: skipped (scene=%p, data.empty=%d)",
                     (void*)scene_, image.data.empty() ? 1 : 0);
    return false;
  }

  // Convert image data to ARGB8888 format for ThorVG
  std::vector<uint32_t> argb_data;
  int img_width = image.width;
  int img_height = image.height;

  if (img_width <= 0 || img_height <= 0) {
    return false;
  }

  argb_data.resize(img_width * img_height);

  // Determine number of components based on color space
  int num_components = 3;  // Default RGB
  ColorSpaceType cs_type = image.color_space.type;

  if (cs_type == ColorSpaceType::DeviceGray || cs_type == ColorSpaceType::CalGray) {
    num_components = 1;
  } else if (cs_type == ColorSpaceType::DeviceCMYK) {
    num_components = 4;
  } else if (cs_type == ColorSpaceType::Indexed) {
    num_components = 1;  // Index into lookup table
  } else if (cs_type == ColorSpaceType::ICCBased) {
    num_components = image.color_space.num_components;
    if (num_components == 0) num_components = 3;  // Default
  }

  // Handle image mask (1-bit data)
  if (image.image_mask) {
    int stride = (img_width + 7) / 8;  // Bytes per row

    // PDF spec: With default Decode [0 1], sample 0 = paint, sample 1 = masked.
    // With Decode [1 0], sample 1 = paint, sample 0 = masked.
    bool invert = false;
    if (image.decode.size() >= 2 && image.decode[0] > image.decode[1]) {
      invert = true;  // Decode [1 0]: bit=1 means paint
    }

    uint32_t paint_color =
        (0xFFu << 24) | (static_cast<uint32_t>(fill_r) << 16) |
        (static_cast<uint32_t>(fill_g) << 8) | static_cast<uint32_t>(fill_b);

    for (int row = 0; row < img_height; row++) {
      for (int col = 0; col < img_width; col++) {
        int byte_idx = row * stride + col / 8;
        int bit_idx = 7 - (col % 8);

        if (byte_idx < static_cast<int>(image.data.size())) {
          bool bit_set = (image.data[byte_idx] >> bit_idx) & 1;
          // Default Decode [0 1]: bit=0 → paint, bit=1 → transparent
          // Decode [1 0]: bit=1 → paint, bit=0 → transparent
          bool should_paint = invert ? bit_set : !bit_set;
          argb_data[row * img_width + col] = should_paint ? paint_color : 0x00000000;
        }
      }
    }
  }
  // Handle indexed color space
  else if (cs_type == ColorSpaceType::Indexed) {
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

    for (int i = 0; i < img_width * img_height && i < static_cast<int>(image.data.size()); i++) {
      uint8_t idx = image.data[i];
      int lookup_idx = idx * base_components;

      uint8_t r = 0, g = 0, b = 0;
      if (base_components == 1 && lookup_idx < static_cast<int>(lookup.size())) {
        r = g = b = lookup[lookup_idx];
      } else if (base_components >= 3 && lookup_idx + 2 < static_cast<int>(lookup.size())) {
        r = lookup[lookup_idx];
        g = lookup[lookup_idx + 1];
        b = lookup[lookup_idx + 2];
      }
      argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }
  }
  // Handle ICCBased color space with profile conversion
  else if (cs_type == ColorSpaceType::ICCBased) {
    std::vector<uint8_t> rgb_data;
    convert_icc_to_srgb(image.data, rgb_data, img_width, img_height, image.color_space);

    for (int i = 0; i < img_width * img_height; i++) {
      int src_idx = i * 3;
      if (src_idx + 2 < static_cast<int>(rgb_data.size())) {
        uint8_t r = rgb_data[src_idx];
        uint8_t g = rgb_data[src_idx + 1];
        uint8_t b = rgb_data[src_idx + 2];
        argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }
  // Handle Separation color space (single spot color)
  else if (cs_type == ColorSpaceType::Separation) {
    // Separation has a single tint value that maps to alternate color space
    const auto& colorant = image.color_space.colorant_names;
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

    for (int i = 0; i < img_width * img_height && i < static_cast<int>(image.data.size()); i++) {
      // Get tint value (0-1 range from 8-bit data)
      double tint = image.data[i] / 255.0;

      // Evaluate tint function if present
      std::vector<double> outputs;
      uint8_t r = 0, g = 0, b = 0;

      if (tint_func.type != Value::UNDEFINED && current_pdf_ &&
          evaluate_pdf_function(*current_pdf_, tint_func, {tint}, outputs) &&
          !outputs.empty()) {
        // Convert alternate color to RGB
        if (alt_components == 1) {
          // Gray
          uint8_t gray = static_cast<uint8_t>(outputs[0] * 255);
          r = g = b = gray;
        } else if (alt_components == 3 && outputs.size() >= 3) {
          // RGB
          r = static_cast<uint8_t>(outputs[0] * 255);
          g = static_cast<uint8_t>(outputs[1] * 255);
          b = static_cast<uint8_t>(outputs[2] * 255);
        } else if (alt_components == 4 && outputs.size() >= 4) {
          // CMYK to RGB
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

      argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }
  }
  // Handle DeviceN color space (multiple spot colors)
  else if (cs_type == ColorSpaceType::DeviceN) {
    int n_colorants = static_cast<int>(image.color_space.colorant_names.size());
    if (n_colorants == 0) n_colorants = num_components;
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

    for (int i = 0; i < img_width * img_height; i++) {
      int src_idx = i * n_colorants;

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

      argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }
  }
  // Handle grayscale
  else if (num_components == 1) {
    // Check if this is 1-bit per pixel (packed bits)
    if (image.bits_per_component == 1) {
      // Unpack 1-bit data: 8 pixels per byte
      int stride = (img_width + 7) / 8;  // Bytes per row
      for (int row = 0; row < img_height; row++) {
        for (int col = 0; col < img_width; col++) {
          int byte_idx = row * stride + col / 8;
          int bit_idx = 7 - (col % 8);  // MSB first

          if (byte_idx < static_cast<int>(image.data.size())) {
            bool bit_set = (image.data[byte_idx] >> bit_idx) & 1;
            // CCITT decoder outputs: 1=white, 0=black (when BlackIs1=false, which is typical)
            uint8_t gray = bit_set ? 0xFF : 0x00;
            argb_data[row * img_width + col] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
          }
        }
      }
    } else {
      // 8-bit grayscale
      for (int i = 0; i < img_width * img_height && i < static_cast<int>(image.data.size()); i++) {
        uint8_t gray = image.data[i];
        argb_data[i] = (0xFF << 24) | (gray << 16) | (gray << 8) | gray;
      }
    }
  }
  // Handle RGB
  else if (num_components == 3) {
    for (int i = 0; i < img_width * img_height; i++) {
      int src_idx = i * 3;
      if (src_idx + 2 < static_cast<int>(image.data.size())) {
        uint8_t r = image.data[src_idx];
        uint8_t g = image.data[src_idx + 1];
        uint8_t b = image.data[src_idx + 2];
        argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }
  // Handle CMYK (convert to RGB)
  else if (num_components == 4) {
    for (int i = 0; i < img_width * img_height; i++) {
      int src_idx = i * 4;
      if (src_idx + 3 < static_cast<int>(image.data.size())) {
        uint8_t c = image.data[src_idx];
        uint8_t m = image.data[src_idx + 1];
        uint8_t y = image.data[src_idx + 2];
        uint8_t k = image.data[src_idx + 3];
        // Simple CMYK to RGB conversion
        uint8_t r = static_cast<uint8_t>(255 * (1.0f - c / 255.0f) * (1.0f - k / 255.0f));
        uint8_t g = static_cast<uint8_t>(255 * (1.0f - m / 255.0f) * (1.0f - k / 255.0f));
        uint8_t b_val = static_cast<uint8_t>(255 * (1.0f - y / 255.0f) * (1.0f - k / 255.0f));
        argb_data[i] = (0xFF << 24) | (r << 16) | (g << 8) | b_val;
      }
    }
  }

  // Create ThorVG Picture from raw data
  auto picture = tvg::Picture::gen();
  if (!picture) {
    return false;
  }

  // Load raw pixel data
  // Note: ThorVG expects the data to persist, so we copy it
  auto* data_copy = new uint32_t[argb_data.size()];
  std::copy(argb_data.begin(), argb_data.end(), data_copy);

  auto result = picture->load(data_copy, img_width, img_height, tvg::ColorSpace::ARGB8888, true);
  if (result != tvg::Result::Success) {
    delete[] data_copy;
    return false;
  }

  // Apply transformation to position and scale the image
  // PDF coordinate: (x, y) is bottom-left, we need to flip Y for screen coordinates
  float scale_x = width / img_width;
  float scale_y = height / img_height;

  // Convert PDF coordinates to canvas coordinates
  // In PDF, y=0 is at bottom. In screen coords, y=0 is at top.
  // PDF (x, y) with height h maps to canvas (x*scale, (page_h - y - h)*scale)
  float canvas_x = x * state_.scale;
  float canvas_y = (state_.page_height - y - height) * state_.scale;

  NANOPDF_LOG_DEBUG("ThorVG", "draw_image transform: page_h=%.1f, scale=%.3f, canvas=(%.1f,%.1f)",
                    state_.page_height, state_.scale, canvas_x, canvas_y);

  // Apply transformation using matrix for non-uniform scaling
  tvg::Matrix m;
  m.e11 = scale_x * state_.scale;
  m.e12 = 0.0f;
  m.e13 = canvas_x;
  m.e21 = 0.0f;
  m.e22 = scale_y * state_.scale;
  m.e23 = canvas_y;
  m.e31 = 0.0f;
  m.e32 = 0.0f;
  m.e33 = 1.0f;
  picture->transform(m);

  // Push to scene
  auto push_result = scene_->add(std::move(picture));
  NANOPDF_LOG_DEBUG("ThorVG", "draw_image: pushed to scene, result=%d", static_cast<int>(push_result));

  return true;
}

// Helper function to extract color stops from a PDF function
// Returns color stops for gradient fills
static std::vector<tvg::Fill::ColorStop> extract_color_stops_from_function(
    const Pdf& pdf, const Value& function) {
  std::vector<tvg::Fill::ColorStop> stops;

  if (function.type != Value::DICTIONARY) {
    // Default black to white
    stops.push_back({0.0f, 0, 0, 0, 255});
    stops.push_back({1.0f, 255, 255, 255, 255});
    return stops;
  }

  auto func_type_it = function.dict.find("FunctionType");
  if (func_type_it == function.dict.end() || func_type_it->second.type != Value::NUMBER) {
    stops.push_back({0.0f, 0, 0, 0, 255});
    stops.push_back({1.0f, 255, 255, 255, 255});
    return stops;
  }

  int func_type = static_cast<int>(func_type_it->second.number);

  if (func_type == 2) {
    // Type 2: Exponential interpolation function
    // C0 = color at t=0, C1 = color at t=1
    auto c0_it = function.dict.find("C0");
    auto c1_it = function.dict.find("C1");

    tvg::Fill::ColorStop stop0 = {0.0f, 0, 0, 0, 255};
    tvg::Fill::ColorStop stop1 = {1.0f, 255, 255, 255, 255};

    if (c0_it != function.dict.end() && c0_it->second.type == Value::ARRAY &&
        c0_it->second.array.size() >= 3) {
      float r = c0_it->second.array[0].type == Value::NUMBER ?
                static_cast<float>(c0_it->second.array[0].number) : 0.0f;
      float g = c0_it->second.array[1].type == Value::NUMBER ?
                static_cast<float>(c0_it->second.array[1].number) : 0.0f;
      float b = c0_it->second.array[2].type == Value::NUMBER ?
                static_cast<float>(c0_it->second.array[2].number) : 0.0f;
      stop0 = {0.0f, static_cast<uint8_t>(r * 255),
               static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255};
    }

    if (c1_it != function.dict.end() && c1_it->second.type == Value::ARRAY &&
        c1_it->second.array.size() >= 3) {
      float r = c1_it->second.array[0].type == Value::NUMBER ?
                static_cast<float>(c1_it->second.array[0].number) : 1.0f;
      float g = c1_it->second.array[1].type == Value::NUMBER ?
                static_cast<float>(c1_it->second.array[1].number) : 1.0f;
      float b = c1_it->second.array[2].type == Value::NUMBER ?
                static_cast<float>(c1_it->second.array[2].number) : 1.0f;
      stop1 = {1.0f, static_cast<uint8_t>(r * 255),
               static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255};
    }

    stops.push_back(stop0);
    stops.push_back(stop1);
  }
  else if (func_type == 3) {
    // Type 3: Stitching function - combines multiple sub-functions
    // Functions = array of sub-functions
    // Bounds = array of boundary values between sub-functions
    // Encode = array mapping each subdomain to sub-function domain
    auto functions_it = function.dict.find("Functions");
    auto bounds_it = function.dict.find("Bounds");

    if (functions_it == function.dict.end() || functions_it->second.type != Value::ARRAY) {
      stops.push_back({0.0f, 0, 0, 0, 255});
      stops.push_back({1.0f, 255, 255, 255, 255});
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

    // For each sub-function, extract its start color
    for (size_t i = 0; i < functions.size() && i < boundaries.size(); i++) {
      Value sub_func = functions[i];

      // Resolve reference if needed
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
          float r = sub_c0_it->second.array[0].type == Value::NUMBER ?
                    static_cast<float>(sub_c0_it->second.array[0].number) : 0.0f;
          float g = sub_c0_it->second.array[1].type == Value::NUMBER ?
                    static_cast<float>(sub_c0_it->second.array[1].number) : 0.0f;
          float b = sub_c0_it->second.array[2].type == Value::NUMBER ?
                    static_cast<float>(sub_c0_it->second.array[2].number) : 0.0f;
          stops.push_back({boundaries[i], static_cast<uint8_t>(r * 255),
                           static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255});
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
          float r = c1_it->second.array[0].type == Value::NUMBER ?
                    static_cast<float>(c1_it->second.array[0].number) : 1.0f;
          float g = c1_it->second.array[1].type == Value::NUMBER ?
                    static_cast<float>(c1_it->second.array[1].number) : 1.0f;
          float b = c1_it->second.array[2].type == Value::NUMBER ?
                    static_cast<float>(c1_it->second.array[2].number) : 1.0f;
          stops.push_back({1.0f, static_cast<uint8_t>(r * 255),
                           static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255});
        }
      }
    }

    // Ensure we have at least 2 stops
    if (stops.size() < 2) {
      stops.clear();
      stops.push_back({0.0f, 0, 0, 0, 255});
      stops.push_back({1.0f, 255, 255, 255, 255});
    }
  }
  else {
    // Unsupported function type - default gradient
    stops.push_back({0.0f, 0, 0, 0, 255});
    stops.push_back({1.0f, 255, 255, 255, 255});
  }

  return stops;
}

// ============================================================
// PostScript Type 4 Calculator Function Evaluator
// ============================================================

// PostScript calculator stack
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
    j = ((j % n) + n) % n;  // Normalize j
    if (j == 0) return;

    std::vector<double> temp;
    for (int i = 0; i < n; ++i) {
      temp.push_back(pop());
    }
    // Rotate and push back
    for (int i = 0; i < n; ++i) {
      push(temp[(i + j) % n]);
    }
  }

  void index(int i) {
    if (i >= 0 && static_cast<size_t>(i) < stack_.size()) {
      push(stack_[stack_.size() - 1 - i]);
    }
  }

  void copy(int n) {
    if (n <= 0 || static_cast<size_t>(n) > stack_.size()) return;
    size_t start = stack_.size() - n;
    for (int i = 0; i < n; ++i) {
      push(stack_[start + i]);
    }
  }

  void clear() { stack_.clear(); }

  const std::vector<double>& data() const { return stack_; }

private:
  std::vector<double> stack_;
};

// Tokenize PostScript code
static std::vector<std::string> tokenize_postscript(const std::string& code) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < code.size()) {
    // Skip whitespace
    while (i < code.size() && (code[i] == ' ' || code[i] == '\t' ||
                                code[i] == '\n' || code[i] == '\r')) {
      ++i;
    }
    if (i >= code.size()) break;

    // Comments
    if (code[i] == '%') {
      while (i < code.size() && code[i] != '\n' && code[i] != '\r') ++i;
      continue;
    }

    // Braces are separate tokens
    if (code[i] == '{' || code[i] == '}') {
      tokens.push_back(std::string(1, code[i]));
      ++i;
      continue;
    }

    // Collect token
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

// Helper: collect tokens for a procedure body (between { and matching })
// pos should point to the token after the opening {.
static std::vector<std::string> collect_procedure(const std::vector<std::string>& tokens,
                                                   size_t& pos) {
  std::vector<std::string> proc;
  int brace_count = 1;
  while (pos < tokens.size() && brace_count > 0) {
    if (tokens[pos] == "{") ++brace_count;
    else if (tokens[pos] == "}") {
      --brace_count;
      if (brace_count == 0) { ++pos; break; }
    }
    proc.push_back(tokens[pos]);
    ++pos;
  }
  return proc;
}

// Forward declaration
static bool execute_procedure(const std::vector<std::string>& proc_tokens,
                               PSStack& stack, int depth);

// Execute PostScript tokens
static bool execute_postscript(const std::vector<std::string>& tokens,
                                size_t& pos, PSStack& stack,
                                int depth = 0) {
  const int MAX_DEPTH = 100;
  if (depth > MAX_DEPTH) return false;

  // Procedure stack for collecting { ... } bodies
  std::vector<std::vector<std::string>> proc_stack;

  while (pos < tokens.size()) {
    const std::string& tok = tokens[pos];
    ++pos;

    // End of procedure
    if (tok == "}") {
      return true;
    }

    // Start of procedure - collect body and push onto procedure stack
    if (tok == "{") {
      proc_stack.push_back(collect_procedure(tokens, pos));
      continue;
    }

    // Check if it's a number
    double num_val;
    if (is_ps_number(tok, num_val)) {
      stack.push(num_val);
      continue;
    }

    // Boolean literals
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
      stack.push(std::sin(stack.pop() * M_PI / 180.0));  // Degrees
    } else if (tok == "cos") {
      stack.push(std::cos(stack.pop() * M_PI / 180.0));  // Degrees
    } else if (tok == "atan") {
      double x = stack.pop(), y = stack.pop();
      stack.push(std::atan2(y, x) * 180.0 / M_PI);  // Degrees
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
    // Conditional operators
    else if (tok == "if") {
      // Format: { proc } bool if
      bool cond = stack.pop() != 0.0;
      if (!proc_stack.empty()) {
        auto proc = std::move(proc_stack.back());
        proc_stack.pop_back();
        if (cond) {
          execute_procedure(proc, stack, depth + 1);
        }
      }
    } else if (tok == "ifelse") {
      // Format: { true_proc } { false_proc } bool ifelse
      bool cond = stack.pop() != 0.0;
      std::vector<std::string> false_proc, true_proc;
      if (proc_stack.size() >= 2) {
        false_proc = std::move(proc_stack.back());
        proc_stack.pop_back();
        true_proc = std::move(proc_stack.back());
        proc_stack.pop_back();
      }
      if (cond) {
        execute_procedure(true_proc, stack, depth + 1);
      } else {
        execute_procedure(false_proc, stack, depth + 1);
      }
    }
    // Value clamping (common in PDF functions)
    else if (tok == "cvr") {
      // Convert to real - already real
    } else if (tok == "cvi") {
      stack.push(static_cast<double>(static_cast<int>(stack.pop())));
    }
    // Unknown operator - ignore
  }
  return true;
}

// Execute a collected procedure's tokens
static bool execute_procedure(const std::vector<std::string>& proc_tokens,
                               PSStack& stack, int depth) {
  size_t pos = 0;
  return execute_postscript(proc_tokens, pos, stack, depth);
}

// Evaluate a PostScript Type 4 function
static bool evaluate_postscript_function(const std::string& code,
                                          const std::vector<double>& inputs,
                                          const std::vector<double>& domain,
                                          const std::vector<double>& range,
                                          std::vector<double>& outputs) {
  // Tokenize the code (strip leading '{' and trailing '}')
  std::string stripped = code;
  size_t start = stripped.find('{');
  size_t end = stripped.rfind('}');
  if (start != std::string::npos && end != std::string::npos && end > start) {
    stripped = stripped.substr(start + 1, end - start - 1);
  }

  std::vector<std::string> tokens = tokenize_postscript(stripped);

  // Initialize stack with inputs (clamped to domain)
  PSStack stack;
  for (size_t i = 0; i < inputs.size(); ++i) {
    double v = inputs[i];
    if (domain.size() >= (i + 1) * 2) {
      v = clamp14(v, domain[i * 2], domain[i * 2 + 1]);
    }
    stack.push(v);
  }

  // Execute the PostScript code
  size_t pos = 0;
  if (!execute_postscript(tokens, pos, stack)) {
    return false;
  }

  // Get outputs from stack (in reverse order)
  int n_outputs = static_cast<int>(range.size() / 2);
  if (n_outputs <= 0) n_outputs = static_cast<int>(stack.size());

  outputs.clear();
  const auto& stack_data = stack.data();

  // Stack top is last output
  for (int i = n_outputs - 1; i >= 0 && !stack_data.empty(); --i) {
    size_t idx = stack_data.size() - 1 - i;
    if (idx < stack_data.size()) {
      double v = stack_data[idx];
      // Clamp to range
      if (range.size() >= static_cast<size_t>((n_outputs - 1 - i + 1) * 2)) {
        size_t ri = (n_outputs - 1 - i) * 2;
        v = clamp14(v, range[ri], range[ri + 1]);
      }
      outputs.push_back(v);
    }
  }

  // Reverse to get correct order
  std::reverse(outputs.begin(), outputs.end());

  return !outputs.empty();
}

// PDF function evaluator - evaluates a PDF function at given input values
// Returns output color values (typically RGB, each in range 0-1)
static bool evaluate_pdf_function(const Pdf& pdf, const Value& function,
                                   const std::vector<double>& inputs,
                                   std::vector<double>& outputs) {
  if (function.type != Value::DICTIONARY) {
    return false;
  }

  auto func_type_it = function.dict.find("FunctionType");
  if (func_type_it == function.dict.end() || func_type_it->second.type != Value::NUMBER) {
    return false;
  }

  int func_type = static_cast<int>(func_type_it->second.number);

  // Get function domain
  std::vector<double> domain;
  auto domain_it = function.dict.find("Domain");
  if (domain_it != function.dict.end() && domain_it->second.type == Value::ARRAY) {
    for (const auto& v : domain_it->second.array) {
      if (v.type == Value::NUMBER) {
        domain.push_back(v.number);
      }
    }
  }

  // Get function range (optional)
  std::vector<double> range;
  auto range_it = function.dict.find("Range");
  if (range_it != function.dict.end() && range_it->second.type == Value::ARRAY) {
    for (const auto& v : range_it->second.array) {
      if (v.type == Value::NUMBER) {
        range.push_back(v.number);
      }
    }
  }

  if (func_type == 0) {
    // Type 0: Sampled function
    // Lookup and interpolate from a sample table
    auto size_it = function.dict.find("Size");
    auto bps_it = function.dict.find("BitsPerSample");

    if (size_it == function.dict.end() || bps_it == function.dict.end()) {
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
    if (n_outputs <= 0) n_outputs = 3;  // Default to RGB

    // Get decode array
    std::vector<double> decode;
    auto decode_it = function.dict.find("Decode");
    if (decode_it != function.dict.end() && decode_it->second.type == Value::ARRAY) {
      for (const auto& v : decode_it->second.array) {
        if (v.type == Value::NUMBER) {
          decode.push_back(v.number);
        }
      }
    }

    // For sampled functions in a stream, decode the stream data
    if (function.type == Value::STREAM && !sizes.empty() && !inputs.empty()) {
      auto decoded = decode_stream(pdf, function);
      if (decoded.success && !decoded.data.empty()) {
        // Simple 1D or 2D interpolation
        int n_inputs = static_cast<int>(sizes.size());
        if (n_inputs == 1 && domain.size() >= 2) {
          // 1D sampled function
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
            // Apply decode mapping if present
            if (decode.size() >= static_cast<size_t>((i + 1) * 2)) {
              val = decode[i * 2] + val * (decode[i * 2 + 1] - decode[i * 2]);
            }
            outputs[i] = val;
          }
          return true;
        }
      }
    }

    // Fallback: return mid-gray
    outputs.resize(n_outputs, 0.5);
    return true;

  } else if (func_type == 2) {
    // Type 2: Exponential interpolation
    // y = C0 + x^N * (C1 - C0)
    auto c0_it = function.dict.find("C0");
    auto c1_it = function.dict.find("C1");
    auto n_it = function.dict.find("N");

    std::vector<double> c0, c1;
    double n_exp = 1.0;

    if (c0_it != function.dict.end() && c0_it->second.type == Value::ARRAY) {
      for (const auto& v : c0_it->second.array) {
        if (v.type == Value::NUMBER) {
          c0.push_back(v.number);
        }
      }
    }
    if (c1_it != function.dict.end() && c1_it->second.type == Value::ARRAY) {
      for (const auto& v : c1_it->second.array) {
        if (v.type == Value::NUMBER) {
          c1.push_back(v.number);
        }
      }
    }
    if (n_it != function.dict.end() && n_it->second.type == Value::NUMBER) {
      n_exp = n_it->second.number;
    }

    // Default C0 = [0] and C1 = [1] if not specified
    if (c0.empty()) c0 = {0.0};
    if (c1.empty()) c1 = {1.0};

    // Get input (clamp to domain)
    double x = inputs.empty() ? 0.0 : inputs[0];
    if (domain.size() >= 2) {
      x = clamp14(x, domain[0], domain[1]);
      x = (x - domain[0]) / (domain[1] - domain[0]);  // Normalize to 0-1
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
    auto functions_it = function.dict.find("Functions");
    auto bounds_it = function.dict.find("Bounds");
    auto encode_it = function.dict.find("Encode");

    if (functions_it == function.dict.end() || functions_it->second.type != Value::ARRAY) {
      return false;
    }

    const auto& functions = functions_it->second.array;

    std::vector<double> bounds;
    if (bounds_it != function.dict.end() && bounds_it->second.type == Value::ARRAY) {
      for (const auto& v : bounds_it->second.array) {
        if (v.type == Value::NUMBER) {
          bounds.push_back(v.number);
        }
      }
    }

    std::vector<double> encode;
    if (encode_it != function.dict.end() && encode_it->second.type == Value::ARRAY) {
      for (const auto& v : encode_it->second.array) {
        if (v.type == Value::NUMBER) {
          encode.push_back(v.number);
        }
      }
    }

    // Get input value
    double x = inputs.empty() ? 0.0 : inputs[0];
    if (domain.size() >= 2) {
      x = clamp14(x, domain[0], domain[1]);
    }

    // Find which sub-function to use
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

    // Map x to sub-function domain using encode array
    double encode_min = (func_idx * 2 < encode.size()) ? encode[func_idx * 2] : 0.0;
    double encode_max = (func_idx * 2 + 1 < encode.size()) ? encode[func_idx * 2 + 1] : 1.0;

    double sub_x = encode_min;
    if (subdomain_max != subdomain_min) {
      sub_x = encode_min + (x - subdomain_min) / (subdomain_max - subdomain_min) * (encode_max - encode_min);
    }

    // Resolve and evaluate sub-function
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
    // The function code is stored in the stream data
    if (function.type != Value::STREAM) {
      return false;
    }

    auto decoded = decode_stream(pdf, function);
    if (!decoded.success || decoded.data.empty()) {
      return false;
    }

    // Convert stream data to string
    std::string ps_code(decoded.data.begin(), decoded.data.end());

    // Evaluate the PostScript function
    return evaluate_postscript_function(ps_code, inputs, domain, range, outputs);
  }

  return false;
}

// Rasterize function-based shading to a bitmap
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
    // Degenerate matrix, fill with background or gray
    std::fill(pixels.begin(), pixels.end(), 0xFF808080);
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
      uint8_t r = 128, g = 128, b = 128, alpha = 255;

      if (dx >= x_min && dx <= x_max && dy >= y_min && dy <= y_max) {
        // Evaluate function at (dx, dy)
        std::vector<double> outputs;
        if (evaluate_pdf_function(pdf, shading.function, {dx, dy}, outputs)) {
          if (outputs.size() >= 3) {
            r = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
            g = static_cast<uint8_t>(clamp14(outputs[1], 0.0, 1.0) * 255);
            b = static_cast<uint8_t>(clamp14(outputs[2], 0.0, 1.0) * 255);
          } else if (outputs.size() == 1) {
            // Grayscale
            uint8_t gray = static_cast<uint8_t>(clamp14(outputs[0], 0.0, 1.0) * 255);
            r = g = b = gray;
          }
        }
      } else if (!shading.background.empty()) {
        // Use background color outside domain
        if (shading.background.size() >= 3) {
          r = static_cast<uint8_t>(clamp14(shading.background[0], 0.0, 1.0) * 255);
          g = static_cast<uint8_t>(clamp14(shading.background[1], 0.0, 1.0) * 255);
          b = static_cast<uint8_t>(clamp14(shading.background[2], 0.0, 1.0) * 255);
        }
      } else {
        // Transparent outside domain
        alpha = 0;
      }

      pixels[py * width + px] = (alpha << 24) | (r << 16) | (g << 8) | b;
    }
  }

  return true;
}

// Helper to lookup a resource, checking Form XObject resources first, then page resources
const Value* ThorVGBackend::lookup_resource(const std::string& resource_type, const std::string& name) const {
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

bool ThorVGBackend::draw_shading(const std::string& shading_name) {
  if (!scene_ || !current_pdf_ || !current_page_) {
    return false;
  }

  // Look up shading in page resources
  auto shading_dict_it = current_page_->resources.find("Shading");
  if (shading_dict_it == current_page_->resources.end()) {
    return false;
  }

  // Resolve shading dictionary if it's a reference
  Value shading_dict_value = shading_dict_it->second;
  if (shading_dict_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_, shading_dict_value.ref_object_number,
                                      shading_dict_value.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    shading_dict_value = resolved.value;
  }

  if (shading_dict_value.type != Value::DICTIONARY) {
    return false;
  }

  // Find the specific shading by name
  auto shading_it = shading_dict_value.dict.find(shading_name);
  if (shading_it == shading_dict_value.dict.end()) {
    return false;
  }

  const Value& shading_value = shading_it->second;

  // Resolve reference if needed
  Value resolved_shading = shading_value;
  if (shading_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_, shading_value.ref_object_number,
                                      shading_value.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return false;
    }
    resolved_shading = resolved.value;
  }

  if (resolved_shading.type != Value::DICTIONARY) {
    return false;
  }

  // Parse shading
  auto shading = parse_shading(*current_pdf_, resolved_shading.dict);
  if (!shading) {
    return false;
  }

  // Create shape that covers the current clipping area (or entire page)
  auto shape = tvg::Shape::gen();
  if (!shape) {
    return false;
  }

  // Default to full page bounds
  float x = 0;
  float y = 0;
  float w = state_.page_width * state_.scale;
  float h = state_.page_height * state_.scale;

  // Use shading BBox if available
  if (shading->bbox.size() >= 4) {
    x = static_cast<float>(shading->bbox[0]) * state_.scale;
    y = (state_.page_height - static_cast<float>(shading->bbox[3])) * state_.scale;
    w = (static_cast<float>(shading->bbox[2]) - static_cast<float>(shading->bbox[0])) * state_.scale;
    h = (static_cast<float>(shading->bbox[3]) - static_cast<float>(shading->bbox[1])) * state_.scale;
  }

  shape->appendRect(x, y, w, h, 0, 0);

  // Create gradient based on shading type
  if (shading->type == ShadingType::Axial && shading->coords.size() >= 4) {
    // Linear gradient
    auto gradient = tvg::LinearGradient::gen();
    if (!gradient) {
      tvg::Paint::rel(shape);
      return false;
    }

    // Coords are [x0, y0, x1, y1]
    float x0 = static_cast<float>(shading->coords[0]) * state_.scale;
    float y0 = (state_.page_height - static_cast<float>(shading->coords[1])) * state_.scale;
    float x1 = static_cast<float>(shading->coords[2]) * state_.scale;
    float y1 = (state_.page_height - static_cast<float>(shading->coords[3])) * state_.scale;

    gradient->linear(x0, y0, x1, y1);

    // Extract color stops from function (supports Type 2 and Type 3 functions)
    auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function);
    gradient->colorStops(colorStops.data(), colorStops.size());

    // Set spread mode based on extend flags
    if (shading->extend.size() >= 2 && (shading->extend[0] || shading->extend[1])) {
      gradient->spread(tvg::FillSpread::Pad);
    }

    shape->fill(gradient);
  }
  else if (shading->type == ShadingType::Radial && shading->coords.size() >= 6) {
    // Radial gradient
    auto gradient = tvg::RadialGradient::gen();
    if (!gradient) {
      tvg::Paint::rel(shape);
      return false;
    }

    // Coords are [x0, y0, r0, x1, y1, r1]
    float x0 = static_cast<float>(shading->coords[0]) * state_.scale;
    float y0 = (state_.page_height - static_cast<float>(shading->coords[1])) * state_.scale;
    float r0 = static_cast<float>(shading->coords[2]) * state_.scale;
    float x1 = static_cast<float>(shading->coords[3]) * state_.scale;
    float y1 = (state_.page_height - static_cast<float>(shading->coords[4])) * state_.scale;
    float r1 = static_cast<float>(shading->coords[5]) * state_.scale;

    // ThorVG radial uses (cx, cy, r, fx, fy, fr) where:
    // - (cx, cy, r) is the outer circle
    // - (fx, fy, fr) is the focal/inner circle
    gradient->radial(x1, y1, r1, x0, y0, r0);

    // Extract color stops from function (supports Type 2 and Type 3 functions)
    auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function);
    gradient->colorStops(colorStops.data(), colorStops.size());

    if (shading->extend.size() >= 2 && (shading->extend[0] || shading->extend[1])) {
      gradient->spread(tvg::FillSpread::Pad);
    }

    shape->fill(gradient);
  }
  else if (shading->type == ShadingType::FunctionBased) {
    // Type 1: Function-based shading
    // Evaluate function at each point in domain and rasterize to bitmap

#if NANOPDF_DEBUG_PRINT
    printf("DEBUG: Type 1 (Function-based) shading requested\n");
    printf("DEBUG: Domain=[%g,%g,%g,%g], Matrix=[%g,%g,%g,%g,%g,%g]\n",
           shading->domain.size() >= 4 ? shading->domain[0] : 0,
           shading->domain.size() >= 4 ? shading->domain[1] : 1,
           shading->domain.size() >= 4 ? shading->domain[2] : 0,
           shading->domain.size() >= 4 ? shading->domain[3] : 1,
           shading->matrix.size() >= 6 ? shading->matrix[0] : 1,
           shading->matrix.size() >= 6 ? shading->matrix[1] : 0,
           shading->matrix.size() >= 6 ? shading->matrix[2] : 0,
           shading->matrix.size() >= 6 ? shading->matrix[3] : 1,
           shading->matrix.size() >= 6 ? shading->matrix[4] : 0,
           shading->matrix.size() >= 6 ? shading->matrix[5] : 0);
#endif

    // Rasterize the function-based shading to a bitmap
    int raster_w = static_cast<int>(w);
    int raster_h = static_cast<int>(h);
    if (raster_w <= 0) raster_w = 256;
    if (raster_h <= 0) raster_h = 256;

    std::vector<uint32_t> pixels;
    if (rasterize_function_shading(*current_pdf_, *shading, raster_w, raster_h,
                                    state_.scale, state_.page_height, pixels)) {
      // Create ThorVG Picture from rasterized bitmap
      auto picture = tvg::Picture::gen();
      if (picture) {
        auto* data_copy = new uint32_t[pixels.size()];
        std::copy(pixels.begin(), pixels.end(), data_copy);

        auto result = picture->load(data_copy, raster_w, raster_h,
                                     tvg::ColorSpace::ARGB8888, true);
        if (result == tvg::Result::Success) {
          // Position the picture at the shape location
          tvg::Matrix m;
          m.e11 = 1.0f; m.e12 = 0.0f; m.e13 = x;
          m.e21 = 0.0f; m.e22 = 1.0f; m.e23 = y;
          m.e31 = 0.0f; m.e32 = 0.0f; m.e33 = 1.0f;
          picture->transform(m);

          scene_->add(picture);
          tvg::Paint::rel(shape);  // Don't need the shape anymore
          return true;
        } else {
          delete[] data_copy;
        }
      }
    }

    // Fallback: use gradient approximation
#if NANOPDF_DEBUG_PRINT
    printf("DEBUG: Function rasterization failed, using gradient fallback\n");
#endif
    auto gradient = tvg::LinearGradient::gen();
    if (!gradient) {
      tvg::Paint::rel(shape);
      return false;
    }

    float x0 = 0, y0 = 0, x1 = w, y1 = 0;
    if (shading->domain.size() >= 4 && shading->matrix.size() >= 6) {
      float xmin = static_cast<float>(shading->domain[0]);
      float xmax = static_cast<float>(shading->domain[1]);
      float a = static_cast<float>(shading->matrix[0]);
      float e = static_cast<float>(shading->matrix[4]);
      x0 = (a * xmin + e) * state_.scale;
      x1 = (a * xmax + e) * state_.scale;
    }

    gradient->linear(x0, y0, x1, y1);
    auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function);
    gradient->colorStops(colorStops.data(), colorStops.size());
    shape->fill(gradient);
  }
  else if (shading->type == ShadingType::FreeFormTriangleMesh ||
           shading->type == ShadingType::LatticeFormTriangleMesh) {
    // Type 4: Free-form triangle mesh shading
    // Type 5: Lattice-form triangle mesh shading

#if NANOPDF_DEBUG_PRINT
    const char* type_name = (shading->type == ShadingType::FreeFormTriangleMesh) ?
                            "Type 4 (Free-form Triangle Mesh)" :
                            "Type 5 (Lattice-form Triangle Mesh)";
    printf("DEBUG: %s shading requested\n", type_name);
    printf("DEBUG: BitsPerCoordinate=%d, BitsPerComponent=%d, BitsPerFlag=%d\n",
           shading->bits_per_coordinate, shading->bits_per_component, shading->bits_per_flag);
    printf("DEBUG: Data stream size=%zu bytes\n", shading->data_stream.size());
#endif

    // Validate parameters
    if (shading->data_stream.empty() || shading->decode.size() < 4) {
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Invalid mesh data - skipping\n");
#endif
      tvg::Paint::rel(shape);
      return false;
    }

    // Create a bitmap to rasterize the mesh
    int bmp_width = static_cast<int>(w);
    int bmp_height = static_cast<int>(h);
    if (bmp_width <= 0 || bmp_height <= 0 || bmp_width > 4096 || bmp_height > 4096) {
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Invalid bitmap size - skipping\n");
#endif
      tvg::Paint::rel(shape);
      return false;
    }

    std::vector<uint32_t> bitmap(bmp_width * bmp_height, 0x00000000);  // Transparent

    // Parse decode array
    float x_min = static_cast<float>(shading->decode[0]);
    float x_max = static_cast<float>(shading->decode[1]);
    float y_min = static_cast<float>(shading->decode[2]);
    float y_max = static_cast<float>(shading->decode[3]);

    // Color components (RGB for most color spaces)
    int num_components = 3;  // Assume RGB
    std::vector<float> color_min, color_max;
    for (size_t i = 4; i + 1 < shading->decode.size() && i < 4 + num_components * 2; i += 2) {
      color_min.push_back(static_cast<float>(shading->decode[i]));
      color_max.push_back(static_cast<float>(shading->decode[i + 1]));
    }

    // Setup bit stream
    BitStream bit_stream(shading->data_stream.data(), shading->data_stream.size());

    if (shading->type == ShadingType::FreeFormTriangleMesh) {
      // Type 4: Free-form triangle mesh with flags
      MeshVertex triangle[3];
      int vertex_count = 0;

      while (!bit_stream.is_eof()) {
        // Read flag
        uint32_t flag = 0;
        if (shading->bits_per_flag > 0 && bit_stream.bits_remaining() >= shading->bits_per_flag) {
          flag = bit_stream.get_bits(shading->bits_per_flag) & 0x03;
        } else {
          break;
        }

        // Read vertex
        if (bit_stream.bits_remaining() < shading->bits_per_coordinate * 2) break;
        uint32_t x_enc = bit_stream.get_bits(shading->bits_per_coordinate);
        uint32_t y_enc = bit_stream.get_bits(shading->bits_per_coordinate);

        MeshVertex vert;
        vert.x = x + decode_coord(x_enc, shading->bits_per_coordinate, x_min, x_max) * state_.scale;
        vert.y = y + (h - decode_coord(y_enc, shading->bits_per_coordinate, y_min, y_max)) * state_.scale;

        // Read color
        if (bit_stream.bits_remaining() < shading->bits_per_component * num_components) break;
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

        // Build triangles based on flag
        if (flag == 0) {
          // Start new triangle
          triangle[0] = vert;
          vertex_count = 1;
        } else {
          // Continue triangle strip
          if (vertex_count < 3) {
            triangle[vertex_count++] = vert;
          }

          if (vertex_count == 3) {
            // Draw triangle
            draw_gouraud_triangle(bitmap.data(), bmp_width, bmp_height,
                                  triangle[0], triangle[1], triangle[2]);

            // Shift vertices for triangle strip
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
    } else if (shading->type == ShadingType::LatticeFormTriangleMesh) {
      // Type 5: Lattice triangle mesh - requires VerticesPerRow
      // For now, skip as we'd need to parse the dictionary for this parameter
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Lattice mesh - simplified rendering\n");
#endif
    }

    // Convert bitmap to ThorVG Picture
    auto picture = tvg::Picture::gen();
    if (!picture) {
      tvg::Paint::rel(shape);
      return false;
    }

    // Convert BGRA to RGBA for ThorVG
    std::vector<uint32_t> rgba_data(bitmap.size());
    for (size_t i = 0; i < bitmap.size(); ++i) {
      uint32_t bgra = bitmap[i];
      uint32_t a = (bgra >> 24) & 0xFF;
      uint32_t r = bgra & 0xFF;
      uint32_t g = (bgra >> 8) & 0xFF;
      uint32_t b = (bgra >> 16) & 0xFF;
      rgba_data[i] = (a << 24) | (b << 16) | (g << 8) | r;
    }

    // Load pixel data (ARGB8888S = un-premultiplied ARGB)
    if (picture->load(reinterpret_cast<uint32_t*>(rgba_data.data()),
                      bmp_width, bmp_height, tvg::ColorSpace::ARGB8888S, true) != tvg::Result::Success) {
      tvg::Paint::rel(shape);
      return false;
    }

    picture->translate(x, y);
    scene_->add(picture);
    tvg::Paint::rel(shape);  // Don't need shape wrapper
    return true;
  }
  else if (shading->type == ShadingType::CoonsPatchMesh ||
           shading->type == ShadingType::TensorProductPatchMesh) {
    // Type 6: Coons patch mesh shading (12 control points per patch)
    // Type 7: Tensor-product patch mesh shading (16 control points per patch)

    bool is_tensor = (shading->type == ShadingType::TensorProductPatchMesh);

#if NANOPDF_DEBUG_PRINT
    const char* type_name = is_tensor ? "Type 7 (Tensor-product Patch Mesh)" :
                                       "Type 6 (Coons Patch Mesh)";
    printf("DEBUG: %s shading requested\n", type_name);
    printf("DEBUG: BitsPerCoordinate=%d, BitsPerComponent=%d, BitsPerFlag=%d\n",
           shading->bits_per_coordinate, shading->bits_per_component, shading->bits_per_flag);
    printf("DEBUG: Data stream size=%zu bytes\n", shading->data_stream.size());
#endif

    // Validate parameters
    if (shading->data_stream.empty() || shading->decode.size() < 4) {
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Invalid patch data - skipping\n");
#endif
      tvg::Paint::rel(shape);
      return false;
    }

    // Create a bitmap to rasterize patches
    int bmp_width = static_cast<int>(w);
    int bmp_height = static_cast<int>(h);
    if (bmp_width <= 0 || bmp_height <= 0 || bmp_width > 4096 || bmp_height > 4096) {
#if NANOPDF_DEBUG_PRINT
      printf("DEBUG: Invalid bitmap size - skipping\n");
#endif
      tvg::Paint::rel(shape);
      return false;
    }

    std::vector<uint32_t> bitmap(bmp_width * bmp_height, 0x00000000);  // Transparent

    // Parse decode array
    float x_min = static_cast<float>(shading->decode[0]);
    float x_max = static_cast<float>(shading->decode[1]);
    float y_min = static_cast<float>(shading->decode[2]);
    float y_max = static_cast<float>(shading->decode[3]);

    // Color components
    int num_components = 3;  // RGB
    std::vector<float> color_min, color_max;
    for (size_t i = 4; i + 1 < shading->decode.size() && i < 4 + num_components * 2; i += 2) {
      color_min.push_back(static_cast<float>(shading->decode[i]));
      color_max.push_back(static_cast<float>(shading->decode[i + 1]));
    }

    // Setup bit stream
    BitStream bit_stream(shading->data_stream.data(), shading->data_stream.size());

    // Track previous patch for edge sharing
    BezierPatch prev_patch;
    bool have_prev = false;

    while (!bit_stream.is_eof()) {
      // Read flag
      uint32_t flag = 0;
      if (shading->bits_per_flag > 0 && bit_stream.bits_remaining() >= shading->bits_per_flag) {
        flag = bit_stream.get_bits(shading->bits_per_flag) & 0x03;
      } else {
        break;
      }

      BezierPatch patch;

      // Number of control points to read depends on flag and type
      // flag=0: new patch, read all points
      // flag=1,2,3: shared edge, read fewer points
      int num_points_to_read = is_tensor ? 16 : 12;
      if (flag > 0 && have_prev) {
        // Shared edge - fewer points to read (simplified, full implementation complex)
        num_points_to_read = is_tensor ? 12 : 8;
      }

      // Read control points (simplified - Type 6/7 have complex point orderings)
      // For a proper implementation, need to handle Coons vs Tensor point layouts
      for (int i = 0; i < 4 && i < num_points_to_read / 4; ++i) {
        for (int j = 0; j < 4; ++j) {
          if (bit_stream.bits_remaining() < shading->bits_per_coordinate * 2) break;

          uint32_t x_enc = bit_stream.get_bits(shading->bits_per_coordinate);
          uint32_t y_enc = bit_stream.get_bits(shading->bits_per_coordinate);

          patch.points[i][j][0] = x + decode_coord(x_enc, shading->bits_per_coordinate,
                                                    x_min, x_max) * state_.scale;
          patch.points[i][j][1] = y + (h - decode_coord(y_enc, shading->bits_per_coordinate,
                                                         y_min, y_max)) * state_.scale;
        }
      }

      // Read 4 corner colors
      for (int corner = 0; corner < 4; ++corner) {
        if (bit_stream.bits_remaining() < shading->bits_per_component * num_components) break;

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

      // Draw patch using recursive subdivision
      draw_patch_recursive(bitmap.data(), bmp_width, bmp_height, patch);

      prev_patch = patch;
      have_prev = true;
    }

    // Convert bitmap to ThorVG Picture
    auto picture = tvg::Picture::gen();
    if (!picture) {
      tvg::Paint::rel(shape);
      return false;
    }

    // Convert BGRA to RGBA
    std::vector<uint32_t> rgba_data(bitmap.size());
    for (size_t i = 0; i < bitmap.size(); ++i) {
      uint32_t bgra = bitmap[i];
      uint32_t a = (bgra >> 24) & 0xFF;
      uint32_t r = bgra & 0xFF;
      uint32_t g = (bgra >> 8) & 0xFF;
      uint32_t b = (bgra >> 16) & 0xFF;
      rgba_data[i] = (a << 24) | (b << 16) | (g << 8) | r;
    }

    // Load pixel data (ARGB8888S = un-premultiplied ARGB)
    if (picture->load(reinterpret_cast<uint32_t*>(rgba_data.data()),
                      bmp_width, bmp_height, tvg::ColorSpace::ARGB8888S, true) != tvg::Result::Success) {
      tvg::Paint::rel(shape);
      return false;
    }

    picture->translate(x, y);
    scene_->add(picture);
    tvg::Paint::rel(shape);
    return true;
  }
  else {
    // Unsupported shading type - fill with gray
    shape->fill(128, 128, 128, 255);
  }

  return push_with_clip(shape);
}

bool ThorVGBackend::parse_inline_image(const std::string& content, size_t& pos) {
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
             content[pos] != '/') {
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
               content[pos] != '/') {
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
               content[pos] != '/') {
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

bool ThorVGBackend::apply_pattern_fill(tvg::Shape* shape, const std::string& pattern_name, bool is_stroke) {
  if (!shape || !current_pdf_ || !current_page_) {
    return false;
  }

  // Look up pattern from page resources
  auto pattern_dict_it = current_page_->resources.find("Pattern");
  if (pattern_dict_it == current_page_->resources.end()) {
    NANOPDF_LOG_TRACE("ThorVG", "No Pattern resources in page");
    return false;
  }

  // Get the pattern dictionary
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
    NANOPDF_LOG_DEBUG("ThorVG", "Pattern '%s' not found", pattern_name.c_str());
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
    NANOPDF_LOG_DEBUG("ThorVG", "Failed to parse pattern '%s'", pattern_name.c_str());
    return false;
  }

  NANOPDF_LOG_TRACE("ThorVG", "Applying pattern '%s', type=%d", pattern_name.c_str(),
                    static_cast<int>(pattern->type));

  if (pattern->type == PatternType::Shading && pattern->shading) {
    // Shading pattern - apply as gradient fill
    auto shading = pattern->shading.get();

    if (shading->type == ShadingType::Axial && shading->coords.size() >= 4) {
      // Linear gradient
      auto gradient = tvg::LinearGradient::gen();
      if (!gradient) return false;

      // Apply pattern matrix if present
      float x0 = static_cast<float>(shading->coords[0]);
      float y0 = static_cast<float>(shading->coords[1]);
      float x1 = static_cast<float>(shading->coords[2]);
      float y1 = static_cast<float>(shading->coords[3]);

      // Transform coords through pattern matrix (if any)
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

      gradient->linear(x0, y0, x1, y1);

      // Extract color stops
      auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function);
      gradient->colorStops(colorStops.data(), colorStops.size());

      if (shading->extend.size() >= 2 && (shading->extend[0] || shading->extend[1])) {
        gradient->spread(tvg::FillSpread::Pad);
      }

      if (is_stroke) {
        shape->strokeFill(gradient);
      } else {
        shape->fill(gradient);
      }
      return true;
    }
    else if (shading->type == ShadingType::Radial && shading->coords.size() >= 6) {
      // Radial gradient
      auto gradient = tvg::RadialGradient::gen();
      if (!gradient) return false;

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

        // Scale radius by average scale factor
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

      gradient->radial(x1, y1, r1, x0, y0, r0);

      auto colorStops = extract_color_stops_from_function(*current_pdf_, shading->function);
      gradient->colorStops(colorStops.data(), colorStops.size());

      if (shading->extend.size() >= 2 && (shading->extend[0] || shading->extend[1])) {
        gradient->spread(tvg::FillSpread::Pad);
      }

      if (is_stroke) {
        shape->strokeFill(gradient);
      } else {
        shape->fill(gradient);
      }
      return true;
    }
  }
  else if (pattern->type == PatternType::Tiling && pattern->tiling) {
    // Tiling pattern - render tile and repeat
    return apply_tiling_pattern(shape, pattern->tiling.get(), pattern->matrix, is_stroke);
  }

  return false;
}

bool ThorVGBackend::apply_tiling_pattern(tvg::Shape* shape, const TilingPattern* tiling,
                                          const std::vector<double>& matrix, bool is_stroke) {
  if (!shape || !tiling || !current_pdf_) {
    return false;
  }

#if NANOPDF_DEBUG_PRINT
  printf("DEBUG: Tiling pattern requested: paint_type=%d, bbox=[%g,%g,%g,%g], x_step=%g, y_step=%g\n",
         static_cast<int>(tiling->paint_type),
         tiling->bbox.size() >= 4 ? tiling->bbox[0] : 0,
         tiling->bbox.size() >= 4 ? tiling->bbox[1] : 0,
         tiling->bbox.size() >= 4 ? tiling->bbox[2] : 0,
         tiling->bbox.size() >= 4 ? tiling->bbox[3] : 0,
         tiling->x_step, tiling->y_step);
#endif

  // Get tile dimensions from BBox
  if (tiling->bbox.size() < 4 || tiling->content_stream.empty()) {
    // No content to render - use fallback color
    goto fallback;
  }

  {
    float tile_width = static_cast<float>(tiling->bbox[2] - tiling->bbox[0]);
    float tile_height = static_cast<float>(tiling->bbox[3] - tiling->bbox[1]);

    if (tile_width <= 0 || tile_height <= 0) {
      goto fallback;
    }

    // Calculate tile size in pixels (with some reasonable scaling)
    float pattern_scale = state_.scale;
    int tile_px_w = static_cast<int>(std::ceil(tile_width * pattern_scale));
    int tile_px_h = static_cast<int>(std::ceil(tile_height * pattern_scale));

    // Limit tile size to avoid excessive memory usage
    tile_px_w = clamp14(tile_px_w, 1, 512);
    tile_px_h = clamp14(tile_px_h, 1, 512);

    // Create temporary canvas for rendering the tile
    std::vector<uint32_t> tile_buffer(tile_px_w * tile_px_h, 0);

    // Initialize with transparent (for colored) or white (for uncolored)
    if (tiling->paint_type == TilingPaintType::ColoredTiles) {
      // Transparent background for colored patterns (ARGB = 0x00000000)
      std::fill(tile_buffer.begin(), tile_buffer.end(), 0);
    } else {
      // For uncolored, we'll apply color after rendering (ARGB = 0xFFFFFFFF = white)
      std::fill(tile_buffer.begin(), tile_buffer.end(), 0xFFFFFFFF);
    }

    tvg::SwCanvas* tile_canvas = tvg::SwCanvas::gen();
    if (!tile_canvas) {
      goto fallback;
    }

    if (tile_canvas->target(tile_buffer.data(),
                            tile_px_w, tile_px_w, tile_px_h,
                            tvg::ColorSpace::ABGR8888) != tvg::Result::Success) {
      delete tile_canvas;
      goto fallback;
    }

    tvg::Scene* tile_scene = tvg::Scene::gen();
    if (!tile_scene) {
      delete tile_canvas;
      goto fallback;
    }

    // Save current state
    tvg::SwCanvas* saved_canvas = canvas_;
    tvg::Scene* saved_scene = scene_;
    std::vector<uint32_t> saved_buffer = std::move(buffer_);
    uint32_t saved_width = width_;
    uint32_t saved_height = height_;
    GraphicsState saved_state = state_;

    // Switch to tile canvas
    canvas_ = tile_canvas;
    scene_ = tile_scene;
    buffer_ = std::move(tile_buffer);
    width_ = tile_px_w;
    height_ = tile_px_h;

    // Reset graphics state for tile rendering
    state_ = GraphicsState();
    state_.page_width = tile_width;
    state_.page_height = tile_height;
    state_.scale = pattern_scale;

    // Push pattern resources if available
    if (!tiling->resources.empty()) {
      form_resources_stack_.push_back(tiling->resources);
    }

    // Parse and render the tile content
    parse_pdf_content(tiling->content_stream);

    // Pop pattern resources
    if (!tiling->resources.empty() && !form_resources_stack_.empty()) {
      form_resources_stack_.pop_back();
    }

    // Finalize tile rendering
    if (tile_canvas->add(tile_scene) == tvg::Result::Success) {
      tile_canvas->draw(true);
      tile_canvas->sync();
    }

    // Get the rendered tile pixels
    std::vector<uint32_t> rendered_tile = std::move(buffer_);

    // Clean up tile canvas
    delete tile_canvas;

    // Restore original state
    canvas_ = saved_canvas;
    scene_ = saved_scene;
    buffer_ = std::move(saved_buffer);
    width_ = saved_width;
    height_ = saved_height;
    state_ = saved_state;

    // Now create a tiled bitmap covering the target area
    // Get shape bounds
    float shape_x, shape_y, shape_w, shape_h;
    shape->bounds(&shape_x, &shape_y, &shape_w, &shape_h);

    // Calculate how many tiles we need
    float step_x = static_cast<float>(tiling->x_step) * state_.scale;
    float step_y = static_cast<float>(tiling->y_step) * state_.scale;
    if (step_x <= 0) step_x = tile_px_w;
    if (step_y <= 0) step_y = tile_px_h;

    int tiles_x = static_cast<int>(std::ceil(shape_w / step_x)) + 2;
    int tiles_y = static_cast<int>(std::ceil(shape_h / step_y)) + 2;

    // Limit total tiles
    tiles_x = clamp14(tiles_x, 1, 50);
    tiles_y = clamp14(tiles_y, 1, 50);

    int tiled_w = static_cast<int>(tiles_x * step_x);
    int tiled_h = static_cast<int>(tiles_y * step_y);

    // Create the tiled bitmap
    std::vector<uint32_t> tiled_pixels(tiled_w * tiled_h);

    // Get color for uncolored patterns
    uint8_t pattern_r = state_.fill_r;
    uint8_t pattern_g = state_.fill_g;
    uint8_t pattern_b = state_.fill_b;
    uint8_t pattern_a = static_cast<uint8_t>(state_.fill_opacity * 255);
    if (is_stroke) {
      pattern_r = state_.stroke_r;
      pattern_g = state_.stroke_g;
      pattern_b = state_.stroke_b;
      pattern_a = static_cast<uint8_t>(state_.stroke_opacity * 255);
    }

    // Tile the pattern
    for (int ty = 0; ty < tiles_y; ++ty) {
      for (int tx = 0; tx < tiles_x; ++tx) {
        int offset_x = static_cast<int>(tx * step_x);
        int offset_y = static_cast<int>(ty * step_y);

        for (int py = 0; py < tile_px_h && offset_y + py < tiled_h; ++py) {
          for (int px = 0; px < tile_px_w && offset_x + px < tiled_w; ++px) {
            size_t src_idx = py * tile_px_w + px;
            size_t dst_idx = (offset_y + py) * tiled_w + (offset_x + px);

            // Extract BGRA components from ABGR8888 format pixel
            uint32_t pixel = rendered_tile[src_idx];
            uint8_t b_val = static_cast<uint8_t>(pixel & 0xFF);
            uint8_t g_val = static_cast<uint8_t>((pixel >> 8) & 0xFF);
            uint8_t r_val = static_cast<uint8_t>((pixel >> 16) & 0xFF);
            uint8_t a_val = static_cast<uint8_t>((pixel >> 24) & 0xFF);

            if (tiling->paint_type == TilingPaintType::UncoloredTiles) {
              // For uncolored patterns, use the alpha as a mask and apply current color
              // Invert: white background with black pattern content
              uint8_t intensity = static_cast<uint8_t>((255 - ((r_val + g_val + b_val) / 3)));
              a_val = static_cast<uint8_t>((intensity * pattern_a) / 255);
              r_val = pattern_r;
              g_val = pattern_g;
              b_val = pattern_b;
            }

            tiled_pixels[dst_idx] = (a_val << 24) | (r_val << 16) | (g_val << 8) | b_val;
          }
        }
      }
    }

    // Create ThorVG Picture from tiled bitmap
    auto picture = tvg::Picture::gen();
    if (picture) {
      auto* data_copy = new uint32_t[tiled_pixels.size()];
      std::copy(tiled_pixels.begin(), tiled_pixels.end(), data_copy);

      auto result = picture->load(data_copy, tiled_w, tiled_h,
                                   tvg::ColorSpace::ARGB8888, true);
      if (result == tvg::Result::Success) {
        // Position at shape origin
        tvg::Matrix m;
        m.e11 = 1.0f; m.e12 = 0.0f; m.e13 = shape_x;
        m.e21 = 0.0f; m.e22 = 1.0f; m.e23 = shape_y;
        m.e31 = 0.0f; m.e32 = 0.0f; m.e33 = 1.0f;
        picture->transform(m);

        // Clip to shape using the clip() method
        // Note: In newer ThorVG, clip() takes ownership of the shape
        tvg::Shape* clip_shape = tvg::Shape::gen();
        if (clip_shape) {
          // Copy path from original shape
          float sx, sy, sw, sh;
          shape->bounds(&sx, &sy, &sw, &sh);
          clip_shape->appendRect(sx, sy, sw, sh, 0, 0);
          picture->clip(clip_shape);
        }

        scene_->add(picture);
        return true;
      } else {
        delete[] data_copy;
      }
    }
  }

fallback:
  // Fallback: use solid color based on paint_type
  uint8_t r, g, b, a;

  if (tiling->paint_type == TilingPaintType::UncoloredTiles) {
    if (is_stroke) {
      r = state_.stroke_r;
      g = state_.stroke_g;
      b = state_.stroke_b;
      a = static_cast<uint8_t>(state_.stroke_opacity * 255);
    } else {
      r = state_.fill_r;
      g = state_.fill_g;
      b = state_.fill_b;
      a = static_cast<uint8_t>(state_.fill_opacity * 255);
    }
  } else {
    // Colored pattern placeholder
    r = 200;
    g = 150;
    b = 220;
    a = 255;
  }

  if (is_stroke) {
    shape->strokeFill(r, g, b, a);
    shape->strokeWidth(state_.stroke_width * state_.scale);
  } else {
    shape->fill(r, g, b, a);
  }

  return true;
}

bool ThorVGBackend::draw_glyph(int codepoint, float x, float y, float size,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  // Get current font
  FontCache* font = get_font(current_font_name_);
  if (!font) {
    // Fallback to placeholder rectangle
    auto shape = tvg::Shape::gen();
    if (!shape) return false;
    float glyph_width = size * 0.5f;
    shape->appendRect(x, y - size, glyph_width, size, 0, 0);
    shape->fill(r, g, b, a);
    push_with_clip(shape);
    return true;
  }

  // Get glyph outline from stb_truetype
  stbtt_vertex* vertices = nullptr;
  int num_verts = stbtt_GetCodepointShape(&font->font_info, codepoint, &vertices);

  if (num_verts == 0 || !vertices) {
    // Glyph not found, draw placeholder
    auto shape = tvg::Shape::gen();
    if (!shape) return false;
    float glyph_width = size * 0.5f;
    shape->appendRect(x, y - size, glyph_width, size, 0, 0);
    shape->fill(r, g, b, a);
    push_with_clip(shape);
    return true;
  }

  // Calculate scale factor
  float scale = stbtt_ScaleForPixelHeight(&font->font_info, size);

  // Build ThorVG path from stb_truetype vertices
  auto shape = tvg::Shape::gen();
  if (!shape) {
    stbtt_FreeShape(&font->font_info, vertices);
    return false;
  }

  // Track current position for quadratic-to-cubic conversion
  float curr_x = x, curr_y = y;

  for (int i = 0; i < num_verts; i++) {
    stbtt_vertex* v = &vertices[i];
    float vx = x + v->x * scale;
    float vy = y - v->y * scale;  // Flip Y for screen coordinates

    switch (v->type) {
      case STBTT_vmove:
        shape->moveTo(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vline:
        shape->lineTo(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vcurve: {
        // Quadratic bezier - convert to cubic for ThorVG
        // Control point (P1)
        float cx = x + v->cx * scale;
        float cy = y - v->cy * scale;
        // End point (P2) is (vx, vy)
        // Start point (P0) is (curr_x, curr_y)

        // Convert quadratic to cubic bezier:
        // CP1 = P0 + 2/3 * (P1 - P0)
        // CP2 = P2 + 2/3 * (P1 - P2)
        float cp1x = curr_x + (2.0f / 3.0f) * (cx - curr_x);
        float cp1y = curr_y + (2.0f / 3.0f) * (cy - curr_y);
        float cp2x = vx + (2.0f / 3.0f) * (cx - vx);
        float cp2y = vy + (2.0f / 3.0f) * (cy - vy);

        shape->cubicTo(cp1x, cp1y, cp2x, cp2y, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
      case STBTT_vcubic: {
        float cx1 = x + v->cx * scale;
        float cy1 = y - v->cy * scale;
        float cx2 = x + v->cx1 * scale;
        float cy2 = y - v->cy1 * scale;
        shape->cubicTo(cx1, cy1, cx2, cy2, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
    }
  }

  shape->close();

  // Apply rendering based on text rendering mode
  // 0 = Fill, 1 = Stroke, 2 = Fill+Stroke, 3 = Invisible
  // 4 = Fill+Clip, 5 = Stroke+Clip, 6 = Fill+Stroke+Clip, 7 = Clip only
  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 || render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 || render_mode == 5 || render_mode == 6);
  bool invisible = (render_mode == 3);  // Only mode 3 is truly invisible
  bool add_to_clip = (render_mode >= 4 && render_mode <= 7);

  // If adding to clip, accumulate glyph path in text_clip arrays
  if (add_to_clip) {
    state_.text_clip_active = true;
    // Re-trace the glyph path to accumulate clip commands
    float curr_x_clip = x, curr_y_clip = y;
    for (int i = 0; i < num_verts; i++) {
      stbtt_vertex* v = &vertices[i];
      float vx = x + v->x * scale;
      float vy = y - v->y * scale;

      switch (v->type) {
        case STBTT_vmove:
          state_.text_clip_commands.push_back(tvg::PathCommand::MoveTo);
          state_.text_clip_points.push_back({vx, vy});
          curr_x_clip = vx;
          curr_y_clip = vy;
          break;
        case STBTT_vline:
          state_.text_clip_commands.push_back(tvg::PathCommand::LineTo);
          state_.text_clip_points.push_back({vx, vy});
          curr_x_clip = vx;
          curr_y_clip = vy;
          break;
        case STBTT_vcurve: {
          float cx = x + v->cx * scale;
          float cy = y - v->cy * scale;
          float cp1x = curr_x_clip + (2.0f / 3.0f) * (cx - curr_x_clip);
          float cp1y = curr_y_clip + (2.0f / 3.0f) * (cy - curr_y_clip);
          float cp2x = vx + (2.0f / 3.0f) * (cx - vx);
          float cp2y = vy + (2.0f / 3.0f) * (cy - vy);
          state_.text_clip_commands.push_back(tvg::PathCommand::CubicTo);
          state_.text_clip_points.push_back({cp1x, cp1y});
          state_.text_clip_points.push_back({cp2x, cp2y});
          state_.text_clip_points.push_back({vx, vy});
          curr_x_clip = vx;
          curr_y_clip = vy;
          break;
        }
        case STBTT_vcubic: {
          float cx1 = x + v->cx * scale;
          float cy1 = y - v->cy * scale;
          float cx2 = x + v->cx1 * scale;
          float cy2 = y - v->cy1 * scale;
          state_.text_clip_commands.push_back(tvg::PathCommand::CubicTo);
          state_.text_clip_points.push_back({cx1, cy1});
          state_.text_clip_points.push_back({cx2, cy2});
          state_.text_clip_points.push_back({vx, vy});
          curr_x_clip = vx;
          curr_y_clip = vy;
          break;
        }
      }
    }
    state_.text_clip_commands.push_back(tvg::PathCommand::Close);
  }

  // Mode 7: clip only - don't render visible shape
  if (render_mode == 7 || invisible) {
    stbtt_FreeShape(&font->font_info, vertices);
    return true;
  }

  if (do_fill) {
    shape->fill(r, g, b, a);
  }

  if (do_stroke) {
    // Apply stroke with current stroke style
    float stroke_width = state_.stroke_width * state_.scale;
    if (stroke_width < 0.5f) stroke_width = 0.5f;  // Minimum stroke width for visibility
    shape->strokeWidth(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);

    // Set line cap and join
    tvg::StrokeCap cap = tvg::StrokeCap::Butt;
    if (state_.line_cap == 1) cap = tvg::StrokeCap::Round;
    else if (state_.line_cap == 2) cap = tvg::StrokeCap::Square;
    shape->strokeCap(cap);

    tvg::StrokeJoin join = tvg::StrokeJoin::Miter;
    if (state_.line_join == 1) join = tvg::StrokeJoin::Round;
    else if (state_.line_join == 2) join = tvg::StrokeJoin::Bevel;
    shape->strokeJoin(join);
  }

  stbtt_FreeShape(&font->font_info, vertices);

  return push_with_clip(shape);
}

bool ThorVGBackend::draw_glyph_by_index(int glyph_index, float x, float y, float size,
                                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!scene_) {
    return false;
  }

  // Get current font
  FontCache* font = get_font(current_font_name_);
  if (!font) {
    // Fallback to placeholder rectangle
    auto shape = tvg::Shape::gen();
    if (!shape) return false;
    float glyph_width = size * 0.5f;
    shape->appendRect(x, y - size, glyph_width, size, 0, 0);
    shape->fill(r, g, b, a);
    push_with_clip(shape);
    return true;
  }

  // Get glyph outline from stb_truetype using glyph index directly
  stbtt_vertex* vertices = nullptr;
  int num_verts = stbtt_GetGlyphShape(&font->font_info, glyph_index, &vertices);

  if (num_verts == 0 || !vertices) {
    // Glyph not found, draw placeholder
    auto shape = tvg::Shape::gen();
    if (!shape) return false;
    float glyph_width = size * 0.5f;
    shape->appendRect(x, y - size, glyph_width, size, 0, 0);
    shape->fill(r, g, b, a);
    push_with_clip(shape);
    return true;
  }

  // Calculate scale factor
  float scale = stbtt_ScaleForPixelHeight(&font->font_info, size);

  // Build ThorVG path from stb_truetype vertices
  auto shape = tvg::Shape::gen();
  if (!shape) {
    stbtt_FreeShape(&font->font_info, vertices);
    return false;
  }

  // Track current position for quadratic-to-cubic conversion
  float curr_x = x, curr_y = y;

  for (int i = 0; i < num_verts; i++) {
    stbtt_vertex* v = &vertices[i];
    float vx = x + v->x * scale;
    float vy = y - v->y * scale;  // Flip Y for screen coordinates

    switch (v->type) {
      case STBTT_vmove:
        shape->moveTo(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vline:
        shape->lineTo(vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      case STBTT_vcurve: {
        // Quadratic bezier - convert to cubic for ThorVG
        // Control point (P1)
        float cx = x + v->cx * scale;
        float cy = y - v->cy * scale;
        // End point (P2) is (vx, vy)
        // Start point (P0) is (curr_x, curr_y)

        // Convert quadratic to cubic bezier:
        // CP1 = P0 + 2/3 * (P1 - P0)
        // CP2 = P2 + 2/3 * (P1 - P2)
        float cp1x = curr_x + (2.0f / 3.0f) * (cx - curr_x);
        float cp1y = curr_y + (2.0f / 3.0f) * (cy - curr_y);
        float cp2x = vx + (2.0f / 3.0f) * (cx - vx);
        float cp2y = vy + (2.0f / 3.0f) * (cy - vy);

        shape->cubicTo(cp1x, cp1y, cp2x, cp2y, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
      case STBTT_vcubic: {
        float cx1 = x + v->cx * scale;
        float cy1 = y - v->cy * scale;
        float cx2 = x + v->cx1 * scale;
        float cy2 = y - v->cy1 * scale;
        shape->cubicTo(cx1, cy1, cx2, cy2, vx, vy);
        curr_x = vx;
        curr_y = vy;
        break;
      }
    }
  }

  shape->close();

  // Apply rendering based on text rendering mode
  // 0 = Fill, 1 = Stroke, 2 = Fill+Stroke, 3 = Invisible
  // 4 = Fill+Clip, 5 = Stroke+Clip, 6 = Fill+Stroke+Clip, 7 = Clip only
  int render_mode = state_.text_render_mode;
  bool do_fill = (render_mode == 0 || render_mode == 2 || render_mode == 4 || render_mode == 6);
  bool do_stroke = (render_mode == 1 || render_mode == 2 || render_mode == 5 || render_mode == 6);
  bool invisible = (render_mode == 3);  // Only mode 3 is truly invisible
  bool add_to_clip = (render_mode >= 4 && render_mode <= 7);

  // If adding to clip, accumulate glyph path in text_clip arrays
  if (add_to_clip) {
    state_.text_clip_active = true;
    // Re-trace the glyph path to accumulate clip commands
    float curr_x_clip = x, curr_y_clip = y;
    for (int i = 0; i < num_verts; i++) {
      stbtt_vertex* v = &vertices[i];
      float vx_c = x + v->x * scale;
      float vy_c = y - v->y * scale;

      switch (v->type) {
        case STBTT_vmove:
          state_.text_clip_commands.push_back(tvg::PathCommand::MoveTo);
          state_.text_clip_points.push_back({vx_c, vy_c});
          curr_x_clip = vx_c;
          curr_y_clip = vy_c;
          break;
        case STBTT_vline:
          state_.text_clip_commands.push_back(tvg::PathCommand::LineTo);
          state_.text_clip_points.push_back({vx_c, vy_c});
          curr_x_clip = vx_c;
          curr_y_clip = vy_c;
          break;
        case STBTT_vcurve: {
          float cx_c = x + v->cx * scale;
          float cy_c = y - v->cy * scale;
          float cp1x = curr_x_clip + (2.0f / 3.0f) * (cx_c - curr_x_clip);
          float cp1y = curr_y_clip + (2.0f / 3.0f) * (cy_c - curr_y_clip);
          float cp2x = vx_c + (2.0f / 3.0f) * (cx_c - vx_c);
          float cp2y = vy_c + (2.0f / 3.0f) * (cy_c - vy_c);
          state_.text_clip_commands.push_back(tvg::PathCommand::CubicTo);
          state_.text_clip_points.push_back({cp1x, cp1y});
          state_.text_clip_points.push_back({cp2x, cp2y});
          state_.text_clip_points.push_back({vx_c, vy_c});
          curr_x_clip = vx_c;
          curr_y_clip = vy_c;
          break;
        }
        case STBTT_vcubic: {
          float cx1 = x + v->cx * scale;
          float cy1 = y - v->cy * scale;
          float cx2 = x + v->cx1 * scale;
          float cy2 = y - v->cy1 * scale;
          state_.text_clip_commands.push_back(tvg::PathCommand::CubicTo);
          state_.text_clip_points.push_back({cx1, cy1});
          state_.text_clip_points.push_back({cx2, cy2});
          state_.text_clip_points.push_back({vx_c, vy_c});
          curr_x_clip = vx_c;
          curr_y_clip = vy_c;
          break;
        }
      }
    }
    state_.text_clip_commands.push_back(tvg::PathCommand::Close);
  }

  // Mode 7: clip only - don't render visible shape
  if (render_mode == 7 || invisible) {
    stbtt_FreeShape(&font->font_info, vertices);
    return true;
  }

  if (do_fill) {
    shape->fill(r, g, b, a);
  }

  if (do_stroke) {
    // Apply stroke with current stroke style
    float stroke_width = state_.stroke_width * state_.scale;
    if (stroke_width < 0.5f) stroke_width = 0.5f;  // Minimum stroke width for visibility
    shape->strokeWidth(stroke_width);
    uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
    shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);

    // Set line cap and join
    tvg::StrokeCap cap = tvg::StrokeCap::Butt;
    if (state_.line_cap == 1) cap = tvg::StrokeCap::Round;
    else if (state_.line_cap == 2) cap = tvg::StrokeCap::Square;
    shape->strokeCap(cap);

    tvg::StrokeJoin join = tvg::StrokeJoin::Miter;
    if (state_.line_join == 1) join = tvg::StrokeJoin::Round;
    else if (state_.line_join == 2) join = tvg::StrokeJoin::Bevel;
    shape->strokeJoin(join);
  }

  stbtt_FreeShape(&font->font_info, vertices);

  return push_with_clip(shape);
}

ThorVGRenderResult ThorVGBackend::get_buffer() {
  ThorVGRenderResult result;

  if (!initialized_) {
    result.error = "Backend not initialized";
    return result;
  }

  result.width = width_;
  result.height = height_;

  // Convert from ABGR8888 to RGBA8888
  result.pixels.resize(width_ * height_ * 4);
  for (size_t i = 0; i < buffer_.size(); ++i) {
    uint32_t pixel = buffer_[i];
    uint8_t a = (pixel >> 24) & 0xFF;
    uint8_t b = (pixel >> 16) & 0xFF;
    uint8_t g = (pixel >> 8) & 0xFF;
    uint8_t r = pixel & 0xFF;

    result.pixels[i * 4 + 0] = r;
    result.pixels[i * 4 + 1] = g;
    result.pixels[i * 4 + 2] = b;
    result.pixels[i * 4 + 3] = a;
  }

  result.success = true;
  return result;
}

bool ThorVGBackend::save_to_png(const std::string& filename) {
  ThorVGRenderOptions options;
  options.format = ThorVGRenderOptions::Format::PNG;
  return save_to_file(filename, options);
}

bool ThorVGBackend::save_to_file(const std::string& filename, const ThorVGRenderOptions& options) {
  auto result = get_buffer();
  if (!result.success) {
    return false;
  }

  int ret = 0;
  int width = static_cast<int>(result.width);
  int height = static_cast<int>(result.height);
  int stride = width * 4;

  switch (options.format) {
    case ThorVGRenderOptions::Format::PNG: {
      stbi_write_png_compression_level = options.png_compression;
      ret = stbi_write_png(filename.c_str(), width, height, 4,
                           result.pixels.data(), stride);
      break;
    }
    case ThorVGRenderOptions::Format::JPEG: {
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
    case ThorVGRenderOptions::Format::BMP:
      ret = stbi_write_bmp(filename.c_str(), width, height, 4,
                           result.pixels.data());
      break;
    case ThorVGRenderOptions::Format::TGA:
      ret = stbi_write_tga(filename.c_str(), width, height, 4,
                           result.pixels.data());
      break;
  }

  return ret != 0;
}

ThorVGRenderResult ThorVGBackend::render_page(const Pdf& pdf, const Page& page,
                                               const ThorVGRenderOptions& options) {
  ThorVGRenderResult result;

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

  if (!begin_scene()) {
    result.error = "Failed to begin scene";
    return result;
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

  // Draw background
  draw_rectangle(0, 0, width_, height_, options.bg_r, options.bg_g, options.bg_b, options.bg_a);

  // Process content streams
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

  if (!end_scene()) {
    result.error = "Failed to end scene";
    return result;
  }

  current_pdf_ = nullptr;
  current_page_ = nullptr;

  return get_buffer();
}

ThorVGRenderResult ThorVGBackend::render_page(const Pdf& pdf, const Page& page) {
  ThorVGRenderResult result;

  if (!initialized_) {
    result.error = "Backend not initialized";
    return result;
  }

  if (!begin_scene()) {
    result.error = "Failed to begin scene";
    return result;
  }

  // Get page dimensions from media_box [left, bottom, right, top]
  double page_width = 612.0;   // Default US Letter
  double page_height = 792.0;
  if (page.media_box.size() >= 4) {
    page_width = page.media_box[2] - page.media_box[0];
    page_height = page.media_box[3] - page.media_box[1];
  }

  // Calculate scale to fit the page into our canvas
  float scale_x = static_cast<float>(width_) / page_width;
  float scale_y = static_cast<float>(height_) / page_height;
  float scale = std::min(scale_x, scale_y);

  // Store PDF and Page references for font access
  current_pdf_ = &pdf;
  current_page_ = &page;

  // Ensure page fonts are loaded
  page.ensure_fonts_loaded(pdf);

  // Draw white background
  draw_rectangle(0, 0, width_, height_, 255, 255, 255, 255);

  // Parse and render page content
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
      // Pass object numbers for proper decryption
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

  // Render page annotations (including form widgets)
  for (const auto& annot : page.annotations) {
    if (!annot) continue;

    // Get annotation rect
    if (annot->rect.size() < 4) continue;
    float ax1 = static_cast<float>(annot->rect[0]) * scale;
    float ay1 = (page_height - static_cast<float>(annot->rect[3])) * scale;
    float ax2 = static_cast<float>(annot->rect[2]) * scale;
    float ay2 = (page_height - static_cast<float>(annot->rect[1])) * scale;
    float aw = ax2 - ax1;
    float ah = ay2 - ay1;

    if (aw <= 0 || ah <= 0) continue;

    // Check for appearance stream
    auto n_it = annot->appearance_streams.find("N");
    if (n_it != annot->appearance_streams.end()) {
      // Has normal appearance - render it as a Form XObject
      Value ap_stream = n_it->second;
      if (ap_stream.type == Value::REFERENCE) {
        auto resolved = resolve_reference(pdf, ap_stream.ref_object_number,
                                           ap_stream.ref_generation_number);
        if (resolved.success) {
          ap_stream = resolved.value;
        }
      }

      if (ap_stream.type == Value::STREAM) {
        auto decoded = decode_stream(pdf, ap_stream);
        if (decoded.success) {
          // Save state and set up for appearance stream
          GraphicsState saved_state = state_;
          state_ = GraphicsState();
          state_.page_width = page_width;
          state_.page_height = page_height;
          state_.scale = scale;

          // Set up transformation for annotation rect
          state_.transform.a = scale;
          state_.transform.d = scale;
          state_.transform.e = ax1;
          state_.transform.f = ay1;

          parse_pdf_content(decoded.data);

          state_ = saved_state;
        }
      }
      continue;
    }

    // No appearance stream - render default appearance based on type
    if (annot->type == AnnotationType::Widget) {
      auto* widget = static_cast<WidgetAnnotation*>(annot.get());

      // Draw widget border
      auto shape = tvg::Shape::gen();
      if (shape) {
        shape->appendRect(ax1, ay1, aw, ah, 0, 0);

        // Light gray background for form fields
        shape->fill(245, 245, 245, 255);
        shape->strokeWidth(1.0f);
        shape->strokeFill(128, 128, 128, 255);

        scene_->add(std::move(shape));
      }

      // Render field value for text fields
      if (widget->field_type == FieldType::Text && !widget->field_value.empty()) {
        // Draw text value (simple implementation)
        float text_size = std::min(ah * 0.7f, 12.0f * scale);
        float text_x = ax1 + 2 * scale;
        float text_y = ay1 + ah * 0.7f;
        draw_text(text_x, text_y, widget->field_value, text_size, 0, 0, 0, 255);
      }
      // Render checkmark for button fields (checkboxes)
      else if (widget->field_type == FieldType::Button) {
        // Check if the field is checked (value is not "Off")
        if (widget->field_value != "Off" && !widget->field_value.empty()) {
          // Draw checkmark
          auto check = tvg::Shape::gen();
          if (check) {
            float cx = ax1 + aw * 0.5f;
            float cy = ay1 + ah * 0.5f;
            float s = std::min(aw, ah) * 0.35f;

            // Simple checkmark path
            check->moveTo(cx - s, cy);
            check->lineTo(cx - s * 0.3f, cy + s * 0.6f);
            check->lineTo(cx + s, cy - s * 0.5f);

            check->strokeWidth(2.0f * scale);
            check->strokeFill(0, 0, 0, 255);
            check->strokeCap(tvg::StrokeCap::Round);
            check->strokeJoin(tvg::StrokeJoin::Round);

            scene_->add(std::move(check));
          }
        }
      }
    }
    // Other annotation types could be rendered here (links, highlights, etc.)
  }

  if (!end_scene()) {
    result.error = "Failed to end scene";
    return result;
  }

  return get_buffer();
}

bool ThorVGBackend::parse_pdf_content(const std::vector<uint8_t>& content_data) {
  // Enhanced PDF content parser with more operators support

  std::string content(content_data.begin(), content_data.end());

  std::vector<std::string> operands;
  std::vector<GraphicsState> state_stack;  // For save/restore state

  size_t pos = 0;
  while (pos < content.size()) {
    // Skip whitespace
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' ||
           content[pos] == '\r' || content[pos] == '\n')) {
      pos++;
    }
    if (pos >= content.size()) break;

    // Skip comments (% to end of line)
    if (content[pos] == '%') {
      while (pos < content.size() && content[pos] != '\r' && content[pos] != '\n') {
        pos++;
      }
      continue;
    }

    std::string token;

    // Check for hex string <...>
    if (content[pos] == '<') {
      size_t end = content.find('>', pos);
      if (end != std::string::npos) {
        token = content.substr(pos, end - pos + 1);
        pos = end + 1;
      } else {
        pos++;
        continue;
      }
    }
    // Check for literal string (...)
    else if (content[pos] == '(') {
      int depth = 1;
      size_t start = pos;
      pos++;
      while (pos < content.size() && depth > 0) {
        if (content[pos] == '(' && (pos == 0 || content[pos-1] != '\\')) {
          depth++;
        } else if (content[pos] == ')' && (pos == 0 || content[pos-1] != '\\')) {
          depth--;
        }
        pos++;
      }
      token = content.substr(start, pos - start);
    }
    // Check for array [...] - skip for now, just collect the bracket
    else if (content[pos] == '[') {
      token = "[";
      pos++;
    }
    else if (content[pos] == ']') {
      token = "]";
      pos++;
    }
    // Regular token
    else {
      size_t start = pos;
      while (pos < content.size() && content[pos] != ' ' && content[pos] != '\t' &&
             content[pos] != '\r' && content[pos] != '\n' && content[pos] != '<' &&
             content[pos] != '(' && content[pos] != '[' && content[pos] != ']') {
        pos++;
      }
      token = content.substr(start, pos - start);
    }

    if (token.empty()) continue;

    // Check if token is an operator (starts with a letter or special chars)
    bool is_operator = false;
    if (!token.empty()) {
      char first = token[0];
      // Operators start with letters or are special like ' " *
      // But not if it looks like a hex string or literal string
      if (first != '<' && first != '(' && first != '[' && first != ']') {
        if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') ||
            first == '*' || first == '\'' || first == '"') {
          is_operator = true;
        }
      }
    }

    if (is_operator) {
      // Process operator with accumulated operands

      // Path construction operators
      if (token == "m") {  // moveTo
        if (operands.size() >= 2) {
          float x = std::stof(operands[0]);
          float y = std::stof(operands[1]);

          // Apply transformation
          state_.transform.transform(x, y);

          state_.current_x = x;
          state_.current_y = height_ - y;  // Flip Y coordinate
          state_.path_commands.push_back(tvg::PathCommand::MoveTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
          state_.in_path = true;
        }
      } else if (token == "l") {  // lineTo
        if (operands.size() >= 2) {
          float x = std::stof(operands[0]);
          float y = std::stof(operands[1]);

          // Apply transformation
          state_.transform.transform(x, y);

          state_.current_x = x;
          state_.current_y = height_ - y;  // Flip Y coordinate
          state_.path_commands.push_back(tvg::PathCommand::LineTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
        }
      } else if (token == "c") {  // curveTo (cubic Bezier)
        if (operands.size() >= 6) {
          float x1 = std::stof(operands[0]);
          float y1 = std::stof(operands[1]);
          float x2 = std::stof(operands[2]);
          float y2 = std::stof(operands[3]);
          float x3 = std::stof(operands[4]);
          float y3 = std::stof(operands[5]);

          // Apply transformation
          state_.transform.transform(x1, y1);
          state_.transform.transform(x2, y2);
          state_.transform.transform(x3, y3);

          // Flip Y coordinates
          y1 = height_ - y1;
          y2 = height_ - y2;
          y3 = height_ - y3;

          state_.path_commands.push_back(tvg::PathCommand::CubicTo);
          state_.path_points.push_back({x1, y1});
          state_.path_points.push_back({x2, y2});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "v") {  // curveTo variant (first control point = current point)
        if (operands.size() >= 4) {
          float x2 = std::stof(operands[0]);
          float y2 = height_ - std::stof(operands[1]);
          float x3 = std::stof(operands[2]);
          float y3 = height_ - std::stof(operands[3]);
          state_.path_commands.push_back(tvg::PathCommand::CubicTo);
          state_.path_points.push_back({state_.current_x, state_.current_y});
          state_.path_points.push_back({x2, y2});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "y") {  // curveTo variant (second control point = end point)
        if (operands.size() >= 4) {
          float x1 = std::stof(operands[0]);
          float y1 = height_ - std::stof(operands[1]);
          float x3 = std::stof(operands[2]);
          float y3 = height_ - std::stof(operands[3]);
          state_.path_commands.push_back(tvg::PathCommand::CubicTo);
          state_.path_points.push_back({x1, y1});
          state_.path_points.push_back({x3, y3});
          state_.path_points.push_back({x3, y3});
          state_.current_x = x3;
          state_.current_y = y3;
        }
      } else if (token == "re") {  // rectangle
        if (operands.size() >= 4) {
          float x = std::stof(operands[0]);
          float y_pdf = std::stof(operands[1]);
          float w = std::stof(operands[2]);
          float h = std::stof(operands[3]);

          // Convert to canvas coordinates: flip Y using page height, then scale
          float y = (state_.page_height - y_pdf - h) * state_.scale;  // Flip Y and adjust for height
          x = x * state_.scale;
          w = w * state_.scale;
          h = h * state_.scale;

          // Add rectangle to path
          state_.path_commands.push_back(tvg::PathCommand::MoveTo);
          state_.path_points.push_back({x, y});
          state_.path_commands.push_back(tvg::PathCommand::LineTo);
          state_.path_points.push_back({x + w, y});
          state_.path_commands.push_back(tvg::PathCommand::LineTo);
          state_.path_points.push_back({x + w, y + h});
          state_.path_commands.push_back(tvg::PathCommand::LineTo);
          state_.path_points.push_back({x, y + h});
          state_.path_commands.push_back(tvg::PathCommand::Close);
          state_.in_path = true;
        }
      } else if (token == "h") {  // Close path
        if (state_.in_path) {
          state_.path_commands.push_back(tvg::PathCommand::Close);
        }
      }
      // Path painting operators
      else if (token == "f" || token == "F" || token == "f*") {  // fill (various winding rules)
        if (!state_.path_commands.empty()) {
          auto shape = tvg::Shape::gen();
          if (shape) {
            shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                             state_.path_points.data(), state_.path_points.size());

            // Set fill rule: f*/F* use even-odd, f/F use non-zero
            if (token == "f*") {
              shape->fillRule(tvg::FillRule::EvenOdd);
            } else {
              shape->fillRule(tvg::FillRule::NonZero);
            }

            // Check for pattern fill
            if (!state_.fill_pattern.empty()) {
              apply_pattern_fill(shape, state_.fill_pattern, false);
            } else {
              // Apply solid fill with opacity
              uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);
              shape->fill(state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
            }
            push_with_clip(shape);
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
        }
      } else if (token == "s") {  // close and stroke
        if (state_.in_path) {
          state_.path_commands.push_back(tvg::PathCommand::Close);
        }
        // Fall through to stroke
      }
      if (token == "S" || token == "s") {  // stroke
        if (!state_.path_commands.empty()) {
          // Create stroked shape
          auto shape = tvg::Shape::gen();
          if (shape) {
            shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                             state_.path_points.data(), state_.path_points.size());
            // Apply stroke style from graphics state
            shape->strokeWidth(state_.stroke_width * state_.scale);

            // Check for stroke pattern
            if (!state_.stroke_pattern.empty()) {
              apply_pattern_fill(shape, state_.stroke_pattern, true);
            } else {
              uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
              shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);
            }
            // Set line cap: 0=butt, 1=round, 2=square
            tvg::StrokeCap cap = tvg::StrokeCap::Butt;
            if (state_.line_cap == 1) cap = tvg::StrokeCap::Round;
            else if (state_.line_cap == 2) cap = tvg::StrokeCap::Square;
            shape->strokeCap(cap);
            // Set line join: 0=miter, 1=round, 2=bevel
            tvg::StrokeJoin join = tvg::StrokeJoin::Miter;
            if (state_.line_join == 1) join = tvg::StrokeJoin::Round;
            else if (state_.line_join == 2) join = tvg::StrokeJoin::Bevel;
            shape->strokeJoin(join);
            shape->strokeMiterlimit(state_.miter_limit);
            // Apply dash pattern if set
            if (!state_.dash_pattern.empty()) {
              std::vector<float> scaled_dash;
              for (float d : state_.dash_pattern) {
                scaled_dash.push_back(d * state_.scale);
              }
              shape->strokeDash(scaled_dash.data(), scaled_dash.size(),
                               state_.dash_phase * state_.scale);
            }
            push_with_clip(shape);
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
        }
      } else if (token == "b" || token == "b*") {  // close, fill and stroke
        if (state_.in_path) {
          state_.path_commands.push_back(tvg::PathCommand::Close);
        }
        if (!state_.path_commands.empty()) {
          // Fill first
          {
            auto fill_shape = tvg::Shape::gen();
            if (fill_shape) {
              fill_shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                                    state_.path_points.data(), state_.path_points.size());
              // Set fill rule
              if (token == "b*") {
                fill_shape->fillRule(tvg::FillRule::EvenOdd);
              } else {
                fill_shape->fillRule(tvg::FillRule::NonZero);
              }
              // Check for pattern fill
              if (!state_.fill_pattern.empty()) {
                apply_pattern_fill(fill_shape, state_.fill_pattern, false);
              } else {
                uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);
                fill_shape->fill(state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
              }
              push_with_clip(fill_shape);
            }
          }

          // Stroke with style
          auto shape = tvg::Shape::gen();
          if (shape) {
            shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                             state_.path_points.data(), state_.path_points.size());
            shape->strokeWidth(state_.stroke_width * state_.scale);
            // Check for stroke pattern
            if (!state_.stroke_pattern.empty()) {
              apply_pattern_fill(shape, state_.stroke_pattern, true);
            } else {
              uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
              shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);
            }
            tvg::StrokeCap cap = tvg::StrokeCap::Butt;
            if (state_.line_cap == 1) cap = tvg::StrokeCap::Round;
            else if (state_.line_cap == 2) cap = tvg::StrokeCap::Square;
            shape->strokeCap(cap);
            tvg::StrokeJoin join = tvg::StrokeJoin::Miter;
            if (state_.line_join == 1) join = tvg::StrokeJoin::Round;
            else if (state_.line_join == 2) join = tvg::StrokeJoin::Bevel;
            shape->strokeJoin(join);
            shape->strokeMiterlimit(state_.miter_limit);
            // Apply dash pattern if set
            if (!state_.dash_pattern.empty()) {
              std::vector<float> scaled_dash;
              for (float d : state_.dash_pattern) {
                scaled_dash.push_back(d * state_.scale);
              }
              shape->strokeDash(scaled_dash.data(), scaled_dash.size(),
                               state_.dash_phase * state_.scale);
            }
            push_with_clip(shape);
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
        }
      } else if (token == "B" || token == "B*") {  // fill and stroke
        if (!state_.path_commands.empty()) {
          // Fill first
          {
            auto fill_shape = tvg::Shape::gen();
            if (fill_shape) {
              fill_shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                                    state_.path_points.data(), state_.path_points.size());
              // Set fill rule
              if (token == "B*") {
                fill_shape->fillRule(tvg::FillRule::EvenOdd);
              } else {
                fill_shape->fillRule(tvg::FillRule::NonZero);
              }
              // Check for pattern fill
              if (!state_.fill_pattern.empty()) {
                apply_pattern_fill(fill_shape, state_.fill_pattern, false);
              } else {
                uint8_t fill_alpha = static_cast<uint8_t>(state_.fill_a * state_.fill_opacity);
                fill_shape->fill(state_.fill_r, state_.fill_g, state_.fill_b, fill_alpha);
              }
              push_with_clip(fill_shape);
            }
          }

          // Stroke with style
          auto shape = tvg::Shape::gen();
          if (shape) {
            shape->appendPath(state_.path_commands.data(), state_.path_commands.size(),
                             state_.path_points.data(), state_.path_points.size());
            shape->strokeWidth(state_.stroke_width * state_.scale);
            // Check for stroke pattern
            if (!state_.stroke_pattern.empty()) {
              apply_pattern_fill(shape, state_.stroke_pattern, true);
            } else {
              uint8_t stroke_alpha = static_cast<uint8_t>(state_.stroke_a * state_.stroke_opacity);
              shape->strokeFill(state_.stroke_r, state_.stroke_g, state_.stroke_b, stroke_alpha);
            }
            tvg::StrokeCap cap = tvg::StrokeCap::Butt;
            if (state_.line_cap == 1) cap = tvg::StrokeCap::Round;
            else if (state_.line_cap == 2) cap = tvg::StrokeCap::Square;
            shape->strokeCap(cap);
            tvg::StrokeJoin join = tvg::StrokeJoin::Miter;
            if (state_.line_join == 1) join = tvg::StrokeJoin::Round;
            else if (state_.line_join == 2) join = tvg::StrokeJoin::Bevel;
            shape->strokeJoin(join);
            shape->strokeMiterlimit(state_.miter_limit);
            // Apply dash pattern if set
            if (!state_.dash_pattern.empty()) {
              std::vector<float> scaled_dash;
              for (float d : state_.dash_pattern) {
                scaled_dash.push_back(d * state_.scale);
              }
              shape->strokeDash(scaled_dash.data(), scaled_dash.size(),
                               state_.dash_phase * state_.scale);
            }
            push_with_clip(shape);
          }
          state_.path_commands.clear();
          state_.path_points.clear();
          state_.in_path = false;
        }
      } else if (token == "n") {  // End path without fill or stroke
        state_.path_commands.clear();
        state_.path_points.clear();
        state_.in_path = false;
      }
      // Clipping path operators
      else if (token == "W" || token == "W*") {
        // Set clipping path using current path
        // W = non-zero winding rule, W* = even-odd rule
        // The path will be used by subsequent paint operations
        if (!state_.path_commands.empty()) {
          // If we already have a clip, we need to intersect the new clip with it
          // For simplicity, we replace the clip (proper intersection is complex)
          state_.has_clip = true;
          state_.clip_even_odd = (token == "W*");
          state_.clip_commands = state_.path_commands;
          state_.clip_points = state_.path_points;
        }
      }
      // Color operators
      else if (token == "rg") {  // Set RGB fill color
        if (operands.size() >= 3) {
          state_.fill_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.fill_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
          state_.fill_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
        }
      } else if (token == "RG") {  // Set RGB stroke color
        if (operands.size() >= 3) {
          state_.stroke_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.stroke_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
          state_.stroke_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
        }
      } else if (token == "g") {  // Set gray fill color
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.fill_r = state_.fill_g = state_.fill_b = gray;
        }
      } else if (token == "G") {  // Set gray stroke color
        if (operands.size() >= 1) {
          uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
          state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
        }
      } else if (token == "k") {  // Set CMYK fill color (simplified to RGB)
        if (operands.size() >= 4) {
          float c = std::stof(operands[0]);
          float m = std::stof(operands[1]);
          float y = std::stof(operands[2]);
          float k = std::stof(operands[3]);
          // Simple CMYK to RGB conversion
          state_.fill_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.fill_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.fill_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      } else if (token == "K") {  // Set CMYK stroke color (simplified to RGB)
        if (operands.size() >= 4) {
          float c = std::stof(operands[0]);
          float m = std::stof(operands[1]);
          float y = std::stof(operands[2]);
          float k = std::stof(operands[3]);
          // Simple CMYK to RGB conversion
          state_.stroke_r = static_cast<uint8_t>((1.0f - c) * (1.0f - k) * 255);
          state_.stroke_g = static_cast<uint8_t>((1.0f - m) * (1.0f - k) * 255);
          state_.stroke_b = static_cast<uint8_t>((1.0f - y) * (1.0f - k) * 255);
        }
      }
      // Color space operators
      else if (token == "cs") {  // Set non-stroking color space
        if (operands.size() >= 1) {
          std::string cs_name = operands[0];
          if (!cs_name.empty() && cs_name[0] == '/') {
            cs_name = cs_name.substr(1);
          }
          state_.fill_color_space = cs_name;
          state_.fill_pattern.clear();  // Clear any previous pattern
          NANOPDF_LOG_TRACE("ThorVG", "cs: set fill color space to '%s'", cs_name.c_str());
        }
      } else if (token == "CS") {  // Set stroking color space
        if (operands.size() >= 1) {
          std::string cs_name = operands[0];
          if (!cs_name.empty() && cs_name[0] == '/') {
            cs_name = cs_name.substr(1);
          }
          state_.stroke_color_space = cs_name;
          state_.stroke_pattern.clear();  // Clear any previous pattern
          NANOPDF_LOG_TRACE("ThorVG", "CS: set stroke color space to '%s'", cs_name.c_str());
        }
      } else if (token == "sc" || token == "scn") {  // Set non-stroking color
        // Check if last operand is a pattern name (starts with /)
        bool is_pattern = false;
        if (!operands.empty()) {
          const std::string& last = operands.back();
          if (!last.empty() && last[0] == '/') {
            // This is a pattern name
            std::string pattern_name = last.substr(1);
            state_.fill_pattern = pattern_name;
            is_pattern = true;
            NANOPDF_LOG_TRACE("ThorVG", "scn: set fill pattern to '%s'", pattern_name.c_str());
          }
        }

        if (!is_pattern) {
          state_.fill_pattern.clear();  // Not using a pattern
          // Color values depend on the current color space
          if (operands.size() >= 3) {
            state_.fill_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.fill_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
            state_.fill_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
          } else if (operands.size() >= 1) {
            // Grayscale
            uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.fill_r = state_.fill_g = state_.fill_b = gray;
          }
        }
      } else if (token == "SC" || token == "SCN") {  // Set stroking color
        // Check if last operand is a pattern name (starts with /)
        bool is_pattern = false;
        if (!operands.empty()) {
          const std::string& last = operands.back();
          if (!last.empty() && last[0] == '/') {
            // This is a pattern name
            std::string pattern_name = last.substr(1);
            state_.stroke_pattern = pattern_name;
            is_pattern = true;
            NANOPDF_LOG_TRACE("ThorVG", "SCN: set stroke pattern to '%s'", pattern_name.c_str());
          }
        }

        if (!is_pattern) {
          state_.stroke_pattern.clear();  // Not using a pattern
          // Color values depend on the current color space
          if (operands.size() >= 3) {
            state_.stroke_r = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.stroke_g = static_cast<uint8_t>(std::stof(operands[1]) * 255);
            state_.stroke_b = static_cast<uint8_t>(std::stof(operands[2]) * 255);
          } else if (operands.size() >= 1) {
            // Grayscale
            uint8_t gray = static_cast<uint8_t>(std::stof(operands[0]) * 255);
            state_.stroke_r = state_.stroke_g = state_.stroke_b = gray;
          }
        }
      }
      // Line style operators
      else if (token == "w") {  // Set line width
        if (operands.size() >= 1) {
          state_.stroke_width = std::stof(operands[0]);
        }
      } else if (token == "J") {  // Set line cap style
        if (operands.size() >= 1) {
          state_.line_cap = std::stoi(operands[0]);
        }
      } else if (token == "j") {  // Set line join style
        if (operands.size() >= 1) {
          state_.line_join = std::stoi(operands[0]);
        }
      } else if (token == "M") {  // Set miter limit
        if (operands.size() >= 1) {
          state_.miter_limit = std::stof(operands[0]);
        }
      } else if (token == "d") {  // Set dash pattern
        // Dash patterns: [array] phase
        // operands should contain: "[", values..., "]", phase
        state_.dash_pattern.clear();
        state_.dash_phase = 0.0f;

        bool in_array = false;
        for (size_t i = 0; i < operands.size(); i++) {
          if (operands[i] == "[") {
            in_array = true;
          } else if (operands[i] == "]") {
            in_array = false;
          } else if (in_array) {
            try {
              state_.dash_pattern.push_back(std::stof(operands[i]));
            } catch (...) {}
          } else if (!in_array && i == operands.size() - 1) {
            // Last operand after "]" is the phase
            try {
              state_.dash_phase = std::stof(operands[i]);
            } catch (...) {}
          }
        }
      } else if (token == "ri") {  // Set rendering intent
        if (!operands.empty()) {
          state_.rendering_intent = operands[0];
        }
      } else if (token == "i") {  // Set flatness tolerance
        if (!operands.empty()) {
          try {
            state_.flatness = std::stof(operands[0]);
          } catch (...) {}
        }
      }
      // Graphics state operators
      else if (token == "q") {  // Save graphics state
        state_stack.push_back(state_);
      } else if (token == "Q") {  // Restore graphics state
        if (!state_stack.empty()) {
          state_ = state_stack.back();
          state_stack.pop_back();
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
                // Map PDF blend mode to ThorVG blend method
                // ThorVG BlendMethod: Normal=0, Add=1, Screen=2, Multiply=3, Overlay=4,
                //   Difference=5, Exclusion=6, SrcOver=7, Darken=8, Lighten=9, ColorDodge=10,
                //   ColorBurn=11, HardLight=12, SoftLight=13
                if (bm_name == "Normal" || bm_name == "Compatible") {
                  state_.blend_mode = 0;  // Normal
                } else if (bm_name == "Multiply") {
                  state_.blend_mode = 3;  // Multiply
                } else if (bm_name == "Screen") {
                  state_.blend_mode = 2;  // Screen
                } else if (bm_name == "Overlay") {
                  state_.blend_mode = 4;  // Overlay
                } else if (bm_name == "Darken") {
                  state_.blend_mode = 8;  // Darken
                } else if (bm_name == "Lighten") {
                  state_.blend_mode = 9;  // Lighten
                } else if (bm_name == "ColorDodge") {
                  state_.blend_mode = 10;  // ColorDodge
                } else if (bm_name == "ColorBurn") {
                  state_.blend_mode = 11;  // ColorBurn
                } else if (bm_name == "HardLight") {
                  state_.blend_mode = 12;  // HardLight
                } else if (bm_name == "SoftLight") {
                  state_.blend_mode = 13;  // SoftLight
                } else if (bm_name == "Difference") {
                  state_.blend_mode = 5;  // Difference
                } else if (bm_name == "Exclusion") {
                  state_.blend_mode = 6;  // Exclusion
                } else if (bm_name == "Hue" || bm_name == "Saturation" ||
                           bm_name == "Color" || bm_name == "Luminosity") {
                  // Non-separable blend modes - ThorVG doesn't support these natively
                  // Fall back to Normal. For proper support, would need custom blending.
                  state_.blend_mode = 0;  // Normal (fallback)
                } else {
                  state_.blend_mode = 0;  // Default to Normal
                }
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
      // Transformation matrix operators
      else if (token == "cm") {  // Concatenate matrix
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
      // Text operators
      else if (token == "BT") {  // Begin text
        state_.in_text_block = true;
        state_.text_matrix.reset();
        state_.text_line_matrix.reset();
      } else if (token == "ET") {  // End text
        state_.in_text_block = false;

        // If text clipping was active, apply accumulated text path to clipping
        if (state_.text_clip_active && !state_.text_clip_commands.empty()) {
          // Merge text clip path into main clip path
          state_.clip_commands.insert(state_.clip_commands.end(),
                                       state_.text_clip_commands.begin(),
                                       state_.text_clip_commands.end());
          state_.clip_points.insert(state_.clip_points.end(),
                                     state_.text_clip_points.begin(),
                                     state_.text_clip_points.end());
          state_.has_clip = true;

          // Clear text clip state
          state_.text_clip_commands.clear();
          state_.text_clip_points.clear();
          state_.text_clip_active = false;
        }
      } else if (token == "Td") {  // Move text position
        if (operands.size() >= 2 && state_.in_text_block) {
          float tx = std::stof(operands[0]);
          float ty = std::stof(operands[1]);
          // Td translates the text matrix
          state_.text_matrix.e += tx * state_.text_matrix.a + ty * state_.text_matrix.c;
          state_.text_matrix.f += tx * state_.text_matrix.b + ty * state_.text_matrix.d;
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "TD") {  // Move text position and set leading
        if (operands.size() >= 2 && state_.in_text_block) {
          float tx = std::stof(operands[0]);
          float ty = std::stof(operands[1]);
          state_.text_leading = -ty;  // TL = -ty
          // Same as Td
          state_.text_matrix.e += tx * state_.text_matrix.a + ty * state_.text_matrix.c;
          state_.text_matrix.f += tx * state_.text_matrix.b + ty * state_.text_matrix.d;
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "Tm") {  // Set text matrix
        if (operands.size() >= 6 && state_.in_text_block) {
          // Text matrix [a b c d e f]
          state_.text_matrix.a = std::stof(operands[0]);
          state_.text_matrix.b = std::stof(operands[1]);
          state_.text_matrix.c = std::stof(operands[2]);
          state_.text_matrix.d = std::stof(operands[3]);
          state_.text_matrix.e = std::stof(operands[4]);
          state_.text_matrix.f = std::stof(operands[5]);
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "T*") {  // Move to start of next line
        if (state_.in_text_block) {
          // Equivalent to: 0 -TL Td
          // T* moves to the start of the next line using text_leading
          float leading = state_.text_leading;
          if (leading == 0) leading = state_.font_size;  // Default leading

          // Move down by leading (in PDF, Y increases upward, so subtract)
          // For identity matrix: e stays same (x), f decreases (y)
          state_.text_matrix.e = state_.text_line_matrix.e;  // Reset to line start X
          state_.text_matrix.f = state_.text_line_matrix.f - leading;  // Move down by leading
          state_.text_line_matrix = state_.text_matrix;
        }
      } else if (token == "TL") {  // Set text leading
        if (operands.size() >= 1) {
          state_.text_leading = std::stof(operands[0]);
        }
      } else if (token == "Tc") {  // Set character spacing
        if (operands.size() >= 1) {
          state_.char_spacing = std::stof(operands[0]);
        }
      } else if (token == "Tw") {  // Set word spacing
        if (operands.size() >= 1) {
          state_.word_spacing = std::stof(operands[0]);
        }
      } else if (token == "Tz") {  // Set horizontal scaling
        if (operands.size() >= 1) {
          state_.horiz_scaling = std::stof(operands[0]);
        }
      } else if (token == "Ts") {  // Set text rise
        if (operands.size() >= 1) {
          state_.text_rise = static_cast<int>(std::stof(operands[0]));
        }
      } else if (token == "Tr") {  // Set text rendering mode
        // 0 = Fill, 1 = Stroke, 2 = Fill+Stroke, 3 = Invisible
        // 4 = Fill+Clip, 5 = Stroke+Clip, 6 = Fill+Stroke+Clip, 7 = Clip only
        if (operands.size() >= 1) {
          state_.text_render_mode = std::stoi(operands[0]);
          NANOPDF_LOG_TRACE("ThorVG", "Tr: set text rendering mode to %d", state_.text_render_mode);
        }
      } else if (token == "Tf") {  // Set font and size
        if (operands.size() >= 2) {
          // operands[0] is font name (with leading /), operands[1] is size
          std::string font_name = operands[0];
          if (!font_name.empty() && font_name[0] == '/') {
            font_name = font_name.substr(1);  // Remove leading /
          }
          state_.font_size = std::stof(operands[operands.size() - 1]);

          // Try to load the font from page resources
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
      } else if (token == "Tj") {  // Show text
        if (operands.size() >= 1 && state_.in_text_block) {
          // Remove parentheses or angle brackets from text string
          std::string text = operands[0];
          if (!text.empty() && text[0] == '(' && text.back() == ')') {
            text = text.substr(1, text.length() - 2);
          } else if (!text.empty() && text[0] == '<' && text.back() == '>') {
            // Hex string - decode hex pairs to bytes
            std::string hex_str = text.substr(1, text.length() - 2);
            text.clear();
            for (size_t i = 0; i + 1 < hex_str.length(); i += 2) {
              char hex_byte[3] = {hex_str[i], hex_str[i+1], '\0'};
              int byte_val = static_cast<int>(strtol(hex_byte, nullptr, 16));
              text += static_cast<char>(byte_val);
            }
          }

          // Get text position from text matrix
          float text_x = state_.text_matrix.e;
          float text_y = state_.text_matrix.f;

          // Transform from PDF coordinates to canvas coordinates
          float canvas_x = text_x * state_.scale;
          float canvas_y = (state_.page_height - text_y) * state_.scale;

          // Calculate effective font size (considering text matrix scale)
          float font_scale = std::abs(state_.text_matrix.d);
          if (font_scale < 0.01f) font_scale = 1.0f;
          float scaled_size = state_.font_size * font_scale * state_.scale;

          draw_text(canvas_x, canvas_y, text, scaled_size,
                   state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);

          // Advance text matrix by actual text width using font metrics
          float text_advance = calculate_text_width(text, state_.font_size);
          state_.text_matrix.e += text_advance * state_.text_matrix.a;
          state_.text_matrix.f += text_advance * state_.text_matrix.b;
        }
      } else if (token == "TJ") {  // Show text with positioning array
        // TJ takes an array of strings and positioning adjustments
        if (state_.in_text_block) {
          for (const auto& item : operands) {
            if (!item.empty() && (item[0] == '(' || item[0] == '<')) {
              // Text string element
              std::string text = item;
              if (text[0] == '(' && text.back() == ')') {
                text = text.substr(1, text.length() - 2);
              } else if (text[0] == '<' && text.back() == '>') {
                // Hex string - decode hex pairs to bytes
                std::string hex_str = text.substr(1, text.length() - 2);
                text.clear();
                for (size_t i = 0; i + 1 < hex_str.length(); i += 2) {
                  char hex_byte[3] = {hex_str[i], hex_str[i+1], '\0'};
                  int byte_val = static_cast<int>(strtol(hex_byte, nullptr, 16));
                  text += static_cast<char>(byte_val);
                }
              }
              if (!text.empty()) {
                float text_x = state_.text_matrix.e;
                float text_y = state_.text_matrix.f;
                float canvas_x = text_x * state_.scale;
                float canvas_y = (state_.page_height - text_y) * state_.scale;
                float font_scale = std::abs(state_.text_matrix.d);
                if (font_scale < 0.01f) font_scale = 1.0f;
                float scaled_size = state_.font_size * font_scale * state_.scale;

                draw_text(canvas_x, canvas_y, text, scaled_size,
                         state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);

                // Advance by actual text width using font metrics
                float text_advance = calculate_text_width(text, state_.font_size);
                state_.text_matrix.e += text_advance * state_.text_matrix.a;
                state_.text_matrix.f += text_advance * state_.text_matrix.b;
              }
            } else if (!item.empty() && item[0] != '[' && item[0] != ']') {
              // Numeric positioning adjustment
              // Positive values move left, negative move right (in thousandths of em)
              try {
                float adjustment = std::stof(item);
                // Convert from thousandths of em to text space units
                float tx = -adjustment * state_.font_size / 1000.0f;
                state_.text_matrix.e += tx * state_.text_matrix.a;
                state_.text_matrix.f += tx * state_.text_matrix.b;
              } catch (...) {
                // Ignore non-numeric items
              }
            }
          }
        }
      } else if (token == "'" || token == "\"") {  // Move to next line and show text
        if (operands.size() >= 1 && state_.in_text_block) {
          // Move to next line (T*)
          float ty = -state_.text_leading;
          if (ty == 0) ty = -state_.font_size;
          state_.text_matrix.e += ty * state_.text_matrix.c;
          state_.text_matrix.f += ty * state_.text_matrix.d;
          state_.text_line_matrix = state_.text_matrix;

          // For " operator, first two operands are word and char spacing
          size_t text_idx = operands.size() - 1;
          if (token == "\"" && operands.size() >= 3) {
            state_.word_spacing = std::stof(operands[0]);
            state_.char_spacing = std::stof(operands[1]);
          }

          std::string text = operands[text_idx];
          if (!text.empty() && text[0] == '(' && text.back() == ')') {
            text = text.substr(1, text.length() - 2);
          } else if (!text.empty() && text[0] == '<' && text.back() == '>') {
            std::string hex_str = text.substr(1, text.length() - 2);
            text.clear();
            for (size_t i = 0; i + 1 < hex_str.length(); i += 2) {
              char hex_byte[3] = {hex_str[i], hex_str[i+1], '\0'};
              int byte_val = static_cast<int>(strtol(hex_byte, nullptr, 16));
              text += static_cast<char>(byte_val);
            }
          }

          float text_x = state_.text_matrix.e;
          float text_y = state_.text_matrix.f;
          float canvas_x = text_x * state_.scale;
          float canvas_y = (state_.page_height - text_y) * state_.scale;
          float font_scale = std::abs(state_.text_matrix.d);
          if (font_scale < 0.01f) font_scale = 1.0f;
          float scaled_size = state_.font_size * font_scale * state_.scale;

          draw_text(canvas_x, canvas_y, text, scaled_size,
                   state_.fill_r, state_.fill_g, state_.fill_b, state_.fill_a);
        }
      }
      // Extended graphics state operator
      else if (token == "gs") {  // Set graphics state from dictionary
        if (operands.size() >= 1 && current_pdf_ && current_page_) {
          std::string gs_name = operands[0];
          if (!gs_name.empty() && gs_name[0] == '/') {
            gs_name = gs_name.substr(1);
          }

          // Look up ExtGState in page resources
          auto ext_it = current_page_->resources.find("ExtGState");
          if (ext_it != current_page_->resources.end()) {
            Value ext_dict = ext_it->second;
            // Resolve reference if needed
            if (ext_dict.type == Value::REFERENCE) {
              ResolvedObject resolved = resolve_reference(*current_pdf_,
                  ext_dict.ref_object_number, ext_dict.ref_generation_number);
              if (resolved.success) {
                ext_dict = resolved.value;
              }
            }

            if (ext_dict.type == Value::DICTIONARY) {
              auto gs_it = ext_dict.dict.find(gs_name);
              if (gs_it != ext_dict.dict.end()) {
                Value gs_dict = gs_it->second;
                // Resolve reference if needed
                if (gs_dict.type == Value::REFERENCE) {
                  ResolvedObject resolved = resolve_reference(*current_pdf_,
                      gs_dict.ref_object_number, gs_dict.ref_generation_number);
                  if (resolved.success) {
                    gs_dict = resolved.value;
                  }
                }

                if (gs_dict.type == Value::DICTIONARY) {
                  // Read opacity values (ca = fill alpha, CA = stroke alpha)
                  auto ca_it = gs_dict.dict.find("ca");  // Fill opacity
                  if (ca_it != gs_dict.dict.end() && ca_it->second.type == Value::NUMBER) {
                    state_.fill_opacity = static_cast<float>(ca_it->second.number);
                    if (state_.fill_opacity < 0.0f) state_.fill_opacity = 0.0f;
                    if (state_.fill_opacity > 1.0f) state_.fill_opacity = 1.0f;
                  }

                  auto CA_it = gs_dict.dict.find("CA");  // Stroke opacity
                  if (CA_it != gs_dict.dict.end() && CA_it->second.type == Value::NUMBER) {
                    state_.stroke_opacity = static_cast<float>(CA_it->second.number);
                    if (state_.stroke_opacity < 0.0f) state_.stroke_opacity = 0.0f;
                    if (state_.stroke_opacity > 1.0f) state_.stroke_opacity = 1.0f;
                  }

                  // Read line width if specified
                  auto lw_it = gs_dict.dict.find("LW");
                  if (lw_it != gs_dict.dict.end() && lw_it->second.type == Value::NUMBER) {
                    state_.stroke_width = static_cast<float>(lw_it->second.number);
                  }

                  // Read line cap if specified
                  auto lc_it = gs_dict.dict.find("LC");
                  if (lc_it != gs_dict.dict.end() && lc_it->second.type == Value::NUMBER) {
                    state_.line_cap = static_cast<int>(lc_it->second.number);
                  }

                  // Read line join if specified
                  auto lj_it = gs_dict.dict.find("LJ");
                  if (lj_it != gs_dict.dict.end() && lj_it->second.type == Value::NUMBER) {
                    state_.line_join = static_cast<int>(lj_it->second.number);
                  }

                  // Read miter limit if specified
                  auto ml_it = gs_dict.dict.find("ML");
                  if (ml_it != gs_dict.dict.end() && ml_it->second.type == Value::NUMBER) {
                    state_.miter_limit = static_cast<float>(ml_it->second.number);
                  }
                }
              }
            }
          }
        }
      }
      // Inline image (BI ... ID data EI)
      else if (token == "BI") {
        parse_inline_image(content, pos);
        operands.clear();
        continue;  // Skip normal operand clearing
      }
      // XObject (Do operator for images/forms)
      else if (token == "Do") {  // Paint XObject
        if (operands.size() >= 1 && current_pdf_ && current_page_) {
          std::string xobj_name = operands[0];
          // Remove leading '/' if present
          if (!xobj_name.empty() && xobj_name[0] == '/') {
            xobj_name = xobj_name.substr(1);
          }

          NANOPDF_LOG_DEBUG("ThorVG", "Do operator: looking up XObject '%s'", xobj_name.c_str());

          // Look up XObject in page resources
          auto xobj_it = current_page_->resources.find("XObject");
          if (xobj_it == current_page_->resources.end()) {
            // No XObject dictionary
            NANOPDF_LOG_DEBUG("ThorVG", "Do: no XObject dictionary in resources");
          } else if (xobj_it->second.type == Value::REFERENCE) {
            // XObject dict may be a reference, resolve it
            ResolvedObject resolved = resolve_reference(*current_pdf_,
                xobj_it->second.ref_object_number,
                xobj_it->second.ref_generation_number);
            if (resolved.success && resolved.value.type == Value::DICTIONARY) {
              auto entry_it = resolved.value.dict.find(xobj_name);
              if (entry_it != resolved.value.dict.end()) {
                Value xobj_value;
                uint32_t xobj_num = 0;
                uint16_t xobj_gen = 0;
                if (entry_it->second.type == Value::REFERENCE) {
                  xobj_num = entry_it->second.ref_object_number;
                  xobj_gen = entry_it->second.ref_generation_number;
                  ResolvedObject xobj_resolved = resolve_reference(*current_pdf_, xobj_num, xobj_gen);
                  if (xobj_resolved.success) {
                    xobj_value = std::move(xobj_resolved.value);
                  }
                } else {
                  xobj_value = entry_it->second;
                }

                if (xobj_value.type == Value::STREAM) {
                  auto subtype_it = xobj_value.stream.dict.find("Subtype");
                  if (subtype_it != xobj_value.stream.dict.end() &&
                      subtype_it->second.type == Value::NAME) {
                    if (subtype_it->second.name == "Image") {
                      ImageXObject image = parse_image_xobject(*current_pdf_, xobj_value, xobj_num, xobj_gen);
                      float img_x = state_.transform.e;
                      float img_y = state_.transform.f;
                      float img_width = state_.transform.a;
                      float img_height = state_.transform.d;
                      if (img_height < 0) {
                        img_y += img_height;
                        img_height = -img_height;
                      }
                      draw_image(image, img_x, img_y, img_width, img_height,
                          state_.fill_r, state_.fill_g, state_.fill_b);
                    } else if (subtype_it->second.name == "Form") {
                      // Form XObject - decode and parse its content stream
                      auto decoded = decode_stream(*current_pdf_, xobj_value, xobj_num, xobj_gen);
                      if (decoded.success && !decoded.data.empty()) {
                        // Save current state and apply Form's matrix if present
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

                        // Check for Form's BBox (bounding box)
                        auto bbox_it = xobj_value.stream.dict.find("BBox");
                        // Check for Form's Matrix
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

                        // Parse the Form's content stream
                        parse_pdf_content(decoded.data);

                        // Pop Form resources from stack
                        if (has_form_resources) {
                          form_resources_stack_.pop_back();
                        }

                        // Restore state
                        state_ = saved_state;
                      }
                    }
                  }
                }
              }
            }
          } else if (xobj_it->second.type == Value::DICTIONARY) {
            auto entry_it = xobj_it->second.dict.find(xobj_name);
            if (entry_it != xobj_it->second.dict.end()) {
              // Resolve reference if needed
              Value xobj_value;
              uint32_t xobj_num = 0;
              uint16_t xobj_gen = 0;
              if (entry_it->second.type == Value::REFERENCE) {
                xobj_num = entry_it->second.ref_object_number;
                xobj_gen = entry_it->second.ref_generation_number;
                ResolvedObject resolved = resolve_reference(*current_pdf_, xobj_num, xobj_gen);
                if (resolved.success) {
                  xobj_value = std::move(resolved.value);
                }
              } else {
                xobj_value = entry_it->second;
              }

              // Check if it's an image or form XObject
              if (xobj_value.type == Value::STREAM) {
                auto subtype_it = xobj_value.stream.dict.find("Subtype");
                if (subtype_it != xobj_value.stream.dict.end() &&
                    subtype_it->second.type == Value::NAME) {
                  if (subtype_it->second.name == "Image") {
                    // Parse and render the image (pass object numbers for decryption)
                    ImageXObject image = parse_image_xobject(*current_pdf_, xobj_value, xobj_num, xobj_gen);

                    // Get image dimensions from CTM (current transformation matrix)
                    float img_x = state_.transform.e;
                    float img_y = state_.transform.f;
                    float img_width = state_.transform.a;
                    float img_height = state_.transform.d;

                    // Handle negative height (flipped image)
                    if (img_height < 0) {
                      img_y += img_height;
                      img_height = -img_height;
                    }

                    draw_image(image, img_x, img_y, img_width, img_height,
                          state_.fill_r, state_.fill_g, state_.fill_b);
                  } else if (subtype_it->second.name == "Form") {
                    // Form XObject - decode and parse its content stream
                    auto decoded = decode_stream(*current_pdf_, xobj_value, xobj_num, xobj_gen);
                    if (decoded.success && !decoded.data.empty()) {
                      // Save current state and apply Form's matrix if present
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

                      // Check for Form's Matrix
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

                      // Parse the Form's content stream
                      parse_pdf_content(decoded.data);

                      // Pop Form resources from stack
                      if (has_form_resources) {
                        form_resources_stack_.pop_back();
                      }

                      // Restore state
                      state_ = saved_state;
                    }
                  }
                }
              }
            }
          }
        }
      }
      // Shading operator (sh) - paint a shading pattern
      else if (token == "sh") {
        if (operands.size() >= 1) {
          std::string shading_name = operands[0];
          // Remove leading '/' if present
          if (!shading_name.empty() && shading_name[0] == '/') {
            shading_name = shading_name.substr(1);
          }
          draw_shading(shading_name);
        }
      }

      operands.clear();
    } else {
      // Accumulate operand
      operands.push_back(token);
    }
  }

  return true;
}

bool ThorVGBackend::render_soft_mask_group(const Value& group_xobject, int mask_type) {
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

  // Create temporary canvas and scene for rendering the soft mask
  std::vector<uint32_t> mask_buffer(mask_width * mask_height, 0);

  // Initialize mask buffer based on mask type
  if (mask_type == 2) {  // Luminosity - start with white (ARGB = 0xFFFFFFFF)
    std::fill(mask_buffer.begin(), mask_buffer.end(), 0xFFFFFFFF);
  }
  // For alpha mask (type 1), buffer is already zeroed (transparent)

  tvg::SwCanvas* mask_canvas = tvg::SwCanvas::gen();
  if (!mask_canvas) {
    // Fallback to opaque mask
    state_.soft_mask_width = mask_width;
    state_.soft_mask_height = mask_height;
    state_.soft_mask_data.resize(mask_width * mask_height, 255);
    return true;
  }

  if (mask_canvas->target(mask_buffer.data(),
                          mask_width, mask_width, mask_height,
                          tvg::ColorSpace::ABGR8888) != tvg::Result::Success) {
    delete mask_canvas;
    state_.soft_mask_width = mask_width;
    state_.soft_mask_height = mask_height;
    state_.soft_mask_data.resize(mask_width * mask_height, 255);
    return true;
  }

  // Create temporary scene
  tvg::Scene* mask_scene = tvg::Scene::gen();
  if (!mask_scene) {
    delete mask_canvas;
    state_.soft_mask_width = mask_width;
    state_.soft_mask_height = mask_height;
    state_.soft_mask_data.resize(mask_width * mask_height, 255);
    return true;
  }

  // Save current state
  tvg::SwCanvas* saved_canvas = canvas_;
  tvg::Scene* saved_scene = scene_;
  std::vector<uint32_t> saved_buffer = std::move(buffer_);
  uint32_t saved_width = width_;
  uint32_t saved_height = height_;
  GraphicsState saved_state = state_;

  // Switch to mask canvas
  canvas_ = mask_canvas;
  scene_ = mask_scene;
  buffer_ = std::move(mask_buffer);
  width_ = mask_width;
  height_ = mask_height;

  // Reset graphics state for mask rendering
  state_ = GraphicsState();
  state_.page_width = std::abs(bbox[2] - bbox[0]);
  state_.page_height = std::abs(bbox[3] - bbox[1]);
  state_.scale = static_cast<float>(mask_width) / state_.page_width;

  // Parse and render the XObject content to the mask canvas
  parse_pdf_content(decoded.data);

  // Finalize rendering
  if (mask_canvas->add(mask_scene) == tvg::Result::Success) {
    mask_canvas->draw(true);
    mask_canvas->sync();
  }

  // Extract mask values from rendered buffer
  state_.soft_mask_width = mask_width;
  state_.soft_mask_height = mask_height;
  state_.soft_mask_data.resize(mask_width * mask_height);

  for (uint32_t y = 0; y < mask_height; ++y) {
    for (uint32_t x = 0; x < mask_width; ++x) {
      size_t idx = y * mask_width + x;

      // Extract BGRA components from ABGR8888 format pixel
      uint32_t pixel = buffer_[idx];
      uint8_t b = static_cast<uint8_t>(pixel & 0xFF);
      uint8_t g = static_cast<uint8_t>((pixel >> 8) & 0xFF);
      uint8_t r = static_cast<uint8_t>((pixel >> 16) & 0xFF);
      uint8_t a = static_cast<uint8_t>((pixel >> 24) & 0xFF);

      if (mask_type == 2) {  // Luminosity mask
        // Convert RGB to luminance using standard coefficients
        float luminance = 0.2126f * (r / 255.0f) + 0.7152f * (g / 255.0f) + 0.0722f * (b / 255.0f);
        state_.soft_mask_data[idx] = static_cast<uint8_t>(luminance * 255.0f);
      } else {  // Alpha mask
        state_.soft_mask_data[idx] = a;  // Alpha channel
      }
    }
  }

  // Clean up mask canvas (scene was pushed to canvas, so canvas owns it)
  delete mask_canvas;

  // Restore original state
  canvas_ = saved_canvas;
  scene_ = saved_scene;
  buffer_ = std::move(saved_buffer);
  width_ = saved_width;
  height_ = saved_height;

  // Restore graphics state but keep the extracted soft mask data
  auto soft_mask_data = std::move(state_.soft_mask_data);
  auto soft_mask_width = state_.soft_mask_width;
  auto soft_mask_height = state_.soft_mask_height;
  state_ = saved_state;
  state_.soft_mask_data = std::move(soft_mask_data);
  state_.soft_mask_width = soft_mask_width;
  state_.soft_mask_height = soft_mask_height;

  return true;
}

void ThorVGBackend::apply_soft_mask_to_context() {
  if (!state_.has_soft_mask || state_.soft_mask_data.empty()) {
    return;
  }

  // ThorVG doesn't have direct soft mask support like PDF
  // We simulate by adjusting the global alpha based on mask values
  // This is a simplified approximation

  // For proper implementation, we would need to:
  // 1. Render to an offscreen buffer
  // 2. Apply the soft mask as a per-pixel alpha multiplier
  // 3. Composite the result to the main canvas

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

#endif // NANOPDF_USE_THORVG
