// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Type1 font program parser implementation
// Includes eexec decryption, CharString extraction, and Type1 CharString
// interpreter producing ttf_outline_t for rendering.

#include "type1-parser.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace nanopdf {

namespace {

// ===========================================================================
// Adobe Type 1 encryption / decryption
// ===========================================================================

// Standard Type 1 eexec key.
static const uint16_t kEexecKey = 55665;
static const uint16_t kCharStringKey = 4330;
static constexpr int kCharStringStackLimit = 48;

// Decrypt (or encrypt — same operation) len bytes in-place using the
// Adobe Type 1 standard cipher (keyed XOR with feedback).
//   plain[i] = cipher[i] ^ (key >> 8)
//   key = (key + cipher[i]) * 52845 + 22719  (mod 65536)
void t1_crypt_inplace(uint8_t* buf, uint32_t len, uint16_t key) {
  for (uint32_t i = 0; i < len; i++) {
    uint8_t cipher = buf[i];
    uint8_t plain = static_cast<uint8_t>(cipher ^ static_cast<uint8_t>(key >> 8));
    key = static_cast<uint16_t>((static_cast<uint32_t>(key) + cipher) * 52845 + 22719);
    buf[i] = plain;
  }
}

// ===========================================================================
// CharString raw-data helper: extract the decrypted binary after "RD" / "-|"
// from a fully-decrypted eexec buffer.  The caller provides the cursor
// positioned at the first byte of the binary payload.
// ===========================================================================
static bool extract_rd_binary(
    const char* src, size_t src_len, int count, int lenIV,
    std::vector<uint8_t>& out) {
  if (count <= 0) return false;
  if (static_cast<size_t>(count) > src_len) return false;

  std::vector<uint8_t> decrypted(reinterpret_cast<const uint8_t*>(src),
                                 reinterpret_cast<const uint8_t*>(src) + count);
  t1_crypt_inplace(decrypted.data(), static_cast<uint32_t>(decrypted.size()),
                   kCharStringKey);

  int skip = (std::min)(lenIV, count);
  int actual = count - skip;
  if (actual <= 0) {
    out.clear();
    return true;  // empty glyph (e.g. space with no outlines)
  }
  out.assign(decrypted.begin() + skip, decrypted.end());
  return true;
}

// -----------------------------------------------------------------------
// Minimal PostScript tokenizer over the decrypted eexec buffer.
// Returns one of:
//   ""         on end-of-buffer
//   "/name"    a PostScript literal name (leading slash included)
//   "(...)"    a string literal
//   "<...>"    a hex string or dictionary (<< ... >>)
//   "[", "]", "{", "}"  grouping tokens
//   otherwise  a plain token (number, operator keyword)
//
// When count > 0 and the next non-whitespace text is "RD" or "-|", the
// tokenizer treats it as a special binary-read marker.  The caller must
// then consume count raw bytes before continuing.
// -----------------------------------------------------------------------
static std::string ps_token(const char*& p, const char* end) {
  // Skip whitespace and comments
  while (p < end) {
    if (*p == '%') {
      ++p;
      while (p < end && *p != '\n' && *p != '\r') ++p;
      continue;
    }
    if (*p <= ' ' || *p == '\f' || *p == '\v') { ++p; continue; }
    break;
  }
  if (p >= end) return {};

  if (*p == '(') {
    // String literal
    const char* start = p;
    int depth = 1;
    ++p;
    while (p < end && depth > 0) {
      if (*p == '\\' && p + 1 < end) { p += 2; continue; }
      if (*p == '(') { ++depth; }
      else if (*p == ')') { --depth; }
      ++p;
    }
    return std::string(start, p - start);
  }

  if (*p == '<') {
    if (p + 1 < end && p[1] == '<') {
      p += 2;
      return "<<";
    }
    // Hex string
    ++p;
    while (p < end && *p != '>') ++p;
    if (p < end) ++p;
    return "hex";
  }

  if (*p == '/' || *p == '[' || *p == ']' || *p == '{' || *p == '}') {
    if (*p == '/') {
      const char* start = p;
      ++p;
      while (p < end && *p > ' ' && *p != '/' && *p != '(' && *p != ')' &&
             *p != '<' && *p != '>' && *p != '[' && *p != ']' &&
             *p != '{' && *p != '}' && *p != '%')
        ++p;
      return std::string(start, p - start);
    }
    return std::string(1, *p++);
  }

  // Regular token
  const char* start = p;
  while (p < end && *p > ' ' && *p != '/' && *p != '(' && *p != ')' &&
         *p != '<' && *p != '>' && *p != '[' && *p != ']' &&
         *p != '{' && *p != '}' && *p != '%')
    ++p;
  return std::string(start, p - start);
}

// ===========================================================================
// Type1 CharString interpreter
// ===========================================================================

struct T1Interp {
  // Outline builder (same layout as ttf_parse.c's ob_*)
  ttf_point_t* pts{nullptr};
  int num_pts{0}, cap_pts{0};
  int* ends{nullptr};
  int num_contours{0}, cap_contours{0};

  float stack[kCharStringStackLimit];
  int   sp{0};
  float x{0}, y{0};
  int   started{0};       // first moveto issued?
  int   width_set{0};     // width consumed?
  int   num_hints{0};
  int   seac_origin_x{0}, seac_origin_y{0};  // for seac accent
  float other_results[24];
  int other_sp{0};
  int flex_active{0};
  ttf_point_t flex_pts[7];
  int flex_count{0};

