/* SPDX-License-Identifier: Apache-2.0
 * AES-128/256 block cipher with GCM (AEAD) and CBC modes, pure C11. */
#include "ncrypto/nc_aes.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* AES S-box / inverse S-box                                          */
/* ------------------------------------------------------------------ */
static const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

static const uint8_t kInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e,
    0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32,
    0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49,
    0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50,
    0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05,
    0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41,
    0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8,
    0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b,
    0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59,
    0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d,
    0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63,
    0x55, 0x21, 0x0c, 0x7d};

static const uint8_t kRcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10,
                                  0x20, 0x40, 0x80, 0x1b, 0x36};

/* GF(2^8) multiply for MixColumns. */
static uint8_t xtime(uint8_t x) {
  return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 1) p ^= a;
    uint8_t hi = a & 0x80;
    a = (uint8_t)(a << 1);
    if (hi) a ^= 0x1b;
    b >>= 1;
  }
  return p;
}

static uint32_t sub_word(uint32_t w) {
  return ((uint32_t)kSbox[(w >> 24) & 0xff] << 24) |
         ((uint32_t)kSbox[(w >> 16) & 0xff] << 16) |
         ((uint32_t)kSbox[(w >> 8) & 0xff] << 8) |
         ((uint32_t)kSbox[w & 0xff]);
}

static uint32_t rot_word(uint32_t w) {
  return (w << 8) | (w >> 24);
}

