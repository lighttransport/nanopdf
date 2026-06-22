// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "tls-crypto.hh"

#include <cstring>

#include "crypto.hh"

namespace nanopdf {
namespace tlscrypto {

// ---- X25519 (RFC 7748) -----------------------------------------------------
// Ported from the public-domain TweetNaCl crypto_scalarmult (compact, audited).

namespace {
typedef int64_t gf[16];
const gf k121665 = {0xDB41, 1};

void car25519(gf o) {
  for (int i = 0; i < 16; ++i) {
    o[i] += (1LL << 16);
    int64_t c = o[i] >> 16;
    o[(i + 1) * (i < 15 ? 1 : 0)] += c - 1 + 37 * (c - 1) * (i == 15 ? 1 : 0);
    o[i] -= c << 16;
  }
}

void sel25519(gf p, gf q, int b) {
  int64_t t, c = ~(b - 1);
  for (int i = 0; i < 16; ++i) {
    t = c & (p[i] ^ q[i]);
    p[i] ^= t;
    q[i] ^= t;
  }
}

void pack25519(uint8_t* o, const gf n) {
  gf m, t;
  for (int i = 0; i < 16; ++i) t[i] = n[i];
  car25519(t);
  car25519(t);
  car25519(t);
  for (int j = 0; j < 2; ++j) {
    m[0] = t[0] - 0xffed;
    for (int i = 1; i < 15; ++i) {
      m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
      m[i - 1] &= 0xffff;
    }
    m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
    int b = (m[15] >> 16) & 1;
    m[14] &= 0xffff;
    sel25519(t, m, 1 - b);
  }
  for (int i = 0; i < 16; ++i) {
    o[2 * i] = t[i] & 0xff;
    o[2 * i + 1] = t[i] >> 8;
  }
}

void unpack25519(gf o, const uint8_t* n) {
  for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
  o[15] &= 0x7fff;
}

void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
void Zsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }

void M(gf o, const gf a, const gf b) {
  int64_t t[31];
  for (int i = 0; i < 31; ++i) t[i] = 0;
  for (int i = 0; i < 16; ++i)
    for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
  for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
  for (int i = 0; i < 16; ++i) o[i] = t[i];
  car25519(o);
  car25519(o);
}

void inv25519(gf o, const gf i) {
  gf c;
  for (int a = 0; a < 16; ++a) c[a] = i[a];
  for (int a = 253; a >= 0; --a) {
    M(c, c, c);
    if (a != 2 && a != 4) M(c, c, i);
  }
  for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void scalarmult(uint8_t* q, const uint8_t* n, const uint8_t* p) {
  uint8_t z[32];
  gf x, a, b, c, d, e, f;
  for (int i = 0; i < 31; ++i) z[i] = n[i];
  z[31] = (n[31] & 127) | 64;
  z[0] &= 248;
  unpack25519(x, p);
  for (int i = 0; i < 16; ++i) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
  a[0] = d[0] = 1;
  for (int i = 254; i >= 0; --i) {
    int r = (z[i >> 3] >> (i & 7)) & 1;
    sel25519(a, b, r);
    sel25519(c, d, r);
    A(e, a, c);
    Zsub(a, a, c);
    A(c, b, d);
    Zsub(b, b, d);
    M(d, e, e);
    M(f, a, a);
    M(a, c, a);
    M(c, b, e);
    A(e, a, c);
    Zsub(a, a, c);
    M(b, a, a);
    Zsub(c, d, f);
    M(a, c, k121665);
    A(a, a, d);
    M(c, c, a);
    M(a, d, f);
    M(d, b, x);
    M(b, e, e);
    sel25519(a, b, r);
    sel25519(c, d, r);
  }
  inv25519(c, c);  // result = a / c
  M(a, a, c);
  pack25519(q, a);
}
}  // namespace

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
  scalarmult(out, scalar, point);
}
void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
  uint8_t base[32] = {9};
  scalarmult(out, scalar, base);
}

// ---- AES-128-GCM -----------------------------------------------------------

namespace {
// GF(2^128) multiplication (GCM, big-endian bit order), x *= y in place.
void ghash_mul(uint8_t* x, const uint8_t* y) {
  uint8_t z[16] = {0};
  uint8_t v[16];
  std::memcpy(v, y, 16);
  for (int i = 0; i < 128; ++i) {
    if ((x[i >> 3] >> (7 - (i & 7))) & 1)
      for (int j = 0; j < 16; ++j) z[j] ^= v[j];
    bool lsb = v[15] & 1;
    for (int j = 15; j > 0; --j) v[j] = (v[j] >> 1) | (v[j - 1] << 7);
    v[0] >>= 1;
    if (lsb) v[0] ^= 0xe1;  // reduction polynomial
  }
  std::memcpy(x, z, 16);
}

void ghash(uint8_t* tag, const uint8_t* h, const uint8_t* data, size_t len) {
  for (size_t off = 0; off < len; off += 16) {
    size_t n = (len - off < 16) ? len - off : 16;
    for (size_t j = 0; j < n; ++j) tag[j] ^= data[off + j];
    ghash_mul(tag, h);
  }
}

void gcm_core(crypto::AES128& aes, const uint8_t iv[12], const Bytes& aad,
              const uint8_t* in, size_t len, uint8_t* out, uint8_t tag[16]) {
  uint8_t h[16] = {0};
  aes.encrypt_block(h, h);  // H = E(0)
  uint8_t j0[16] = {0};
  std::memcpy(j0, iv, 12);
  j0[15] = 1;
  // CTR starting at J0+1.
  uint8_t ctr[16];
  std::memcpy(ctr, j0, 16);
  for (size_t off = 0; off < len; off += 16) {
    for (int j = 15; j >= 12; --j)
      if (++ctr[j]) break;
    uint8_t ks[16];
    aes.encrypt_block(ctr, ks);
    size_t n = (len - off < 16) ? len - off : 16;
    for (size_t j = 0; j < n; ++j) out[off + j] = in[off + j] ^ ks[j];
  }
  // GHASH(AAD || C || len(AAD)||len(C)).
  std::memset(tag, 0, 16);
  ghash(tag, h, aad.data(), aad.size());
  ghash(tag, h, out, len);  // ciphertext
  uint8_t lenblk[16] = {0};
  uint64_t abits = (uint64_t)aad.size() * 8, cbits = (uint64_t)len * 8;
  for (int j = 0; j < 8; ++j) lenblk[7 - j] = (abits >> (8 * j)) & 0xff;
  for (int j = 0; j < 8; ++j) lenblk[15 - j] = (cbits >> (8 * j)) & 0xff;
  for (int j = 0; j < 16; ++j) tag[j] ^= lenblk[j];
  ghash_mul(tag, h);
  uint8_t ej0[16];
  aes.encrypt_block(j0, ej0);
  for (int j = 0; j < 16; ++j) tag[j] ^= ej0[j];
}
}  // namespace