  // Subrs
  const std::vector<std::vector<uint8_t>>* subrs{nullptr};
};

static int t1_ob_add_point(T1Interp* ti, float x, float y, int on_curve) {
  if (ti->num_pts >= ti->cap_pts) {
    int nc = ti->cap_pts ? ti->cap_pts * 2 : 64;
    auto* np = static_cast<ttf_point_t*>(
        std::realloc(ti->pts, static_cast<size_t>(nc) * sizeof(ttf_point_t)));
    if (!np) return -1;
    ti->pts = np;
    ti->cap_pts = nc;
  }
  ti->pts[ti->num_pts].x = x;
  ti->pts[ti->num_pts].y = y;
  ti->pts[ti->num_pts].on_curve = on_curve;
  ti->num_pts++;
  return 0;
}

static int t1_ob_end_contour(T1Interp* ti) {
  if (ti->num_pts == 0) return 0;
  if (ti->num_contours >= ti->cap_contours) {
    int nc = ti->cap_contours ? ti->cap_contours * 2 : 16;
    auto* ne = static_cast<int*>(
        std::realloc(ti->ends, static_cast<size_t>(nc) * sizeof(int)));
    if (!ne) return -1;
    ti->ends = ne;
    ti->cap_contours = nc;
  }
  ti->ends[ti->num_contours++] = ti->num_pts - 1;
  return 0;
}

static void t1_close_contour(T1Interp* ti) {
  if (ti->started && ti->num_pts > 0) t1_ob_end_contour(ti);
  ti->started = 0;
}

// Type1 CharString interpreter (recursive to support callsubr).
// cs/len is the DECRYPTED charstring program (lenIV bytes already stripped).
static int t1_run_charstring(T1Interp* ti, const uint8_t* cs, uint32_t len,
                              int depth);

static int t1_run_charstring(T1Interp* ti, const uint8_t* cs, uint32_t len,
                              int depth) {
  if (depth > 10) return -1;
  const uint8_t* end = cs + len;

  while (cs < end) {
    uint8_t b0 = *cs++;

    // ---- operand encoding (same as CFF Type 2) ----
    if (b0 >= 32) {
      float val;
      if (b0 <= 246) {
        val = static_cast<float>(static_cast<int>(b0) - 139);
      } else if (b0 <= 250) {
        if (cs >= end) return -1;
        val = static_cast<float>(
            (static_cast<int>(b0) - 247) * 256 + *cs++ + 108);
      } else if (b0 <= 254) {
        if (cs >= end) return -1;
        val = static_cast<float>(
            -(static_cast<int>(b0) - 251) * 256 - *cs++ - 108);
      } else {  // b0 == 255: Type1 32-bit two's-complement integer.
        // NOTE: Type1 charstrings encode this as a plain 32-bit integer, unlike
        // Type2/CFF where 255 is a 16.16 fixed-point value. Dividing by 65536
        // here would turn large operands (e.g. subr indices like 3566) into ~0.
        if (cs + 3 >= end) return -1;
        int32_t v = static_cast<int32_t>((static_cast<uint32_t>(cs[0]) << 24) |
                                         (static_cast<uint32_t>(cs[1]) << 16) |
                                         (static_cast<uint32_t>(cs[2]) << 8) |
                                         static_cast<uint32_t>(cs[3]));
        cs += 4;
        val = static_cast<float>(v);
      }
      if (ti->sp < kCharStringStackLimit) ti->stack[ti->sp++] = val;
      continue;
    }

    if (b0 == 28) {  // 16-bit signed integer
      if (cs + 1 >= end) return -1;
      int16_t v = static_cast<int16_t>((cs[0] << 8) | cs[1]);
      cs += 2;
      if (ti->sp < kCharStringStackLimit) ti->stack[ti->sp++] = static_cast<float>(v);
      continue;
    }

    // ---- Type1 operators ----
    switch (b0) {
    case 1:   // hstem
    case 3: { // vstem
      ti->num_hints += ti->sp / 2;
      if (!ti->width_set && (ti->sp & 1)) ti->width_set = 1;
      ti->sp = 0;
      break;
    }

    case 4: { // vmoveto
      if (!ti->width_set && ti->sp > 1) ti->width_set = 1;
      float dy = (ti->sp > 0) ? ti->stack[ti->sp - 1] : 0;
      ti->y += dy;
      if (!ti->flex_active) {
        t1_close_contour(ti);
        t1_ob_add_point(ti, ti->x, ti->y, 1);
        ti->started = 1;
      }
      ti->sp = 0;
      break;
    }

    case 5: { // rlineto
      for (int i = 0; i + 1 < ti->sp; i += 2) {
        ti->x += ti->stack[i];
        ti->y += ti->stack[i + 1];
        t1_ob_add_point(ti, ti->x, ti->y, 1);
      }
      ti->sp = 0;
      break;
    }

    case 6: { // hlineto
      int horiz = 1;
      for (int i = 0; i < ti->sp; i++) {
        if (horiz) ti->x += ti->stack[i];
        else       ti->y += ti->stack[i];
        t1_ob_add_point(ti, ti->x, ti->y, 1);
        horiz = !horiz;
      }
      ti->sp = 0;
      break;
    }

    case 7: { // vlineto
      int horiz = 0;
      for (int i = 0; i < ti->sp; i++) {
        if (horiz) ti->x += ti->stack[i];
        else       ti->y += ti->stack[i];
        t1_ob_add_point(ti, ti->x, ti->y, 1);
        horiz = !horiz;
      }
      ti->sp = 0;
      break;
    }

    case 8: { // rrcurveto
      for (int i = 0; i + 5 < ti->sp; i += 6) {
        float x1 = ti->x + ti->stack[i];
        float y1 = ti->y + ti->stack[i + 1];
        float x2 = x1    + ti->stack[i + 2];
        float y2 = y1    + ti->stack[i + 3];
        ti->x   = x2    + ti->stack[i + 4];
        ti->y   = y2    + ti->stack[i + 5];
        t1_ob_add_point(ti, x1, y1, 2);
        t1_ob_add_point(ti, x2, y2, 2);
        t1_ob_add_point(ti, ti->x, ti->y, 1);
      }
      ti->sp = 0;
      break;
    }

    case 9: { // closepath
      t1_close_contour(ti);
      ti->sp = 0;
      break;
    }

    case 10: { // callsubr
      if (ti->sp < 1) break;
      int idx = static_cast<int>(ti->stack[--ti->sp]);
      if (ti->subrs && idx >= 0 &&
          idx < static_cast<int>(ti->subrs->size())) {
        const auto& subr = (*ti->subrs)[static_cast<size_t>(idx)];
        if (!subr.empty())
          t1_run_charstring(ti, subr.data(),
                            static_cast<uint32_t>(subr.size()), depth + 1);
      }
      break;
    }

    case 11: // return
      return 0;

    case 12: { // escape (two-byte operator)
      if (cs >= end) return -1;
      uint8_t b1 = *cs++;
      switch (b1) {
      case 0: // dotsection
        ti->sp = 0;
        break;

      case 1: // vstem3
      case 2: // hstem3
        ti->num_hints += ti->sp / 2;
        if (!ti->width_set && (ti->sp & 1)) ti->width_set = 1;
        ti->sp = 0;
        break;

      case 6: { // seac (standard encoding accent composite)
        // Stack: asb adh ady bchar achar
        if (ti->sp < 5) break;
        int achar = static_cast<int>(ti->stack[ti->sp - 1]);
        int bchar = static_cast<int>(ti->stack[ti->sp - 2]);
        float ady = ti->stack[ti->sp - 3];
        float adh = ti->stack[ti->sp - 4];
        float asb = ti->stack[ti->sp - 5];
        // For now, render only the base character (ignore accents).
        // SEAC composites are rare; this avoids crashing.
        (void)achar; (void)bchar; (void)ady; (void)adh; (void)asb;
        ti->sp = 0;
        break;
      }

      case 7: { // sbw: sidebearing x/y and width x/y
        if (ti->sp >= 4) {
          // Stack: sbx sby wx wy, possibly after hint values. The last four
          // operands carry the metrics; TeX extensible symbols use sby for
          // vertical delimiter placement.
          ti->x = ti->stack[ti->sp - 4];
          ti->y = ti->stack[ti->sp - 3];
          ti->width_set = 1;
        }
        ti->sp = 0;
        break;
      }

      case 16: { // callothersubr
        // OtherSubrs: predefined subroutines (flex, hint replacement).
        // Stack: arg0 ... argN n_args other_subr_number callothersubr.
        // Some fonts use OtherSubr 3 as a hint-replacement trampoline:
        //   <subr-index> 1 3 callothersubr pop callsubr
        // The following pop must restore <subr-index>; clearing the stack here
        // drops whole outline subroutines from embedded Type1 text fonts.
        if (ti->sp < 2) {
          ti->sp = 0;
          break;
        }
        int os_num = static_cast<int>(ti->stack[ti->sp - 1]);
        int n_args = static_cast<int>(ti->stack[ti->sp - 2]);
        if (n_args < 0) n_args = 0;
        int arg_start = ti->sp - 2 - n_args;
        if (arg_start < 0) {
          ti->sp = 0;
          ti->other_sp = 0;
          break;
        }
        ti->sp = arg_start;
        ti->other_sp = 0;
        if (os_num == 1) {
          ti->flex_active = 1;
          ti->flex_count = 1;
          ti->flex_pts[0].x = ti->x;
          ti->flex_pts[0].y = ti->y;
          ti->flex_pts[0].on_curve = 1;
        } else if (os_num == 2) {
          if (ti->flex_active && ti->flex_count < 7) {
            ti->flex_pts[ti->flex_count].x = ti->x;
            ti->flex_pts[ti->flex_count].y = ti->y;
            ti->flex_pts[ti->flex_count].on_curve =
                (ti->flex_count == 3 || ti->flex_count == 6) ? 1 : 2;
            ti->flex_count++;
          }
        } else if (os_num == 3) {
          for (int i = 0; i < n_args && ti->other_sp < 24; ++i) {
            ti->other_results[ti->other_sp++] = ti->stack[arg_start + i];
          }
        } else if (os_num == 0) {
          if (ti->flex_active && ti->flex_count >= 7) {
            if (!ti->started) {
              t1_ob_add_point(ti, ti->flex_pts[0].x, ti->flex_pts[0].y, 1);
              ti->started = 1;
            }
            for (int i = 1; i < 7; ++i) {
              t1_ob_add_point(ti, ti->flex_pts[i].x, ti->flex_pts[i].y,
                              ti->flex_pts[i].on_curve);
            }
          }
          ti->flex_active = 0;
          ti->flex_count = 0;
          // Flex end returns the current point for "pop pop setcurrentpoint".
          if (ti->other_sp < 24) ti->other_results[ti->other_sp++] = ti->y;
          if (ti->other_sp < 24) ti->other_results[ti->other_sp++] = ti->x;
        }
        break;
      }

      case 17: // pop
        if (ti->other_sp > 0 && ti->sp < kCharStringStackLimit) {
          ti->stack[ti->sp++] = ti->other_results[--ti->other_sp];
        }
        break;

      case 33: // setcurrentpoint
        // Stack: x y
        if (ti->sp >= 2) {
          ti->x = ti->stack[ti->sp - 2];
          ti->y = ti->stack[ti->sp - 1];
        }
        ti->sp = 0;
        break;

      case 12: // div
        if (ti->sp >= 2) {
          float denom = ti->stack[ti->sp - 1];
          float numer = ti->stack[ti->sp - 2];
          ti->sp -= 2;
          ti->stack[ti->sp++] = (denom != 0.0f) ? numer / denom : 0.0f;
        }
        break;

      default:
        ti->sp = 0;
        break;
      }
      break;
    }

    case 13: { // hsbw: horizontal sidebearing and width
      if (ti->sp >= 2) {
        ti->x = ti->stack[ti->sp - 2];
        ti->y = 0.0f;
        ti->width_set = 1;
      }
      ti->sp = 0;
      break;
    }

    case 14: // endchar
      t1_close_contour(ti);
      ti->sp = 0;
      return 0;

    case 21: { // rmoveto
      if (!ti->width_set && ti->sp > 2) ti->width_set = 1;
      float dx = (ti->sp >= 2) ? ti->stack[ti->sp - 2] : 0.0f;
      float dy = (ti->sp >= 1) ? ti->stack[ti->sp - 1] : 0.0f;
      ti->x += dx;
      ti->y += dy;
      if (!ti->flex_active) {
        t1_close_contour(ti);
        t1_ob_add_point(ti, ti->x, ti->y, 1);
        ti->started = 1;
      }
      ti->sp = 0;
      break;
    }

    case 22: { // hmoveto
      if (!ti->width_set && ti->sp > 1) ti->width_set = 1;
      float dx = (ti->sp >= 1) ? ti->stack[ti->sp - 1] : 0.0f;
      ti->x += dx;
      if (!ti->flex_active) {
        t1_close_contour(ti);
        t1_ob_add_point(ti, ti->x, ti->y, 1);
        ti->started = 1;
      }
      ti->sp = 0;
      break;
    }

    case 30: { // vhcurveto
      int i = 0;
      while (i + 3 < ti->sp) {
        float x1 = ti->x;
        float y1 = ti->y + ti->stack[i];
        float x2 = x1 + ti->stack[i + 1];
        float y2 = y1 + ti->stack[i + 2];
        ti->x = x2 + ti->stack[i + 3];
        ti->y = y2;
        t1_ob_add_point(ti, x1, y1, 2);
        t1_ob_add_point(ti, x2, y2, 2);
        t1_ob_add_point(ti, ti->x, ti->y, 1);
        i += 4;
      }
      ti->sp = 0;
      break;
    }

    case 31: { // hvcurveto
      int i = 0;
      while (i + 3 < ti->sp) {
        float x1 = ti->x + ti->stack[i];
        float y1 = ti->y;
        float x2 = x1 + ti->stack[i + 1];
        float y2 = y1 + ti->stack[i + 2];
        ti->x = x2;
        ti->y = y2 + ti->stack[i + 3];
        t1_ob_add_point(ti, x1, y1, 2);
        t1_ob_add_point(ti, x2, y2, 2);
        t1_ob_add_point(ti, ti->x, ti->y, 1);
        i += 4;
      }
      ti->sp = 0;
      break;
    }

    default:
      // Unknown operator: clear stack
      ti->sp = 0;
      break;
    }
  }
  return 0;
}

// Standard encoding (Adobe Type 1 standard)
const char* kStandardEncoding[256] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent",
    "ampersand", "quoteright", "parenleft", "parenright", "asterisk", "plus",
    "comma", "hyphen", "period", "slash", "zero", "one", "two", "three",
    "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon",
    "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E",
    "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash",
    "bracketright", "asciicircum", "underscore", "quoteleft", "a", "b", "c",
    "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q",
    "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar",
    "braceright", "asciitilde", nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, "exclamdown", "cent", "sterling",
    "fraction", "yen", "florin", "section", "currency", "quotesingle",
    "quotedblleft", "guillemotleft", "guilsinglleft", "guilsinglright", "fi",
    "fl", nullptr, "endash", "dagger", "daggerdbl", "periodcentered", nullptr,
    "paragraph", "bullet", "quotesinglbase", "quotedblbase", "quotedblright",
    "guillemotright", "ellipsis", "perthousand", nullptr, "questiondown",
    nullptr, "grave", "acute", "circumflex", "tilde", "macron", "breve",
    "dotaccent", "dieresis", nullptr, "ring", "cedilla", nullptr,
    "hungarumlaut", "ogonek", "caron", "emdash", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, "AE", nullptr,
    "ordfeminine", nullptr, nullptr, nullptr, nullptr, "Lslash", "Oslash",
    "OE", "ordmasculine", nullptr, nullptr, nullptr, nullptr, nullptr, "ae",
    nullptr, nullptr, nullptr, "dotlessi", nullptr, nullptr, "lslash",
    "oslash", "oe", "germandbls", nullptr, nullptr, nullptr, nullptr
};

