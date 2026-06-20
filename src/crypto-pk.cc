// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "crypto-pk.hh"

#include <algorithm>
#include <cstring>

namespace nanopdf {
namespace crypto {

// ---- BigInt ----------------------------------------------------------------

void BigInt::trim() {
  while (!limbs_.empty() && limbs_.back() == 0) limbs_.pop_back();
}

BigInt BigInt::from_bytes(const uint8_t* data, size_t len) {
  BigInt r;
  // Strip leading zero bytes, then pack big-endian bytes into 32-bit limbs.
  size_t start = 0;
  while (start < len && data[start] == 0) ++start;
  size_t n = len - start;
  if (n == 0) return r;
  r.limbs_.assign((n + 3) / 4, 0);
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = data[len - 1 - i];  // least-significant byte first
    r.limbs_[i / 4] |= static_cast<uint32_t>(b) << (8 * (i % 4));
  }
  r.trim();
  return r;
}

std::vector<uint8_t> BigInt::to_bytes(size_t fixed_len) const {
  // Big-endian, minimal length unless fixed_len given (left zero-padded).
  std::vector<uint8_t> be;
  for (size_t i = 0; i < limbs_.size(); ++i) {
    uint32_t l = limbs_[i];
    be.push_back(static_cast<uint8_t>(l & 0xFF));
    be.push_back(static_cast<uint8_t>((l >> 8) & 0xFF));
    be.push_back(static_cast<uint8_t>((l >> 16) & 0xFF));
    be.push_back(static_cast<uint8_t>((l >> 24) & 0xFF));
  }
  while (be.size() > 1 && be.back() == 0) be.pop_back();  // drop high zeros
  std::reverse(be.begin(), be.end());
  if (fixed_len) {
    if (be.size() > fixed_len) be.erase(be.begin(), be.end() - fixed_len);
    else if (be.size() < fixed_len)
      be.insert(be.begin(), fixed_len - be.size(), 0);
  }
  return be;
}

size_t BigInt::bit_length() const {
  if (limbs_.empty()) return 0;
  uint32_t top = limbs_.back();
  size_t bits = (limbs_.size() - 1) * 32;
  while (top) { ++bits; top >>= 1; }
  return bits;
}

int BigInt::cmp(const BigInt& a, const BigInt& b) {
  if (a.limbs_.size() != b.limbs_.size())
    return a.limbs_.size() < b.limbs_.size() ? -1 : 1;
  for (size_t i = a.limbs_.size(); i-- > 0;)
    if (a.limbs_[i] != b.limbs_[i]) return a.limbs_[i] < b.limbs_[i] ? -1 : 1;
  return 0;
}

BigInt BigInt::add(const BigInt& a, const BigInt& b) {
  BigInt r;
  size_t n = std::max(a.limbs_.size(), b.limbs_.size());
  r.limbs_.assign(n, 0);
  uint64_t carry = 0;
  for (size_t i = 0; i < n; ++i) {
    uint64_t s = carry;
    if (i < a.limbs_.size()) s += a.limbs_[i];
    if (i < b.limbs_.size()) s += b.limbs_[i];
    r.limbs_[i] = static_cast<uint32_t>(s);
    carry = s >> 32;
  }
  if (carry) r.limbs_.push_back(static_cast<uint32_t>(carry));
  return r;
}

BigInt BigInt::sub(const BigInt& a, const BigInt& b) {
  BigInt r;  // assumes a >= b
  r.limbs_.assign(a.limbs_.size(), 0);
  int64_t borrow = 0;
  for (size_t i = 0; i < a.limbs_.size(); ++i) {
    int64_t s = static_cast<int64_t>(a.limbs_[i]) - borrow -
                (i < b.limbs_.size() ? static_cast<int64_t>(b.limbs_[i]) : 0);
    if (s < 0) { s += (int64_t(1) << 32); borrow = 1; } else borrow = 0;
    r.limbs_[i] = static_cast<uint32_t>(s);
  }
  r.trim();
  return r;
}

BigInt BigInt::mul(const BigInt& a, const BigInt& b) {
  BigInt r;
  if (a.is_zero() || b.is_zero()) return r;
  r.limbs_.assign(a.limbs_.size() + b.limbs_.size(), 0);
  for (size_t i = 0; i < a.limbs_.size(); ++i) {
    uint64_t carry = 0;
    uint64_t ai = a.limbs_[i];
    for (size_t j = 0; j < b.limbs_.size(); ++j) {
      uint64_t cur = r.limbs_[i + j] + ai * b.limbs_[j] + carry;
      r.limbs_[i + j] = static_cast<uint32_t>(cur);
      carry = cur >> 32;
    }
    r.limbs_[i + b.limbs_.size()] += static_cast<uint32_t>(carry);
  }
  r.trim();
  return r;
}

// Shift left by one bit.
static void shl1(BigInt& v, std::vector<uint32_t>& limbs) {
  (void)v;
  uint32_t carry = 0;
  for (size_t i = 0; i < limbs.size(); ++i) {
    uint32_t nc = limbs[i] >> 31;
    limbs[i] = (limbs[i] << 1) | carry;
    carry = nc;
  }
  if (carry) limbs.push_back(carry);
}

void BigInt::divmod(const BigInt& a, const BigInt& b, BigInt* q, BigInt* r) {
  // Bit-at-a-time long division (simple, correct; not the fastest).
  BigInt quotient, remainder;
  if (cmp(a, b) < 0) {
    if (q) *q = quotient;
    if (r) *r = a;
    return;
  }
  size_t bits = a.bit_length();
  quotient.limbs_.assign((bits + 31) / 32, 0);
  for (size_t i = bits; i-- > 0;) {
    // remainder = (remainder << 1) | bit i of a
    shl1(remainder, remainder.limbs_);
    uint32_t bit = (a.limbs_[i / 32] >> (i % 32)) & 1u;
    if (bit) {
      if (remainder.limbs_.empty()) remainder.limbs_.push_back(1);
      else remainder.limbs_[0] |= 1u;
    }
    remainder.trim();
    if (cmp(remainder, b) >= 0) {
      remainder = sub(remainder, b);
      quotient.limbs_[i / 32] |= (1u << (i % 32));
    }
  }
  quotient.trim();
  if (q) *q = quotient;
  if (r) *r = remainder;
}

BigInt BigInt::mod(const BigInt& a, const BigInt& m) {
  BigInt r;
  divmod(a, m, nullptr, &r);
  return r;
}

BigInt BigInt::modexp(const BigInt& base, const BigInt& exp, const BigInt& m) {
  BigInt result(1);
  BigInt b = mod(base, m);
  size_t bits = exp.bit_length();
  for (size_t i = 0; i < bits; ++i) {
    if ((exp.limbs_[i / 32] >> (i % 32)) & 1u)
      result = mod(mul(result, b), m);
    b = mod(mul(b, b), m);
  }
  return result;
}

// ---- minimal DER reader (for RSA key parsing) ------------------------------

namespace {

struct Der {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
};

// Read a tag+length; on success advance past the length and set @content/@clen
// to the value bytes; returns the tag (or 0 on error).
uint8_t der_read_tl(Der& d, const uint8_t** content, size_t* clen) {
  if (!d.ok || d.p + 2 > d.end) { d.ok = false; return 0; }
  uint8_t tag = *d.p++;
  size_t len = *d.p++;
  if (len & 0x80) {
    size_t nb = len & 0x7F;
    if (nb == 0 || nb > 4 || d.p + nb > d.end) { d.ok = false; return 0; }
    len = 0;
    for (size_t i = 0; i < nb; ++i) len = (len << 8) | *d.p++;
  }
  if (d.p + len > d.end) { d.ok = false; return 0; }
  *content = d.p;
  *clen = len;
  d.p += len;
  return tag;
}

BigInt der_read_integer(Der& d) {
  const uint8_t* c;
  size_t n;
  uint8_t tag = der_read_tl(d, &c, &n);
  if (tag != 0x02) { d.ok = false; return BigInt(); }
  return BigInt::from_bytes(c, n);
}

std::vector<uint8_t> pem_to_der(const std::string& pem, const char* label) {
  std::string begin = std::string("-----BEGIN ") + label + "-----";
  std::string end = std::string("-----END ") + label + "-----";
  size_t b = pem.find(begin);
  if (b == std::string::npos) return {};
  b += begin.size();
  size_t e = pem.find(end, b);
  if (e == std::string::npos) return {};
  std::string body = pem.substr(b, e - b);
  static const char* B64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int dtab[256];
  for (int i = 0; i < 256; ++i) dtab[i] = -1;
  for (int i = 0; i < 64; ++i) dtab[(unsigned char)B64[i]] = i;
  std::vector<uint8_t> out;
  int val = 0, bits = 0;
  for (char ch : body) {
    int dv = dtab[(unsigned char)ch];
    if (dv < 0) continue;  // skip whitespace/newlines/padding
    val = (val << 6) | dv;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace

RsaPrivateKey rsa_parse_private_key_der(const uint8_t* der, size_t len) {
  RsaPrivateKey key;
  Der d{der, der + len, true};
  const uint8_t* seq;
  size_t seqlen;
  if (der_read_tl(d, &seq, &seqlen) != 0x30) return key;  // outer SEQUENCE
  Der inner{seq, seq + seqlen, true};

  // Could be PKCS#1 RSAPrivateKey or PKCS#8 PrivateKeyInfo. Peek the first
  // INTEGER (version). For PKCS#8 it is followed by an AlgorithmIdentifier
  // SEQUENCE and an OCTET STRING wrapping the PKCS#1 key.
  const uint8_t* save = inner.p;
  BigInt version = der_read_integer(inner);
  (void)version;
  const uint8_t* c;
  size_t n;
  uint8_t next = der_read_tl(inner, &c, &n);
  if (next == 0x30) {
    // PKCS#8: skip AlgorithmIdentifier, read the OCTET STRING, recurse.
    uint8_t t = der_read_tl(inner, &c, &n);
    if (t != 0x04) return key;
    return rsa_parse_private_key_der(c, n);
  }
  // PKCS#1: rewind and parse version,n,e,d,...
  Der p1{save, seq + seqlen, true};
  der_read_integer(p1);            // version (0)
  key.n = der_read_integer(p1);    // modulus
  key.e = der_read_integer(p1);    // publicExponent
  key.d = der_read_integer(p1);    // privateExponent
  if (!p1.ok || key.n.is_zero() || key.d.is_zero()) return key;
  key.modulus_bytes = (key.n.bit_length() + 7) / 8;
  key.valid = true;
  return key;
}

RsaPrivateKey rsa_parse_private_key_pem(const std::string& pem) {
  std::vector<uint8_t> der = pem_to_der(pem, "RSA PRIVATE KEY");
  if (der.empty()) der = pem_to_der(pem, "PRIVATE KEY");
  if (der.empty()) return RsaPrivateKey{};
  return rsa_parse_private_key_der(der.data(), der.size());
}

// ---- RSA PKCS#1 v1.5 -------------------------------------------------------

static const uint8_t kSha256DigestInfoPrefix[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
    0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

std::vector<uint8_t> rsa_sign_pkcs1v15(const RsaPrivateKey& key,
                                       const uint8_t* digest_info,
                                       size_t digest_info_len) {
  if (!key.valid) return {};
  size_t k = key.modulus_bytes;
  if (digest_info_len + 11 > k) return {};  // message too long for modulus
  // EM = 0x00 || 0x01 || PS(0xFF...) || 0x00 || DigestInfo
  std::vector<uint8_t> em(k, 0xFF);
  em[0] = 0x00;
  em[1] = 0x01;
  size_t ps_len = k - digest_info_len - 3;
  em[2 + ps_len] = 0x00;
  std::memcpy(em.data() + 3 + ps_len, digest_info, digest_info_len);

  BigInt m = BigInt::from_bytes(em.data(), em.size());
  BigInt s = BigInt::modexp(m, key.d, key.n);
  return s.to_bytes(k);
}

std::vector<uint8_t> rsa_sign_sha256(const RsaPrivateKey& key,
                                     const uint8_t sha256[32]) {
  std::vector<uint8_t> di(std::begin(kSha256DigestInfoPrefix),
                          std::end(kSha256DigestInfoPrefix));
  di.insert(di.end(), sha256, sha256 + 32);
  return rsa_sign_pkcs1v15(key, di.data(), di.size());
}

bool rsa_verify_pkcs1v15(const RsaPublicKey& key, const uint8_t* sig,
                         size_t sig_len, const uint8_t* digest_info,
                         size_t digest_info_len) {
  if (!key.valid || sig_len != key.modulus_bytes) return false;
  BigInt s = BigInt::from_bytes(sig, sig_len);
  BigInt m = BigInt::modexp(s, key.e, key.n);
  std::vector<uint8_t> em = m.to_bytes(key.modulus_bytes);
  // Rebuild the expected EM and compare.
  size_t k = key.modulus_bytes;
  if (digest_info_len + 11 > k) return false;
  std::vector<uint8_t> exp(k, 0xFF);
  exp[0] = 0x00;
  exp[1] = 0x01;
  size_t ps_len = k - digest_info_len - 3;
  exp[2 + ps_len] = 0x00;
  std::memcpy(exp.data() + 3 + ps_len, digest_info, digest_info_len);
  return em.size() == exp.size() &&
         std::memcmp(em.data(), exp.data(), k) == 0;
}

}  // namespace crypto
}  // namespace nanopdf
