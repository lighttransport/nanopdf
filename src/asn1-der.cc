// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "asn1-der.hh"

#include <algorithm>

namespace nanopdf {
namespace asn1 {

Bytes encode_length(size_t len) {
  Bytes out;
  if (len < 0x80) {
    out.push_back(static_cast<uint8_t>(len));
    return out;
  }
  Bytes tmp;
  while (len) {
    tmp.push_back(static_cast<uint8_t>(len & 0xFF));
    len >>= 8;
  }
  out.push_back(static_cast<uint8_t>(0x80 | tmp.size()));
  out.insert(out.end(), tmp.rbegin(), tmp.rend());  // big-endian
  return out;
}

Bytes tlv(uint8_t tag, const Bytes& content) {
  Bytes out;
  out.push_back(tag);
  Bytes len = encode_length(content.size());
  out.insert(out.end(), len.begin(), len.end());
  out.insert(out.end(), content.begin(), content.end());
  return out;
}

Bytes der_integer(const Bytes& be) {
  // Strip leading zeros, then add one back if the top bit is set (so the value
  // stays non-negative), per DER INTEGER encoding.
  size_t i = 0;
  while (i + 1 < be.size() && be[i] == 0) ++i;
  Bytes mag(be.begin() + i, be.end());
  if (mag.empty()) mag.push_back(0);
  Bytes content;
  if (mag[0] & 0x80) content.push_back(0x00);
  content.insert(content.end(), mag.begin(), mag.end());
  return tlv(TAG_INTEGER, content);
}

Bytes der_integer_u32(uint32_t v) {
  Bytes be;
  for (int s = 24; s >= 0; s -= 8) {
    uint8_t b = static_cast<uint8_t>((v >> s) & 0xFF);
    if (!be.empty() || b || s == 0) be.push_back(b);
  }
  return der_integer(be);
}

Bytes der_oid(const std::string& dotted) {
  // Parse the dotted decimal into arc values.
  std::vector<uint64_t> arcs;
  uint64_t cur = 0;
  bool have = false;
  for (char c : dotted) {
    if (c == '.') { arcs.push_back(cur); cur = 0; have = false; }
    else if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); have = true; }
  }
  if (have) arcs.push_back(cur);
  Bytes content;
  if (arcs.size() >= 2) {
    content.push_back(static_cast<uint8_t>(40 * arcs[0] + arcs[1]));
    for (size_t i = 2; i < arcs.size(); ++i) {
      uint64_t v = arcs[i];
      Bytes tmp;
      tmp.push_back(static_cast<uint8_t>(v & 0x7F));
      v >>= 7;
      while (v) { tmp.push_back(static_cast<uint8_t>((v & 0x7F) | 0x80)); v >>= 7; }
      content.insert(content.end(), tmp.rbegin(), tmp.rend());  // big-endian base128
    }
  }
  return tlv(TAG_OID, content);
}

Bytes der_octet_string(const Bytes& data) { return tlv(TAG_OCTET_STRING, data); }

Bytes der_bit_string(const Bytes& data) {
  Bytes content;
  content.push_back(0x00);  // number of unused bits
  content.insert(content.end(), data.begin(), data.end());
  return tlv(TAG_BIT_STRING, content);
}

Bytes der_null() { return Bytes{TAG_NULL, 0x00}; }

Bytes der_bool(bool v) { return Bytes{TAG_BOOLEAN, 0x01, uint8_t(v ? 0xFF : 0)}; }

Bytes der_printable_string(const std::string& s) {
  return tlv(TAG_PRINTABLESTRING, Bytes(s.begin(), s.end()));
}

Bytes der_utf8_string(const std::string& s) {
  return tlv(TAG_UTF8STRING, Bytes(s.begin(), s.end()));
}

Bytes der_utctime(const std::string& s) {
  return tlv(TAG_UTCTIME, Bytes(s.begin(), s.end()));
}

Bytes der_generalizedtime(const std::string& s) {
  return tlv(TAG_GENERALIZEDTIME, Bytes(s.begin(), s.end()));
}

static Bytes concat(const std::vector<Bytes>& items) {
  Bytes c;
  for (const auto& it : items) c.insert(c.end(), it.begin(), it.end());
  return c;
}

Bytes der_sequence(const std::vector<Bytes>& items) {
  return tlv(TAG_SEQUENCE, concat(items));
}

Bytes der_set(const std::vector<Bytes>& items) {
  // DER SET OF: components sorted by their full encoding.
  std::vector<Bytes> sorted = items;
  std::sort(sorted.begin(), sorted.end());
  return tlv(TAG_SET, concat(sorted));
}

Bytes der_set_raw(const std::vector<Bytes>& items) {
  return tlv(TAG_SET, concat(items));
}

Bytes der_context(int n, bool constructed, const Bytes& content) {
  uint8_t tag = static_cast<uint8_t>((constructed ? 0xA0 : 0x80) | (n & 0x1F));
  return tlv(tag, content);
}

Bytes der_algorithm_id(const std::string& oid, bool null_params) {
  std::vector<Bytes> items{der_oid(oid)};
  if (null_params) items.push_back(der_null());
  return der_sequence(items);
}

}  // namespace asn1
}  // namespace nanopdf