bool IsWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool IsDelimiter(char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
         c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

}  // namespace

Type1Parser::Type1Parser() = default;
Type1Parser::~Type1Parser() = default;

bool Type1Parser::Parse(const uint8_t* data, size_t size, Type1FontData& result,
                        bool parse_charstrings) {
  if (!data || size == 0) {
    error_ = "No data provided";
    return false;
  }

  // Try to decode PFB format
  std::vector<uint8_t> decoded;
  const char* fontData;
  size_t fontSize;

  if (UndoPFB(data, size, decoded)) {
    fontData = reinterpret_cast<const char*>(decoded.data());
    fontSize = decoded.size();
  } else {
    // Assume PFA (ASCII) format
    fontData = reinterpret_cast<const char*>(data);
    fontSize = size;
  }

  // Parse the font dictionary
  if (!ParseFontDict(fontData, fontSize, result)) {
    return false;
  }

  // Parse encoding
  if (!ParseEncoding(fontData, fontSize, result)) {
    // Non-fatal - use standard encoding as fallback
    result.uses_standard_encoding = true;
    for (int i = 0; i < 256; ++i) {
      if (kStandardEncoding[i]) {
        result.encoding[i] = kStandardEncoding[i];
      }
    }
  }

  // Parse FontInfo (optional)
  ParseFontInfo(fontData, fontSize, result);

  // Parse Private dictionary (optional)
  ParsePrivate(fontData, fontSize, result);

  // Parse CharStrings if requested (optional)
  if (parse_charstrings) {
    ParseCharStrings(fontData, fontSize, result);
  }

  return true;
}