int nc_aes_init(nc_aes_ctx* c, const uint8_t* key, size_t keylen) {
  if (!c || !key) return -1;
  int nk;
  if (keylen == 16) {
    nk = 4;
    c->rounds = 10;
  } else if (keylen == 32) {
    nk = 8;
    c->rounds = 14;
  } else {
    return -1;
  }
  int total = 4 * (c->rounds + 1);
  for (int i = 0; i < nk; i++) {
    c->rk[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
               ((uint32_t)key[4 * i + 2] << 8) | ((uint32_t)key[4 * i + 3]);
  }
  for (int i = nk; i < total; i++) {
    uint32_t t = c->rk[i - 1];
    if (i % nk == 0) {
      t = sub_word(rot_word(t)) ^ ((uint32_t)kRcon[i / nk] << 24);
    } else if (nk > 6 && i % nk == 4) {
      t = sub_word(t);
    }
    c->rk[i] = c->rk[i - nk] ^ t;
  }
  return 0;
}

/* state stored as 16 bytes, column-major (AES standard). */
static void add_round_key(uint8_t* s, const uint32_t* rk) {
  for (int col = 0; col < 4; col++) {
    uint32_t k = rk[col];
    s[4 * col + 0] ^= (uint8_t)(k >> 24);
    s[4 * col + 1] ^= (uint8_t)(k >> 16);
    s[4 * col + 2] ^= (uint8_t)(k >> 8);
    s[4 * col + 3] ^= (uint8_t)(k);
  }
}

void nc_aes_encrypt_block(const nc_aes_ctx* c, const uint8_t in[16],
                          uint8_t out[16]) {
  uint8_t s[16];
  memcpy(s, in, 16);
  add_round_key(s, &c->rk[0]);
  for (int round = 1; round < c->rounds; round++) {
    /* SubBytes */
    for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];
    /* ShiftRows (rows are positions r within each column). */
    uint8_t t[16];
    for (int col = 0; col < 4; col++) {
      for (int r = 0; r < 4; r++) {
        t[4 * col + r] = s[4 * ((col + r) & 3) + r];
      }
    }
    /* MixColumns */
    for (int col = 0; col < 4; col++) {
      uint8_t a0 = t[4 * col + 0], a1 = t[4 * col + 1], a2 = t[4 * col + 2],
              a3 = t[4 * col + 3];
      s[4 * col + 0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
      s[4 * col + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
      s[4 * col + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
      s[4 * col + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
    add_round_key(s, &c->rk[4 * round]);
  }
  /* final round (no MixColumns) */
  for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];
  {
    uint8_t t[16];
    for (int col = 0; col < 4; col++) {
      for (int r = 0; r < 4; r++) {
        t[4 * col + r] = s[4 * ((col + r) & 3) + r];
      }
    }
    memcpy(s, t, 16);
  }
  add_round_key(s, &c->rk[4 * c->rounds]);
  memcpy(out, s, 16);
}

void nc_aes_decrypt_block(const nc_aes_ctx* c, const uint8_t in[16],
                          uint8_t out[16]) {
  uint8_t s[16];
  memcpy(s, in, 16);
  add_round_key(s, &c->rk[4 * c->rounds]);
  for (int round = c->rounds - 1; round >= 1; round--) {
    /* InvShiftRows */
    uint8_t t[16];
    for (int col = 0; col < 4; col++) {
      for (int r = 0; r < 4; r++) {
        t[4 * col + r] = s[4 * ((col - r) & 3) + r];
      }
    }
    /* InvSubBytes */
    for (int i = 0; i < 16; i++) t[i] = kInvSbox[t[i]];
    add_round_key(t, &c->rk[4 * round]);
    /* InvMixColumns */
    for (int col = 0; col < 4; col++) {
      uint8_t a0 = t[4 * col + 0], a1 = t[4 * col + 1], a2 = t[4 * col + 2],
              a3 = t[4 * col + 3];
      s[4 * col + 0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^
                                 gmul(a3, 9));
      s[4 * col + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^
                                 gmul(a3, 13));
      s[4 * col + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^
                                 gmul(a3, 11));
      s[4 * col + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^
                                 gmul(a3, 14));
    }
  }
  /* final (round 0) */
  {
    uint8_t t[16];
    for (int col = 0; col < 4; col++) {
      for (int r = 0; r < 4; r++) {
        t[4 * col + r] = s[4 * ((col - r) & 3) + r];
      }
    }
    for (int i = 0; i < 16; i++) t[i] = kInvSbox[t[i]];
    add_round_key(t, &c->rk[0]);
    memcpy(out, t, 16);
  }
}

/* ------------------------------------------------------------------ */
/* GCM                                                                */
/* ------------------------------------------------------------------ */
static void ghash_mul(uint8_t* x, const uint8_t* y) {
  uint8_t z[16] = {0};
  uint8_t v[16];
  memcpy(v, y, 16);
  for (int i = 0; i < 128; ++i) {
    if ((x[i >> 3] >> (7 - (i & 7))) & 1)
      for (int j = 0; j < 16; ++j) z[j] ^= v[j];
    int lsb = v[15] & 1;
    for (int j = 15; j > 0; --j)
      v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
    v[0] >>= 1;
    if (lsb) v[0] ^= 0xe1;
  }
  memcpy(x, z, 16);
}

static void ghash(uint8_t* tag, const uint8_t* h, const uint8_t* data,
                  size_t len) {
  for (size_t off = 0; off < len; off += 16) {
    size_t n = (len - off < 16) ? len - off : 16;
    for (size_t j = 0; j < n; ++j) tag[j] ^= data[off + j];
    ghash_mul(tag, h);
  }
}

static void gcm_tag(const nc_aes_ctx* aes, const uint8_t h[16],
                    const uint8_t iv[12], const uint8_t* aad, size_t aadlen,
                    const uint8_t* ct, size_t ctlen, uint8_t tag[16]) {
  memset(tag, 0, 16);
  ghash(tag, h, aad, aadlen);
  ghash(tag, h, ct, ctlen);
  uint8_t lenblk[16] = {0};
  uint64_t abits = (uint64_t)aadlen * 8, cbits = (uint64_t)ctlen * 8;
  for (int j = 0; j < 8; ++j) lenblk[7 - j] = (uint8_t)((abits >> (8 * j)) & 0xff);
  for (int j = 0; j < 8; ++j) lenblk[15 - j] = (uint8_t)((cbits >> (8 * j)) & 0xff);
  for (int j = 0; j < 16; ++j) tag[j] ^= lenblk[j];
  ghash_mul(tag, h);
  uint8_t j0[16] = {0};
  memcpy(j0, iv, 12);
  j0[15] = 1;
  uint8_t ej0[16];
  nc_aes_encrypt_block(aes, j0, ej0);
  for (int j = 0; j < 16; ++j) tag[j] ^= ej0[j];
}

static void gcm_ctr(const nc_aes_ctx* aes, const uint8_t iv[12],
                    const uint8_t* in, size_t len, uint8_t* out) {
  uint8_t ctr[16] = {0};
  memcpy(ctr, iv, 12);
  ctr[15] = 1;  /* J0; first keystream block uses J0+1 */
  for (size_t off = 0; off < len; off += 16) {
    for (int j = 15; j >= 12; --j)
      if (++ctr[j]) break;
    uint8_t ks[16];
    nc_aes_encrypt_block(aes, ctr, ks);
    size_t n = (len - off < 16) ? len - off : 16;
    for (size_t j = 0; j < n; ++j) out[off + j] = in[off + j] ^ ks[j];
  }
}

void nc_aes_gcm_seal(const uint8_t* key, size_t keylen, const uint8_t iv[12],
                     const uint8_t* aad, size_t aadlen, const uint8_t* pt,
                     size_t ptlen, uint8_t* ct, uint8_t tag[16]) {
  nc_aes_ctx aes;
  if (nc_aes_init(&aes, key, keylen) != 0) return;
  uint8_t h[16] = {0};
  nc_aes_encrypt_block(&aes, h, h);
  gcm_ctr(&aes, iv, pt, ptlen, ct);
  gcm_tag(&aes, h, iv, aad ? aad : (const uint8_t*)"", aadlen, ct, ptlen, tag);
}

int nc_aes_gcm_open(const uint8_t* key, size_t keylen, const uint8_t iv[12],
                    const uint8_t* aad, size_t aadlen, const uint8_t* ct,
                    size_t ctlen, const uint8_t tag[16], uint8_t* pt) {
  nc_aes_ctx aes;
  if (nc_aes_init(&aes, key, keylen) != 0) return 0;
  uint8_t h[16] = {0};
  nc_aes_encrypt_block(&aes, h, h);
  uint8_t want[16];
  gcm_tag(&aes, h, iv, aad ? aad : (const uint8_t*)"", aadlen, ct, ctlen, want);
  uint8_t diff = 0;
  for (int j = 0; j < 16; ++j) diff |= (uint8_t)(want[j] ^ tag[j]);
  if (diff) return 0;
  gcm_ctr(&aes, iv, ct, ctlen, pt);
  return 1;
}

/* ------------------------------------------------------------------ */
/* CBC                                                                */
/* ------------------------------------------------------------------ */
int nc_aes_cbc_encrypt(const uint8_t* key, size_t keylen, const uint8_t iv[16],
                       const uint8_t* in, size_t inlen, uint8_t* out) {
  if (inlen % 16 != 0) return -1;
  nc_aes_ctx aes;
  if (nc_aes_init(&aes, key, keylen) != 0) return -1;
  uint8_t prev[16];
  memcpy(prev, iv, 16);
  for (size_t off = 0; off < inlen; off += 16) {
    uint8_t blk[16];
    for (int j = 0; j < 16; ++j) blk[j] = in[off + j] ^ prev[j];
    nc_aes_encrypt_block(&aes, blk, out + off);
    memcpy(prev, out + off, 16);
  }
  return 0;
}

int nc_aes_cbc_decrypt(const uint8_t* key, size_t keylen, const uint8_t iv[16],
                       const uint8_t* in, size_t inlen, uint8_t* out) {
  if (inlen % 16 != 0) return -1;
  nc_aes_ctx aes;
  if (nc_aes_init(&aes, key, keylen) != 0) return -1;
  uint8_t prev[16];
  memcpy(prev, iv, 16);
  for (size_t off = 0; off < inlen; off += 16) {
    uint8_t cur[16];
    memcpy(cur, in + off, 16);  /* save before potential aliasing overwrite */
    uint8_t dec[16];
    nc_aes_decrypt_block(&aes, cur, dec);
    for (int j = 0; j < 16; ++j) out[off + j] = dec[j] ^ prev[j];
    memcpy(prev, cur, 16);
  }
  return 0;
}