Bytes aes128_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                      const Bytes& aad, const Bytes& pt) {
  crypto::AES128 aes;
  aes.set_key(key);
  Bytes out(pt.size() + 16);
  uint8_t tag[16];
  gcm_core(aes, iv, aad, pt.data(), pt.size(), out.data(), tag);
  std::memcpy(out.data() + pt.size(), tag, 16);
  return out;
}

bool aes128_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                     const Bytes& aad, const Bytes& ct_tag, Bytes* pt) {
  if (ct_tag.size() < 16) return false;
  size_t clen = ct_tag.size() - 16;
  crypto::AES128 aes;
  aes.set_key(key);
  // Re-derive the expected tag from the ciphertext, then CTR-decrypt.
  uint8_t h[16] = {0};
  aes.encrypt_block(h, h);
  uint8_t tag[16];
  std::memset(tag, 0, 16);
  ghash(tag, h, aad.data(), aad.size());
  ghash(tag, h, ct_tag.data(), clen);
  uint8_t lenblk[16] = {0};
  uint64_t abits = (uint64_t)aad.size() * 8, cbits = (uint64_t)clen * 8;
  for (int j = 0; j < 8; ++j) lenblk[7 - j] = (abits >> (8 * j)) & 0xff;
  for (int j = 0; j < 8; ++j) lenblk[15 - j] = (cbits >> (8 * j)) & 0xff;
  for (int j = 0; j < 16; ++j) tag[j] ^= lenblk[j];
  ghash_mul(tag, h);
  uint8_t j0[16] = {0};
  std::memcpy(j0, iv, 12);
  j0[15] = 1;
  uint8_t ej0[16];
  aes.encrypt_block(j0, ej0);
  for (int j = 0; j < 16; ++j) tag[j] ^= ej0[j];
  // Constant-time-ish tag compare.
  uint8_t diff = 0;
  for (int j = 0; j < 16; ++j) diff |= tag[j] ^ ct_tag[clen + j];
  if (diff) return false;
  // Decrypt.
  pt->assign(clen, 0);
  uint8_t ctr[16];
  std::memcpy(ctr, j0, 16);
  for (size_t off = 0; off < clen; off += 16) {
    for (int j = 15; j >= 12; --j)
      if (++ctr[j]) break;
    uint8_t ks[16];
    aes.encrypt_block(ctr, ks);
    size_t n = (clen - off < 16) ? clen - off : 16;
    for (size_t j = 0; j < n; ++j) (*pt)[off + j] = ct_tag[off + j] ^ ks[j];
  }
  return true;
}

// ---- HKDF-SHA256 -----------------------------------------------------------

Bytes hkdf_extract(const Bytes& salt, const Bytes& ikm) {
  Bytes s = salt.empty() ? Bytes(32, 0) : salt;
  return crypto::hmac(crypto::Prf::Sha256, s.data(), s.size(), ikm.data(),
                      ikm.size());
}

Bytes hkdf_expand(const Bytes& prk, const Bytes& info, size_t length) {
  Bytes out;
  Bytes t;
  uint8_t counter = 1;
  while (out.size() < length) {
    Bytes msg = t;
    msg.insert(msg.end(), info.begin(), info.end());
    msg.push_back(counter++);
    t = crypto::hmac(crypto::Prf::Sha256, prk.data(), prk.size(), msg.data(),
                     msg.size());
    out.insert(out.end(), t.begin(), t.end());
  }
  out.resize(length);
  return out;
}

Bytes hkdf_expand_label(const Bytes& secret, const std::string& label,
                        const Bytes& context, size_t length) {
  // HkdfLabel = uint16 length || opaque label<7..255> ("tls13 "+label) ||
  //             opaque context<0..255>
  std::string full = "tls13 " + label;
  Bytes info;
  info.push_back((length >> 8) & 0xff);
  info.push_back(length & 0xff);
  info.push_back((uint8_t)full.size());
  info.insert(info.end(), full.begin(), full.end());
  info.push_back((uint8_t)context.size());
  info.insert(info.end(), context.begin(), context.end());
  return hkdf_expand(secret, info, length);
}

}  // namespace tlscrypto
}  // namespace nanopdf