bool Type1Parser::UndoPFB(const uint8_t* data, size_t size,
                          std::vector<uint8_t>& output) {
  if (size < 6 || data[0] != 0x80) {
    return false;  // Not PFB format
  }

  output.clear();
  output.reserve(size);

  size_t pos = 0;
  while (pos + 6 <= size && data[pos] == 0x80) {
    uint8_t type = data[pos + 1];
    if (type < 1 || type > 2) {
      break;  // End marker (type 3) or invalid
    }

    // Read segment length (little-endian 32-bit)
    uint32_t segLen = static_cast<uint32_t>(data[pos + 2]) |
                      (static_cast<uint32_t>(data[pos + 3]) << 8) |
                      (static_cast<uint32_t>(data[pos + 4]) << 16) |
                      (static_cast<uint32_t>(data[pos + 5]) << 24);

    pos += 6;

    if (pos + segLen > size) {
      break;  // Truncated data
    }

    // Append segment data
    output.insert(output.end(), data + pos, data + pos + segLen);
    pos += segLen;
  }

  return !output.empty();
}

bool Type1Parser::ParseFontDict(const char* data, size_t size,
                                Type1FontData& result) {
  const char* p = data;
  const char* end = data + size;

  // Find /FontName
  const char* fontNamePos = FindName(data, size, "/FontName");
  if (fontNamePos) {
    p = fontNamePos;
    p = SkipWhitespaceAndComments(p, end);
    if (p < end && *p == '/') {
      std::string name;
      if (ParseName(p, end, name)) {
        result.font_name = name;
      }
    }
  }

  // Find /FontBBox
  const char* bboxPos = FindName(data, size, "/FontBBox");
  if (bboxPos) {
    p = bboxPos;
    p = SkipWhitespaceAndComments(p, end);

    // Skip opening bracket or brace
    if (p < end && (*p == '[' || *p == '{')) {
      ++p;
    }

    std::vector<double> bbox;
    if (ParseNumberArray(p, end, bbox) && bbox.size() >= 4) {
      result.font_bbox[0] = bbox[0];
      result.font_bbox[1] = bbox[1];
      result.font_bbox[2] = bbox[2];
      result.font_bbox[3] = bbox[3];
    }
  }

  // Find /FontMatrix
  const char* matrixPos = FindName(data, size, "/FontMatrix");
  if (matrixPos) {
    p = matrixPos;
    p = SkipWhitespaceAndComments(p, end);

    if (p < end && (*p == '[' || *p == '{')) {
      ++p;
    }

    std::vector<double> matrix;
    if (ParseNumberArray(p, end, matrix) && matrix.size() >= 6) {
      for (int i = 0; i < 6; ++i) {
        result.font_matrix[i] = matrix[i];
      }
    }
  }

  return !result.font_name.empty();
}

