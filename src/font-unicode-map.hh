// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include "nanopdf.hh"

namespace nanopdf {

inline bool try_map_tounicode(uint32_t char_code, const BaseFont* font,
                              uint32_t* unicode_out) {
  if (!font || !unicode_out) return false;
  if (font->to_unicode_cmap.code_to_unicode.empty()) return false;
  auto it = font->to_unicode_cmap.code_to_unicode.find(char_code);
  if (it == font->to_unicode_cmap.code_to_unicode.end()) return false;
  *unicode_out = it->second;
  return true;
}

inline bool is_identity_cmap(const Type0Font* type0_font) {
  if (!type0_font) return false;
  const std::string& cmap_name = type0_font->encoding_cmap.name;
  return cmap_name == "Identity-H" || cmap_name == "Identity-V";
}

// True when the Type0 font uses a two-byte CID encoding. Identity-H/V and the
// Adobe-registered CJK CMaps (UniJIS*, UniGB*, UniKS*, UniCNS*) all decode
// input bytes in pairs.
inline bool is_type0_two_byte_cid(const Type0Font* t0) {
  if (!t0) return false;
  const std::string& cn = t0->encoding_cmap.name;
  if (cn.find("Identity") != std::string::npos ||
      cn.find("UTF16") != std::string::npos ||
      cn.find("UCS2") != std::string::npos ||
      cn.find("UniJIS") != std::string::npos ||
      cn.find("UniGB") != std::string::npos ||
      cn.find("UniKS") != std::string::npos ||
      cn.find("UniCNS") != std::string::npos) {
    return true;
  }
  const std::string& o = t0->ordering;
  return o == "Japan1" || o == "GB1" || o == "CNS1" || o == "Korea1";
}

// Adobe-Japan1 CID → Unicode for the deterministic, closed-form ranges only.
// Returns 0 for CIDs outside the regular ranges (callers should then consult
// a per-backend or per-PDF fallback table, or rely on ToUnicode CMap).
//
// Covered:
//   CID 1              → U+0020 space
//   CID 2-94           → U+FF01..U+FF5D fullwidth ASCII punctuation/letters
//   CID 231-240        → U+FF10..U+FF19 fullwidth digits
//   CID 241-266        → U+FF21..U+FF3A fullwidth uppercase
//   CID 267-292        → U+FF41..U+FF5A fullwidth lowercase
//   CID 842-924        → U+3041..U+3093 hiragana
//   CID 925-1010       → U+30A1..U+30F6 katakana
//
// Kanji CIDs (1125+ in Adobe-Japan1) are not regular and vary between fonts;
// they require authoritative CMap data or the PDF's ToUnicode map.
inline uint32_t adobe_japan1_cid_to_unicode_regular(uint32_t cid) {
  if (cid == 1) return 0x0020;
  // Fullwidth ASCII block: CID 2..94 → U+FF01..U+FF5D, with CID 61 mapping to
  // U+FFE5 (fullwidth yen sign) per Adobe-Japan1. Easier to keep the table.
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
      0xFF5B, 0xFF5C, 0xFF5D};
  if (cid >= 2 && cid <= 94) return cid_2_94[cid - 2];
  if (cid >= 231 && cid <= 240) return 0xFF10 + (cid - 231);
  if (cid >= 241 && cid <= 266) return 0xFF21 + (cid - 241);
  if (cid >= 267 && cid <= 292) return 0xFF41 + (cid - 267);
  if (cid >= 842 && cid <= 924) return 0x3041 + (cid - 842);
  if (cid >= 925 && cid <= 1010) return 0x30A1 + (cid - 925);
  return 0;
}

