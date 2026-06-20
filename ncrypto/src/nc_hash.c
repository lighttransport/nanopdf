/* SPDX-License-Identifier: Apache-2.0
 * SHA-1 / SHA-256 / SHA-384 / SHA-512 for the ncrypto C11 library.
 * Ported from nanopdf src/crypto.cc (classes SHA1, SHA256, SHA512, SHA384). */
#include "ncrypto/nc_hash.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* SHA-256                                                                    */
/* ------------------------------------------------------------------------- */

static const uint32_t kSHA256InitialState[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

static const uint32_t kSHA256RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotr32(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t* block) {
  uint32_t w[64];
  int i;
  uint32_t a, b, c, d, e, f, g, h;

  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = s1 + w[i - 7] + s0 + w[i - 16];
  }

  a = state[0]; b = state[1]; c = state[2]; d = state[3];
  e = state[4]; f = state[5]; g = state[6]; h = state[7];

  for (i = 0; i < 64; i++) {
    uint32_t ep1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t temp1 = h + ep1 + ch + kSHA256RoundConstants[i] + w[i];
    uint32_t ep0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = ep0 + maj;
    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }

  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void nc_sha256_init(nc_sha256_ctx* c) {
  memcpy(c->state, kSHA256InitialState, sizeof(c->state));
  c->count = 0;
  c->buflen = 0;
}

void nc_sha256_update(nc_sha256_ctx* c, const uint8_t* data, size_t len) {
  size_t i = 0;
  if (data == NULL || len == 0) return;
  c->count += len;

  if (c->buflen > 0) {
    size_t need = 64 - c->buflen;
    size_t take = len < need ? len : need;
    memcpy(c->buf + c->buflen, data, take);
    c->buflen += take;
    i = take;
    if (c->buflen == 64) {
      sha256_transform(c->state, c->buf);
      c->buflen = 0;
    }
  }
  for (; i + 64 <= len; i += 64) {
    sha256_transform(c->state, data + i);
  }
  if (i < len) {
    memcpy(c->buf + c->buflen, data + i, len - i);
    c->buflen += len - i;
  }
}

void nc_sha256_final(nc_sha256_ctx* c, uint8_t out[NC_SHA256_LEN]) {
  uint64_t bit_count = c->count * 8;
  uint8_t pad = 0x80;
  uint8_t zero = 0;
  uint8_t bits[8];
  int i;

  nc_sha256_update(c, &pad, 1);
  while (c->buflen != 56) {
    nc_sha256_update(c, &zero, 1);
  }
  for (i = 0; i < 8; i++) {
    bits[7 - i] = (uint8_t)((bit_count >> (i * 8)) & 0xff);
  }
  nc_sha256_update(c, bits, 8);

  for (i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(c->state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->state[i]);
  }
}

void nc_sha256(const uint8_t* data, size_t len, uint8_t out[NC_SHA256_LEN]) {
  nc_sha256_ctx c;
  nc_sha256_init(&c);
  nc_sha256_update(&c, data, len);
  nc_sha256_final(&c, out);
}

/* ------------------------------------------------------------------------- */
/* SHA-1                                                                      */
/* ------------------------------------------------------------------------- */

static uint32_t rotl32(uint32_t x, uint32_t n) {
  return (x << n) | (x >> (32 - n));
}

static void sha1_transform(uint32_t state[5], const uint8_t* block) {
  uint32_t w[80];
  int i;
  uint32_t a, b, c, d, e;

  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (i = 16; i < 80; i++) {
    w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

  for (i = 0; i < 80; i++) {
    uint32_t f, k, temp;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    temp = rotl32(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rotl32(b, 30); b = a; a = temp;
  }

  state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void nc_sha1_init(nc_sha1_ctx* c) {
  c->state[0] = 0x67452301u;
  c->state[1] = 0xEFCDAB89u;
  c->state[2] = 0x98BADCFEu;
  c->state[3] = 0x10325476u;
  c->state[4] = 0xC3D2E1F0u;
  c->count = 0;
  c->buflen = 0;
}

void nc_sha1_update(nc_sha1_ctx* c, const uint8_t* data, size_t len) {
  size_t i = 0;
  if (data == NULL || len == 0) return;
  c->count += len;

  if (c->buflen > 0) {
    size_t need = 64 - c->buflen;
    size_t take = len < need ? len : need;
    memcpy(c->buf + c->buflen, data, take);
    c->buflen += take;
    i = take;
    if (c->buflen == 64) {
      sha1_transform(c->state, c->buf);
      c->buflen = 0;
    }
  }
  for (; i + 64 <= len; i += 64) {
    sha1_transform(c->state, data + i);
  }
  if (i < len) {
    memcpy(c->buf + c->buflen, data + i, len - i);
    c->buflen += len - i;
  }
}

void nc_sha1_final(nc_sha1_ctx* c, uint8_t out[NC_SHA1_LEN]) {
  uint64_t bit_count = c->count * 8;
  uint8_t pad = 0x80;
  uint8_t zero = 0;
  uint8_t bits[8];
  int i;

  nc_sha1_update(c, &pad, 1);
  while (c->buflen != 56) {
    nc_sha1_update(c, &zero, 1);
  }
  for (i = 0; i < 8; i++) {
    bits[7 - i] = (uint8_t)((bit_count >> (i * 8)) & 0xff);
  }
  nc_sha1_update(c, bits, 8);

  for (i = 0; i < 5; i++) {
    out[i * 4] = (uint8_t)(c->state[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->state[i]);
  }
}

void nc_sha1(const uint8_t* data, size_t len, uint8_t out[NC_SHA1_LEN]) {
  nc_sha1_ctx c;
  nc_sha1_init(&c);
  nc_sha1_update(&c, data, len);
  nc_sha1_final(&c, out);
}

/* ------------------------------------------------------------------------- */
/* SHA-512 / SHA-384                                                          */
/* ------------------------------------------------------------------------- */

static const uint64_t kSHA512RoundConstants[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static const uint64_t kSHA512InitialState[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

static const uint64_t kSHA384InitialState[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
    0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
    0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};

static uint64_t rotr64(uint64_t x, uint64_t n) {
  return (x >> n) | (x << (64 - n));
}

static void sha512_transform(uint64_t state[8], const uint8_t* block) {
  uint64_t w[80];
  int i;
  uint64_t a, b, c, d, e, f, g, h;

  for (i = 0; i < 16; i++) {
    w[i] = ((uint64_t)block[i * 8] << 56) | ((uint64_t)block[i * 8 + 1] << 48) |
           ((uint64_t)block[i * 8 + 2] << 40) |
           ((uint64_t)block[i * 8 + 3] << 32) |
           ((uint64_t)block[i * 8 + 4] << 24) |
           ((uint64_t)block[i * 8 + 5] << 16) |
           ((uint64_t)block[i * 8 + 6] << 8) | (uint64_t)block[i * 8 + 7];
  }
  for (i = 16; i < 80; i++) {
    uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
    uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
    w[i] = s1 + w[i - 7] + s0 + w[i - 16];
  }

  a = state[0]; b = state[1]; c = state[2]; d = state[3];
  e = state[4]; f = state[5]; g = state[6]; h = state[7];

  for (i = 0; i < 80; i++) {
    uint64_t ep1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
    uint64_t ch = (e & f) ^ (~e & g);
    uint64_t temp1 = h + ep1 + ch + kSHA512RoundConstants[i] + w[i];
    uint64_t ep0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
    uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint64_t temp2 = ep0 + maj;
    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }

  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha512_update_impl(nc_sha512_ctx* c, const uint8_t* data,
                               size_t len) {
  size_t i = 0;
  if (data == NULL || len == 0) return;
  c->count += len;

  if (c->buflen > 0) {
    size_t need = 128 - c->buflen;
    size_t take = len < need ? len : need;
    memcpy(c->buf + c->buflen, data, take);
    c->buflen += take;
    i = take;
    if (c->buflen == 128) {
      sha512_transform(c->state, c->buf);
      c->buflen = 0;
    }
  }
  for (; i + 128 <= len; i += 128) {
    sha512_transform(c->state, data + i);
  }
  if (i < len) {
    memcpy(c->buf + c->buflen, data + i, len - i);
    c->buflen += len - i;
  }
}

static void sha512_pad(nc_sha512_ctx* c) {
  uint64_t bit_count = c->count * 8;
  uint8_t pad = 0x80;
  uint8_t zero = 0;
  uint8_t bits[16];
  int i;

  sha512_update_impl(c, &pad, 1);
  while (c->buflen != 112) {
    sha512_update_impl(c, &zero, 1);
  }
  memset(bits, 0, 8); /* upper 64 bits of the 128-bit length are zero */
  for (i = 0; i < 8; i++) {
    bits[15 - i] = (uint8_t)((bit_count >> (i * 8)) & 0xff);
  }
  sha512_update_impl(c, bits, 16);
}

void nc_sha512_init(nc_sha512_ctx* c) {
  memcpy(c->state, kSHA512InitialState, sizeof(c->state));
  c->count = 0;
  c->buflen = 0;
}

void nc_sha512_update(nc_sha512_ctx* c, const uint8_t* data, size_t len) {
  sha512_update_impl(c, data, len);
}

void nc_sha512_final(nc_sha512_ctx* c, uint8_t out[NC_SHA512_LEN]) {
  int i;
  sha512_pad(c);
  for (i = 0; i < 8; i++) {
    out[i * 8] = (uint8_t)(c->state[i] >> 56);
    out[i * 8 + 1] = (uint8_t)(c->state[i] >> 48);
    out[i * 8 + 2] = (uint8_t)(c->state[i] >> 40);
    out[i * 8 + 3] = (uint8_t)(c->state[i] >> 32);
    out[i * 8 + 4] = (uint8_t)(c->state[i] >> 24);
    out[i * 8 + 5] = (uint8_t)(c->state[i] >> 16);
    out[i * 8 + 6] = (uint8_t)(c->state[i] >> 8);
    out[i * 8 + 7] = (uint8_t)(c->state[i]);
  }
}

void nc_sha512(const uint8_t* data, size_t len, uint8_t out[NC_SHA512_LEN]) {
  nc_sha512_ctx c;
  nc_sha512_init(&c);
  nc_sha512_update(&c, data, len);
  nc_sha512_final(&c, out);
}

void nc_sha384_init(nc_sha512_ctx* c) {
  memcpy(c->state, kSHA384InitialState, sizeof(c->state));
  c->count = 0;
  c->buflen = 0;
}

void nc_sha384_update(nc_sha512_ctx* c, const uint8_t* data, size_t len) {
  sha512_update_impl(c, data, len);
}

void nc_sha384_final(nc_sha512_ctx* c, uint8_t out[NC_SHA384_LEN]) {
  int i;
  sha512_pad(c);
  for (i = 0; i < 6; i++) {
    out[i * 8] = (uint8_t)(c->state[i] >> 56);
    out[i * 8 + 1] = (uint8_t)(c->state[i] >> 48);
    out[i * 8 + 2] = (uint8_t)(c->state[i] >> 40);
    out[i * 8 + 3] = (uint8_t)(c->state[i] >> 32);
    out[i * 8 + 4] = (uint8_t)(c->state[i] >> 24);
    out[i * 8 + 5] = (uint8_t)(c->state[i] >> 16);
    out[i * 8 + 6] = (uint8_t)(c->state[i] >> 8);
    out[i * 8 + 7] = (uint8_t)(c->state[i]);
  }
}

void nc_sha384(const uint8_t* data, size_t len, uint8_t out[NC_SHA384_LEN]) {
  nc_sha512_ctx c;
  nc_sha384_init(&c);
  nc_sha384_update(&c, data, len);
  nc_sha384_final(&c, out);
}