bool Type1Parser::ParseEncoding(const char* data, size_t size,
                                Type1FontData& result) {
  const char* encPos = FindName(data, size, "/Encoding");
  if (!encPos) {
    return false;
  }

  const char* p = encPos;
  const char* end = data + size;
  p = SkipWhitespaceAndComments(p, end);

  // Check for "StandardEncoding def"
  std::string token = ParseToken(p, end);
  if (token == "StandardEncoding") {
    p = SkipWhitespaceAndComments(p, end);
    token = ParseToken(p, end);
    if (token == "def") {
      result.uses_standard_encoding = true;
      for (int i = 0; i < 256; ++i) {
        if (kStandardEncoding[i]) {
          result.encoding[i] = kStandardEncoding[i];
        }
      }
      return true;
    }
  }

  // Check for "256 array" encoding definition
  // Rewind and try again
  p = encPos;
  p = SkipWhitespaceAndComments(p, end);

  int arraySize = 0;
  if (ParseInteger(p, end, arraySize) && arraySize == 256) {
    p = SkipWhitespaceAndComments(p, end);
    token = ParseToken(p, end);
    if (token == "array") {
      // Parse encoding entries
      // Format: dup <code> /<glyphname> put
      while (p < end) {
        p = SkipWhitespaceAndComments(p, end);
        token = ParseToken(p, end);

        if (token == "def" || token == "readonly") {
          break;  // End of encoding
        }

        if (token == "dup") {
          p = SkipWhitespaceAndComments(p, end);

          // Parse code (may be decimal or octal with 8#)
          int code = -1;
          const char* codeStart = p;

          // Check for octal prefix "8#"
          if (p + 2 < end && p[0] == '8' && p[1] == '#') {
            p += 2;
            // Parse octal number
            int octalValue = 0;
            while (p < end && *p >= '0' && *p <= '7') {
              octalValue = octalValue * 8 + (*p - '0');
              ++p;
            }
            code = octalValue;
          } else {
            ParseInteger(p, end, code);
          }

          if (code < 0 || code > 255) {
            continue;
          }

          p = SkipWhitespaceAndComments(p, end);

          // Check for name directly attached to code (e.g., "33/exclam")
          if (p < end && *p == '/') {
            std::string glyphName;
            if (ParseName(p, end, glyphName)) {
              result.encoding[code] = glyphName;
            }
          } else {
            // Try parsing as separate token
            std::string glyphName;
            if (ParseName(p, end, glyphName)) {
              result.encoding[code] = glyphName;
            }
          }

          // Skip "put"
          p = SkipWhitespaceAndComments(p, end);
          token = ParseToken(p, end);
        }
      }
      return true;
    }
  }

  return false;
}

bool Type1Parser::ParseFontInfo(const char* data, size_t size,
                                Type1FontData& result) {
  // Find /FontInfo begin ... end
  const char* infoPos = FindName(data, size, "/FontInfo");
  if (!infoPos) {
    return false;
  }

  // Find the "begin" after /FontInfo
  const char* p = infoPos;
  const char* end = data + size;

  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);
    std::string token = ParseToken(p, end);
    if (token == "begin") {
      break;
    }
    if (token.empty()) {
      return false;
    }
  }

  // Parse FontInfo entries until "end"
  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);
    std::string token = ParseToken(p, end);

    if (token == "end") {
      break;
    }

    if (token.empty()) {
      break;
    }

    // Check for known entries
    if (token[0] == '/') {
      std::string key = token.substr(1);
      p = SkipWhitespaceAndComments(p, end);

      if (key == "FullName" || key == "FamilyName" || key == "Weight") {
        std::string value;
        if (*p == '(') {
          ParseString(p, end, value);
        }
        if (key == "FullName") result.full_name = value;
        else if (key == "FamilyName") result.family_name = value;
        else if (key == "Weight") result.weight = value;
      } else if (key == "ItalicAngle") {
        ParseReal(p, end, result.italic_angle);
      } else if (key == "isFixedPitch") {
        std::string boolVal = ParseToken(p, end);
        result.is_fixed_pitch = (boolVal == "true");
      } else if (key == "UnderlinePosition") {
        ParseReal(p, end, result.underline_position);
      } else if (key == "UnderlineThickness") {
        ParseReal(p, end, result.underline_thickness);
      }
    }
  }

  return true;
}