// Adobe Symbol encoding (PDF 1.7 Annex D.5) mapped to Unicode.
// Unassigned slots use 0 so callers can fall through.
inline uint32_t lookup_adobe_symbol_encoding(uint32_t cc) {
  static const uint32_t t[256] = {
      /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      /* 0x10 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      /* 0x20 */ 0x0020,0x0021,0x2200,0x0023,0x2203,0x0025,0x0026,0x220D,
                 0x0028,0x0029,0x2217,0x002B,0x002C,0x2212,0x002E,0x002F,
      /* 0x30 */ 0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,
                 0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
      /* 0x40 */ 0x2245,0x0391,0x0392,0x03A7,0x0394,0x0395,0x03A6,0x0393,
                 0x0397,0x0399,0x03D1,0x039A,0x039B,0x039C,0x039D,0x039F,
      /* 0x50 */ 0x03A0,0x0398,0x03A1,0x03A3,0x03A4,0x03A5,0x03C2,0x03A9,
                 0x039E,0x03A8,0x0396,0x005B,0x2234,0x005D,0x22A5,0x005F,
      /* 0x60 */ 0xF8E5,0x03B1,0x03B2,0x03C7,0x03B4,0x03B5,0x03C6,0x03B3,
                 0x03B7,0x03B9,0x03D5,0x03BA,0x03BB,0x03BC,0x03BD,0x03BF,
      /* 0x70 */ 0x03C0,0x03B8,0x03C1,0x03C3,0x03C4,0x03C5,0x03D6,0x03C9,
                 0x03BE,0x03C8,0x03B6,0x007B,0x007C,0x007D,0x223C,0,
      /* 0x80 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      /* 0x90 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      /* 0xA0 */ 0x20AC,0x03D2,0x2032,0x2264,0x2044,0x221E,0x0192,0x2663,
                 0x2666,0x2665,0x2660,0x2194,0x2190,0x2191,0x2192,0x2193,
      /* 0xB0 */ 0x00B0,0x00B1,0x2033,0x2265,0x00D7,0x221D,0x2202,0x2022,
                 0x00F7,0x2260,0x2261,0x2248,0x2026,0xF8E6,0xF8E7,0x21B5,
      /* 0xC0 */ 0x2135,0x2111,0x211C,0x2118,0x2297,0x2295,0x2205,0x2229,
                 0x222A,0x2283,0x2287,0x2284,0x2282,0x2286,0x2208,0x2209,
      /* 0xD0 */ 0x2220,0x2207,0x00AE,0x00A9,0x2122,0x220F,0x221A,0x22C5,
                 0x00AC,0x2227,0x2228,0x21D4,0x21D0,0x21D1,0x21D2,0x21D3,
      /* 0xE0 */ 0x25CA,0x27E8,0x00AE,0x00A9,0x2122,0x2211,0xF8EB,0xF8EC,
                 0xF8ED,0xF8EE,0xF8EF,0xF8F0,0xF8F1,0xF8F2,0xF8F3,0xF8F4,
      /* 0xF0 */ 0,0x27E9,0x222B,0x2320,0xF8F5,0x2321,0xF8F6,0xF8F7,
                 0xF8F8,0xF8F9,0xF8FA,0xF8FB,0xF8FC,0xF8FD,0xF8FE,0
  };
  return cc < 256 ? t[cc] : 0;
}

// Adobe ZapfDingbats encoding to Unicode.
inline uint32_t lookup_zapf_dingbats_encoding(uint32_t cc) {
  static const uint32_t t[256] = {
      /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      /* 0x10 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      /* 0x20 */ 0x0020,0x2701,0x2702,0x2703,0x2704,0x260E,0x2706,0x2707,
                 0x2708,0x2709,0x261B,0x261E,0x270C,0x270D,0x270E,0x270F,
      /* 0x30 */ 0x2710,0x2711,0x2712,0x2713,0x2714,0x2715,0x2716,0x2717,
                 0x2718,0x2719,0x271A,0x271B,0x271C,0x271D,0x271E,0x271F,
      /* 0x40 */ 0x2720,0x2721,0x2722,0x2723,0x2724,0x2725,0x2726,0x2727,
                 0x2605,0x2729,0x272A,0x272B,0x272C,0x272D,0x272E,0x272F,
      /* 0x50 */ 0x2730,0x2731,0x2732,0x2733,0x2734,0x2735,0x2736,0x2737,
                 0x2738,0x2739,0x273A,0x273B,0x273C,0x273D,0x273E,0x273F,
      /* 0x60 */ 0x2740,0x2741,0x2742,0x2743,0x2744,0x2745,0x2746,0x2747,
                 0x2748,0x2749,0x274A,0x274B,0x25CF,0x274D,0x25A0,0x274F,
      /* 0x70 */ 0x2750,0x2751,0x2752,0x25B2,0x25BC,0x25C6,0x2756,0x25D7,
                 0x2758,0x2759,0x275A,0x275B,0x275C,0x275D,0x275E,0,
      /* 0x80 */ 0xF8D7,0xF8D8,0xF8D9,0xF8DA,0xF8DB,0xF8DC,0xF8DD,0xF8DE,
                 0xF8DF,0xF8E0,0xF8E1,0xF8E2,0xF8E3,0xF8E4,0,0,
      /* 0x90 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      /* 0xA0 */ 0,0x2761,0x2762,0x2763,0x2764,0x2765,0x2766,0x2767,
                 0x2663,0x2666,0x2665,0x2660,0x2460,0x2461,0x2462,0x2463,
      /* 0xB0 */ 0x2464,0x2465,0x2466,0x2467,0x2468,0x2469,0x2776,0x2777,
                 0x2778,0x2779,0x277A,0x277B,0x277C,0x277D,0x277E,0x277F,
      /* 0xC0 */ 0x2780,0x2781,0x2782,0x2783,0x2784,0x2785,0x2786,0x2787,
                 0x2788,0x2789,0x278A,0x278B,0x278C,0x278D,0x278E,0x278F,
      /* 0xD0 */ 0x2790,0x2791,0x2792,0x2793,0x2794,0x2192,0x2194,0x2195,
                 0x2798,0x2799,0x279A,0x279B,0x279C,0x279D,0x279E,0x279F,
      /* 0xE0 */ 0x27A0,0x27A1,0x27A2,0x27A3,0x27A4,0x27A5,0x27A6,0x27A7,
                 0x27A8,0x27A9,0x27AA,0x27AB,0x27AC,0x27AD,0x27AE,0x27AF,
      /* 0xF0 */ 0,0x27B1,0x27B2,0x27B3,0x27B4,0x27B5,0x27B6,0x27B7,
                 0x27B8,0x27B9,0x27BA,0x27BB,0x27BC,0x27BD,0x27BE,0
  };
  return cc < 256 ? t[cc] : 0;
}