bool Type1Parser::ParsePrivate(const char* data, size_t size,
                               Type1FontData& result) {
  // Find /Private
  const char* privPos = FindName(data, size, "/Private");
  if (!privPos) {
    return false;
  }

  const char* p = privPos;
  const char* end = data + size;

  // Skip to "begin"
  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);
    std::string token = ParseToken(p, end);
    if (token == "begin") {
      break;
    }
    if (token.empty()) {
      return false;
    }
  }

  // Parse Private dictionary entries
  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);
    std::string token = ParseToken(p, end);

    if (token == "end" || token == "currentdict" || token == "dup") {
      // End of Private or start of CharStrings
      break;
    }

    if (token.empty()) {
      break;
    }

    if (token[0] == '/') {
      std::string key = token.substr(1);
      p = SkipWhitespaceAndComments(p, end);

      if (key == "BlueValues" || key == "OtherBlues" ||
          key == "FamilyBlues" || key == "FamilyOtherBlues") {
        if (p < end && (*p == '[' || *p == '{')) {
          ++p;
        }
        std::vector<int> values;
        ParseIntArray(p, end, values);
        if (key == "BlueValues") result.blue_values = values;
        else if (key == "OtherBlues") result.other_blues = values;
        else if (key == "FamilyBlues") result.family_blues = values;
        else if (key == "FamilyOtherBlues") result.family_other_blues = values;
      } else if (key == "BlueFuzz") {
        ParseInteger(p, end, result.blue_fuzz);
      } else if (key == "BlueScale") {
        ParseReal(p, end, result.blue_scale);
      } else if (key == "BlueShift") {
        ParseInteger(p, end, result.blue_shift);
      } else if (key == "StdHW") {
        if (p < end && (*p == '[' || *p == '{')) {
          ++p;
        }
        ParseInteger(p, end, result.std_hw);
      } else if (key == "StdVW") {
        if (p < end && (*p == '[' || *p == '{')) {
          ++p;
        }
        ParseInteger(p, end, result.std_vw);
      }
    }
  }

  return true;
}

bool Type1Parser::ParseCharStrings(const char* data, size_t size,
                                    Type1FontData& result) {
  // Find the eexec section and decrypt it.
  const char* eexec_pos = FindName(data, size, "eexec");
  if (!eexec_pos) {
    return false;
  }

  // FindName returns just after the eexec token. Skip only whitespace before
  // the encrypted payload; skipping another full line drops payload bytes,
  // especially for PFB binary eexec sections.
  const char* p = eexec_pos;
  const char* end = data + size;
  auto is_ps_space = [](char ch) {
    unsigned char c = static_cast<unsigned char>(ch);
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\0';
  };
  while (p < end && is_ps_space(*p)) p++;

  if (p >= end) return false;

  // Determine if the eexec data is hex-encoded (PFA) or binary (PFB).
  // In PFA format the data is hex-encoded; in PFB it's raw binary.
  const char* enc_start = p;
  size_t enc_len = static_cast<size_t>(end - p);
  bool is_hex = true;
  size_t hex_digits = 0;
  for (size_t i = 0; i < std::min(enc_len, size_t(256)); i++) {
    unsigned char c = static_cast<unsigned char>(enc_start[i]);
    if (std::isspace(c)) continue;
    if (std::isxdigit(c)) {
      hex_digits++;
      continue;
    }
    is_hex = false;
    break;
  }
  if (hex_digits < 16) is_hex = false;

  std::vector<uint8_t> binary;
  if (is_hex) {
    // Hex-decode
    binary.reserve(enc_len / 2);
    const char* hp = enc_start;
    while (hp < end) {
      while (hp < end && is_ps_space(*hp)) hp++;
      if (hp >= end) break;
      char hi = *hp++;
      while (hp < end && is_ps_space(*hp)) hp++;
      if (hp >= end) break;
      char lo = *hp++;
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      int h = hex(hi), l = hex(lo);
      if (h < 0 || l < 0) break;
      binary.push_back(static_cast<uint8_t>((h << 4) | l));
    }
  } else {
    binary.assign(reinterpret_cast<const uint8_t*>(enc_start),
                  reinterpret_cast<const uint8_t*>(end));
  }

  if (binary.empty()) return false;

  // Decrypt with key 55665 (Adobe Type 1 eexec key)
  t1_crypt_inplace(binary.data(), static_cast<uint32_t>(binary.size()), kEexecKey);

  // eexec has its own four random synchronization bytes. This is independent
  // from CharString lenIV, which is parsed from the decrypted Private dict.
  int skip = 4;
  if (static_cast<int>(binary.size()) > skip) {
    binary.erase(binary.begin(), binary.begin() + skip);
  }

  // Parse the decrypted eexec section as PostScript.
  // Look for /CharStrings and /Subrs, then extract binary data
  // after "RD" (or "-|") operators.
  const char* dec = reinterpret_cast<const char*>(binary.data());
  const char* dec_end = dec + binary.size();
  const char* dp = dec;

  if (const char* len_iv_pos =
          FindName(dec, static_cast<size_t>(dec_end - dec), "/lenIV")) {
    const char* lp = SkipWhitespaceAndComments(len_iv_pos, dec_end);
    int parsed_len_iv = result.lenIV;
    if (ParseInteger(lp, dec_end, parsed_len_iv)) {
      result.lenIV = (std::max)(0, parsed_len_iv);
    }
  }

  bool in_charstrings = false;
  bool in_subrs = false;
  std::string pending_name;
  int pending_count = -1;
  int pending_subr_idx = -1;
  enum { kNormal, kWantRD } state = kNormal;
  int paren_depth = 0;

  while (dp < dec_end) {
    // Skip whitespace
    while (dp < dec_end && is_ps_space(*dp)) dp++;
    if (dp >= dec_end) break;

    // If we're in a parenthesized string, skip it
    if (*dp == '(') {
      paren_depth++;
      dp++;
      while (dp < dec_end && paren_depth > 0) {
        if (*dp == '\\' && dp + 1 < dec_end) { dp += 2; continue; }
        if (*dp == '(') paren_depth++;
        else if (*dp == ')') paren_depth--;
        dp++;
      }
      continue;
    }

    // Read token
    const char* tok_start = dp;
    std::string token;

    if (*dp == '/') {
      // Name literal
      dp++;
      while (dp < dec_end && *dp > ' ' && *dp != '/' && *dp != '(' &&
             *dp != ')' && *dp != '<' && *dp != '>' && *dp != '[' &&
             *dp != ']' && *dp != '{' && *dp != '}')
        dp++;
      token = std::string(tok_start, dp - tok_start);
    } else if (*dp == '<' || *dp == '>' || *dp == '[' || *dp == ']' ||
               *dp == '{' || *dp == '}') {
      token = *dp++;
      if (!token.empty() && token[0] == '<' && dp < dec_end && *dp == '<') {
        token = "<<"; dp++;
      } else if (!token.empty() && token[0] == '>' && dp < dec_end && *dp == '>') {
        token = ">>"; dp++;
      }
    } else {
      while (dp < dec_end && *dp > ' ' && *dp != '/' && *dp != '(' &&
             *dp != ')' && *dp != '<' && *dp != '>' && *dp != '[' &&
             *dp != ']' && *dp != '{' && *dp != '}')
        dp++;
      token = std::string(tok_start, dp - tok_start);
    }

    if (token.empty()) { dp++; continue; }

    // Handle state: expecting RD after 'count' (and optional subr index)
    if (state == kWantRD && pending_count >= 0) {
      if (token == "RD" || token == "-|") {
        // Read pending_count bytes of raw (already-decrypted) data
        while (dp < dec_end && is_ps_space(*dp)) dp++;
        int count = pending_count;
        if (dp + count <= dec_end) {
          if (in_charstrings && !pending_name.empty()) {
            extract_rd_binary(dp, static_cast<size_t>(dec_end - dp), count,
                              result.lenIV, result.char_strings[pending_name]);
          } else if (in_subrs && pending_subr_idx >= 0) {
            size_t idx = static_cast<size_t>(pending_subr_idx);
            if (idx >= result.subrs.size()) {
              result.subrs.resize(idx + 1);
            }
            extract_rd_binary(dp, static_cast<size_t>(dec_end - dp), count,
                              result.lenIV, result.subrs[idx]);
          }
        }
        // Advance past the binary data
        if (dp + pending_count <= dec_end) {
          dp += pending_count;
        } else {
          dp = dec_end;
        }
        pending_count = -1;
        pending_name.clear();
        pending_subr_idx = -1;
        state = kNormal;
        continue;
      }
      // Not RD: reset state
      state = kNormal;
      pending_count = -1;
      pending_name.clear();
      pending_subr_idx = -1;
    }

    // Track context
    if (token == "/CharStrings") {
      in_charstrings = true;
      in_subrs = false;
    } else if (token == "/Subrs") {
      in_subrs = true;
      in_charstrings = false;
    } else if (token == "end") {
      if (in_charstrings || in_subrs) {
        in_charstrings = false;
        in_subrs = false;
      }
    } else if (token == "dup" && in_subrs) {
      // Next token should be the subr index
      pending_subr_idx = -1;
      // Read next token as index
      while (dp < dec_end && is_ps_space(*dp)) dp++;
      if (dp < dec_end && std::isdigit(static_cast<unsigned char>(*dp))) {
        const char* num_start = dp;
        while (dp < dec_end && std::isdigit(static_cast<unsigned char>(*dp))) dp++;
        pending_subr_idx = std::atoi(std::string(num_start, dp - num_start).c_str());
      }
    } else if (token[0] == '/' && in_charstrings) {
      // Glyph name in CharStrings dict
      pending_name = token.substr(1);
      pending_count = -1;
      pending_subr_idx = -1;
      state = kNormal;
    } else {
      // Check for integer tokens (RD length)
      bool is_int = true;
      for (char c : token) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-') {
          is_int = false;
          break;
        }
      }
      if (is_int && (in_charstrings || in_subrs)) {
        pending_count = std::atoi(token.c_str());
        state = kWantRD;
      }
    }
  }

  return !result.char_strings.empty();
}

const char* Type1Parser::FindName(const char* data, size_t size,
                                  const char* name) {
  size_t nameLen = std::strlen(name);
  const char* end = data + size - nameLen;

  for (const char* p = data; p < end; ++p) {
    if (std::strncmp(p, name, nameLen) == 0) {
      // Make sure it's a complete token
      char nextChar = p[nameLen];
      if (IsWhitespace(nextChar) || IsDelimiter(nextChar) || nextChar == '\0') {
        return p + nameLen;
      }
    }
  }

  return nullptr;
}

const char* Type1Parser::SkipWhitespaceAndComments(const char* p,
                                                    const char* end) {
  while (p < end) {
    if (IsWhitespace(*p)) {
      ++p;
    } else if (*p == '%') {
      // Skip comment to end of line
      while (p < end && *p != '\n' && *p != '\r') {
        ++p;
      }
    } else {
      break;
    }
  }
  return p;
}

std::string Type1Parser::ParseToken(const char*& p, const char* end) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end) {
    return "";
  }

  std::string token;

  if (*p == '/') {
    // Name token
    token += *p++;
    while (p < end && !IsWhitespace(*p) && !IsDelimiter(*p)) {
      token += *p++;
    }
  } else if (*p == '(') {
    // String - skip for now, handled separately
    return "";
  } else if (*p == '<' || *p == '>' || *p == '[' || *p == ']' ||
             *p == '{' || *p == '}') {
    token += *p++;
  } else {
    // Regular token
    while (p < end && !IsWhitespace(*p) && !IsDelimiter(*p)) {
      token += *p++;
    }
  }

  return token;
}