template <typename GlyphNameToUnicodeFn, typename AdobeJapanCidToUnicodeFn>
inline uint32_t map_char_to_unicode_generic(
    uint32_t char_code, const BaseFont* font, const uint16_t* win_ansi,
    const uint16_t* mac_roman, GlyphNameToUnicodeFn glyph_name_to_unicode,
    AdobeJapanCidToUnicodeFn adobe_japan1_cid_to_unicode) {
  if (!font) return char_code;

  uint32_t unicode = 0;
  if (try_map_tounicode(char_code, font, &unicode)) {
    return unicode;
  }

  auto* type0_font = as_type0_font(font);
  if (type0_font && type0_font->ordering == "Japan1") {
    uint32_t mapped = adobe_japan1_cid_to_unicode(char_code);
    if (mapped != 0) return mapped;
  }

  auto diff_it = font->encoding_differences.find(char_code);
  if (diff_it != font->encoding_differences.end()) {
    uint32_t mapped = glyph_name_to_unicode(diff_it->second);
    if (mapped != 0) return mapped;
  }

  if (win_ansi && font->encoding == "WinAnsiEncoding" && char_code < 256) {
    return win_ansi[char_code];
  }

  if (mac_roman && font->encoding == "MacRomanEncoding" && char_code < 256) {
    return mac_roman[char_code];
  }

  // Standard Adobe Symbol / ZapfDingbats encodings. These fire only when the
  // font explicitly declares one of those encodings (or its base-font name
  // post-subset-prefix strip exactly matches "Symbol" / "ZapfDingbats"). A
  // substring match like "ABCDEF+CustomSymbol" is NOT enough — subsetted
  // Symbol-named fonts typically assign their own CIDs and need ToUnicode or
  // explicit /Encoding to decode. Applying Adobe's table blindly turns the
  // PDF author's bullet at CID 0x50 into U+03A0 (Pi), etc.
  if (char_code < 256) {
    auto strip_subset = [](std::string_view s) -> std::string_view {
      if (s.size() > 7 && s[6] == '+') {
        bool all_upper = true;
        for (int i = 0; i < 6; ++i) {
          char c = s[i];
          if (c < 'A' || c > 'Z') { all_upper = false; break; }
        }
        if (all_upper) return s.substr(7);
      }
      return s;
    };
    auto eq_ci = [](std::string_view a, std::string_view b) {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
      }
      return true;
    };
    std::string_view bf = strip_subset(font->base_font);
    const bool is_adobe_dingbats =
        eq_ci(bf, "ZapfDingbats") ||
        font->encoding == "ZapfDingbatsEncoding";
    if (is_adobe_dingbats) {
      uint32_t u = lookup_zapf_dingbats_encoding(char_code);
      if (u != 0) return u;
    }
    const bool is_adobe_symbol =
        !is_adobe_dingbats &&
        (eq_ci(bf, "Symbol") || font->encoding == "SymbolEncoding");
    if (is_adobe_symbol) {
      uint32_t u = lookup_adobe_symbol_encoding(char_code);
      if (u != 0) return u;
    }
  }

  return char_code;
}

}  // namespace nanopdf