bool Type1Parser::ParseInteger(const char*& p, const char* end, int& value) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end) {
    return false;
  }

  const char* start = p;
  bool negative = false;

  if (*p == '-') {
    negative = true;
    ++p;
  } else if (*p == '+') {
    ++p;
  }

  if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) {
    p = start;
    return false;
  }

  int result = 0;
  while (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
    result = result * 10 + (*p - '0');
    ++p;
  }

  value = negative ? -result : result;
  return true;
}

bool Type1Parser::ParseReal(const char*& p, const char* end, double& value) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end) {
    return false;
  }

  const char* start = p;
  char* endPtr = nullptr;
  value = std::strtod(p, &endPtr);

  if (endPtr == p) {
    return false;
  }

  p = endPtr;
  return true;
}

bool Type1Parser::ParseString(const char*& p, const char* end,
                              std::string& value) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end || *p != '(') {
    return false;
  }

  ++p;  // Skip opening paren
  value.clear();
  int depth = 1;

  while (p < end && depth > 0) {
    if (*p == '\\' && p + 1 < end) {
      ++p;  // Skip backslash
      switch (*p) {
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case '(': value += '('; break;
        case ')': value += ')'; break;
        case '\\': value += '\\'; break;
        default:
          if (*p >= '0' && *p <= '7') {
            // Octal escape
            int octal = *p - '0';
            if (p + 1 < end && p[1] >= '0' && p[1] <= '7') {
              ++p;
              octal = octal * 8 + (*p - '0');
              if (p + 1 < end && p[1] >= '0' && p[1] <= '7') {
                ++p;
                octal = octal * 8 + (*p - '0');
              }
            }
            value += static_cast<char>(octal);
          } else {
            value += *p;
          }
          break;
      }
    } else if (*p == '(') {
      ++depth;
      value += *p;
    } else if (*p == ')') {
      --depth;
      if (depth > 0) {
        value += *p;
      }
    } else {
      value += *p;
    }
    ++p;
  }

  return depth == 0;
}

bool Type1Parser::ParseName(const char*& p, const char* end,
                            std::string& value) {
  p = SkipWhitespaceAndComments(p, end);
  if (p >= end || *p != '/') {
    return false;
  }

  ++p;  // Skip the slash
  value.clear();

  while (p < end && !IsWhitespace(*p) && !IsDelimiter(*p)) {
    value += *p++;
  }

  return !value.empty();
}

bool Type1Parser::ParseNumberArray(const char*& p, const char* end,
                                   std::vector<double>& values) {
  values.clear();

  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);

    if (*p == ']' || *p == '}') {
      ++p;
      break;
    }

    double value;
    if (ParseReal(p, end, value)) {
      values.push_back(value);
    } else {
      break;
    }
  }

  return !values.empty();
}

bool Type1Parser::ParseIntArray(const char*& p, const char* end,
                                std::vector<int>& values) {
  values.clear();

  while (p < end) {
    p = SkipWhitespaceAndComments(p, end);

    if (*p == ']' || *p == '}') {
      ++p;
      break;
    }

    int value;
    if (ParseInteger(p, end, value)) {
      values.push_back(value);
    } else {
      break;
    }
  }

  return !values.empty();
}

// ===========================================================================
// Public API: t1_glyph_outline
// ===========================================================================

int t1_glyph_outline(const Type1FontData& t1,
                     const std::string& glyph_name,
                     ttf_outline_t* out) {
  memset(out, 0, sizeof(*out));

  auto it = t1.char_strings.find(glyph_name);
  if (it == t1.char_strings.end()) return -1;

  const std::vector<uint8_t>& cs = it->second;
  if (cs.empty()) return -1;

  T1Interp ti;
  ti.subrs = &t1.subrs;

  // Scale from Type1 coordinates (usually 1/1000 of em) to outline units.
  // Type1 font_matrix is typically [0.001 0 0 0.001 0 0].
  // We keep the charstring coordinates as-is (in font units).

  if (t1_run_charstring(&ti, cs.data(), static_cast<uint32_t>(cs.size()), 0) != 0) {
    std::free(ti.pts);
    std::free(ti.ends);
    return -1;
  }

  // Close any open contour
  t1_close_contour(&ti);

  if (ti.num_pts == 0) {
    std::free(ti.pts);
    std::free(ti.ends);
    return -1;
  }

  out->points = ti.pts;
  out->num_points = ti.num_pts;
  out->contour_ends = ti.ends;
  out->num_contours = ti.num_contours;

  // Compute bounding box
  if (ti.num_pts > 0) {
    out->x_min = out->x_max = static_cast<int16_t>(ti.pts[0].x);
    out->y_min = out->y_max = static_cast<int16_t>(ti.pts[0].y);
    for (int i = 1; i < ti.num_pts; i++) {
      int16_t px = static_cast<int16_t>(ti.pts[i].x);
      int16_t py = static_cast<int16_t>(ti.pts[i].y);
      if (px < out->x_min) out->x_min = px;
      if (px > out->x_max) out->x_max = px;
      if (py < out->y_min) out->y_min = py;
      if (py > out->y_max) out->y_max = py;
    }
  }

  return 0;
}

void t1_binary_decrypt(uint8_t* buffer, uint32_t len, uint16_t key) {
  t1_crypt_inplace(buffer, len, key);
}

void t1_binary_encrypt(uint8_t* buffer, uint32_t len, uint16_t key) {
  // Encryption and decryption are the same operation (XOR cipher with
  // feedback), just with different endianness of the accumulation step.
  // The standard encrypt function uses a different accumulation:
  //   cipher[i] = plain[i] ^ (key >> 8)
  //   key = (key + plain[i]) * 52845 + 22719
  // This is identical to decryption because the key update uses the plaintext.
  t1_crypt_inplace(buffer, len, key);
}

}  // namespace nanopdf
