// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "crypto.hh"
#include <algorithm>
#include <cstring>

namespace nanopdf {
namespace crypto {

namespace {

const uint32_t kSHA256InitialState[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

const uint32_t kSHA256RoundConstants[64] = {
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

inline uint8_t aes_xtime(uint8_t x) {
  return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

inline uint8_t aes_mul9(uint8_t x) {
  uint8_t x2 = aes_xtime(x);
  uint8_t x4 = aes_xtime(x2);
  uint8_t x8 = aes_xtime(x4);
  return static_cast<uint8_t>(x8 ^ x);
}

inline uint8_t aes_mul11(uint8_t x) {
  uint8_t x2 = aes_xtime(x);
  uint8_t x4 = aes_xtime(x2);
  uint8_t x8 = aes_xtime(x4);
  return static_cast<uint8_t>(x8 ^ x2 ^ x);
}

inline uint8_t aes_mul13(uint8_t x) {
  uint8_t x2 = aes_xtime(x);
  uint8_t x4 = aes_xtime(x2);
  uint8_t x8 = aes_xtime(x4);
  return static_cast<uint8_t>(x8 ^ x4 ^ x);
}

inline uint8_t aes_mul14(uint8_t x) {
  uint8_t x2 = aes_xtime(x);
  uint8_t x4 = aes_xtime(x2);
  uint8_t x8 = aes_xtime(x4);
  return static_cast<uint8_t>(x8 ^ x4 ^ x2);
}

}  // namespace

// RC4 Implementation
RC4::RC4() : x(0), y(0) {
  for (int i = 0; i < 256; i++) {
    state[i] = i;
  }
}

void RC4::swap(uint8_t& a, uint8_t& b) {
  uint8_t tmp = a;
  a = b;
  b = tmp;
}

void RC4::init(const uint8_t* key, size_t key_len) {
  x = 0;
  y = 0;

  // Initialize state
  for (int i = 0; i < 256; i++) {
    state[i] = i;
  }

  // Key scheduling algorithm
  uint8_t j = 0;
  for (int i = 0; i < 256; i++) {
    j = j + state[i] + key[i % key_len];
    swap(state[i], state[j]);
  }
}

void RC4::crypt(uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    x = (x + 1) & 0xFF;
    y = (y + state[x]) & 0xFF;
    swap(state[x], state[y]);
    uint8_t k = state[(state[x] + state[y]) & 0xFF];
    data[i] ^= k;
  }
}

// AES S-box and inverse S-box
static const uint8_t sbox[256] = {
  0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
  0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
  0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
  0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
  0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
  0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
  0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
  0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
  0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
  0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
  0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
  0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
  0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
  0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
  0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
  0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

static const uint8_t inv_sbox[256] = {
  0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
  0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
  0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
  0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
  0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
  0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
  0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
  0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
  0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
  0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
  0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
  0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
  0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
  0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
  0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
  0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

// Rcon for key expansion
static const uint32_t rcon[10] = {
  0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000,
  0x20000000, 0x40000000, 0x80000000, 0x1b000000, 0x36000000
};

// AES-128 Implementation
AES128::AES128() {
  memset(round_keys, 0, sizeof(round_keys));
}

uint8_t AES128::gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 1) {
      p ^= a;
    }
    uint8_t hi_bit = a & 0x80;
    a <<= 1;
    if (hi_bit) {
      a ^= 0x1b; // x^8 + x^4 + x^3 + x + 1
    }
    b >>= 1;
  }
  return p;
}

void AES128::sub_bytes(uint8_t* state) {
  for (int i = 0; i < 16; i++) {
    state[i] = sbox[state[i]];
  }
}

void AES128::inv_sub_bytes(uint8_t* state) {
  for (int i = 0; i < 16; i++) {
    state[i] = inv_sbox[state[i]];
  }
}

void AES128::shift_rows(uint8_t* state) {
  uint8_t temp;

  // Row 1: shift left by 1
  temp = state[1];
  state[1] = state[5];
  state[5] = state[9];
  state[9] = state[13];
  state[13] = temp;

  // Row 2: shift left by 2
  temp = state[2];
  state[2] = state[10];
  state[10] = temp;
  temp = state[6];
  state[6] = state[14];
  state[14] = temp;

  // Row 3: shift left by 3 (or right by 1)
  temp = state[15];
  state[15] = state[11];
  state[11] = state[7];
  state[7] = state[3];
  state[3] = temp;
}

void AES128::inv_shift_rows(uint8_t* state) {
  uint8_t temp;

  // Row 1: shift right by 1
  temp = state[13];
  state[13] = state[9];
  state[9] = state[5];
  state[5] = state[1];
  state[1] = temp;

  // Row 2: shift right by 2
  temp = state[2];
  state[2] = state[10];
  state[10] = temp;
  temp = state[6];
  state[6] = state[14];
  state[14] = temp;

  // Row 3: shift right by 3 (or left by 1)
  temp = state[3];
  state[3] = state[7];
  state[7] = state[11];
  state[11] = state[15];
  state[15] = temp;
}

void AES128::mix_columns(uint8_t* state) {
  for (int i = 0; i < 4; i++) {
    uint8_t a[4];
    uint8_t b[4];
    for (int j = 0; j < 4; j++) {
      a[j] = state[i * 4 + j];
      b[j] = (a[j] << 1) ^ ((a[j] & 0x80) ? 0x1b : 0);
    }
    state[i * 4 + 0] = b[0] ^ a[1] ^ b[1] ^ a[2] ^ a[3];
    state[i * 4 + 1] = a[0] ^ b[1] ^ a[2] ^ b[2] ^ a[3];
    state[i * 4 + 2] = a[0] ^ a[1] ^ b[2] ^ a[3] ^ b[3];
    state[i * 4 + 3] = a[0] ^ b[0] ^ a[1] ^ a[2] ^ b[3];
  }
}

void AES128::inv_mix_columns(uint8_t* state) {
  for (int i = 0; i < 4; i++) {
    uint8_t a[4];
    for (int j = 0; j < 4; j++) {
      a[j] = state[i * 4 + j];
    }
    state[i * 4 + 0] = aes_mul14(a[0]) ^ aes_mul11(a[1]) ^ aes_mul13(a[2]) ^ aes_mul9(a[3]);
    state[i * 4 + 1] = aes_mul9(a[0]) ^ aes_mul14(a[1]) ^ aes_mul11(a[2]) ^ aes_mul13(a[3]);
    state[i * 4 + 2] = aes_mul13(a[0]) ^ aes_mul9(a[1]) ^ aes_mul14(a[2]) ^ aes_mul11(a[3]);
    state[i * 4 + 3] = aes_mul11(a[0]) ^ aes_mul13(a[1]) ^ aes_mul9(a[2]) ^ aes_mul14(a[3]);
  }
}

void AES128::add_round_key(uint8_t* state, const uint8_t* round_key) {
  for (int i = 0; i < 16; i++) {
    state[i] ^= round_key[i];
  }
}

void AES128::key_expansion(const uint8_t* key) {
  // Copy the key as the first round key
  memcpy(round_keys[0], key, 16);

  // Generate the remaining round keys
  for (int i = 1; i < 11; i++) {
    uint8_t temp[4];

    // Copy the last word of the previous round key
    memcpy(temp, &round_keys[i-1][12], 4);

    // RotWord
    uint8_t t = temp[0];
    temp[0] = temp[1];
    temp[1] = temp[2];
    temp[2] = temp[3];
    temp[3] = t;

    // SubWord
    for (int j = 0; j < 4; j++) {
      temp[j] = sbox[temp[j]];
    }

    // XOR with Rcon
    temp[0] ^= (rcon[i-1] >> 24) & 0xFF;

    // XOR with first word of previous round key
    for (int j = 0; j < 4; j++) {
      round_keys[i][j] = round_keys[i-1][j] ^ temp[j];
    }

    // Generate the rest of the round key
    for (int j = 4; j < 16; j++) {
      round_keys[i][j] = round_keys[i-1][j] ^ round_keys[i][j-4];
    }
  }
}

void AES128::set_key(const uint8_t* key) {
  key_expansion(key);
}

void AES128::encrypt_block(const uint8_t* in, uint8_t* out) {
  uint8_t state[16];
  memcpy(state, in, 16);

  // Initial round
  add_round_key(state, round_keys[0]);

  // Main rounds
  for (int round = 1; round < 10; round++) {
    sub_bytes(state);
    shift_rows(state);
    mix_columns(state);
    add_round_key(state, round_keys[round]);
  }

  // Final round
  sub_bytes(state);
  shift_rows(state);
  add_round_key(state, round_keys[10]);

  memcpy(out, state, 16);
}

void AES128::decrypt_block(const uint8_t* in, uint8_t* out) {
  uint8_t state[16];
  memcpy(state, in, 16);

  // Initial round
  add_round_key(state, round_keys[10]);

  // Main rounds
  for (int round = 9; round > 0; round--) {
    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, round_keys[round]);
    inv_mix_columns(state);
  }

  // Final round
  inv_shift_rows(state);
  inv_sub_bytes(state);
  add_round_key(state, round_keys[0]);

  memcpy(out, state, 16);
}

void AES128::encrypt_cbc(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv) {
  uint8_t prev_block[16];
  memcpy(prev_block, iv, 16);

  for (size_t i = 0; i < len; i += 16) {
    uint8_t block[16];
    memcpy(block, &in[i], 16);

    // XOR with previous ciphertext block
    for (int j = 0; j < 16; j++) {
      block[j] ^= prev_block[j];
    }

    encrypt_block(block, &out[i]);
    memcpy(prev_block, &out[i], 16);
  }
}

void AES128::decrypt_cbc(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv) {
  uint8_t prev_block[16];
  memcpy(prev_block, iv, 16);

  for (size_t i = 0; i < len; i += 16) {
    uint8_t block[16];
    decrypt_block(&in[i], block);

    // XOR with previous ciphertext block
    for (int j = 0; j < 16; j++) {
      out[i + j] = block[j] ^ prev_block[j];
    }

    memcpy(prev_block, &in[i], 16);
  }
}

// MD5 Implementation
MD5::MD5() : count(0), finalized(false) {
  // Initialize MD5 state
  state[0] = 0x67452301;
  state[1] = 0xefcdab89;
  state[2] = 0x98badcfe;
  state[3] = 0x10325476;
  memset(buffer, 0, sizeof(buffer));
  memset(digest, 0, sizeof(digest));
}

uint32_t MD5::f(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) | (~x & z);
}

uint32_t MD5::g(uint32_t x, uint32_t y, uint32_t z) {
  return (x & z) | (y & ~z);
}

uint32_t MD5::h(uint32_t x, uint32_t y, uint32_t z) {
  return x ^ y ^ z;
}

uint32_t MD5::i(uint32_t x, uint32_t y, uint32_t z) {
  return y ^ (x | ~z);
}

uint32_t MD5::rotate_left(uint32_t x, uint32_t n) {
  return (x << n) | (x >> (32 - n));
}

void MD5::transform(const uint8_t* block) {
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t x[16];

  // Convert block to 32-bit words
  for (int i = 0; i < 16; i++) {
    x[i] = block[i * 4] | (block[i * 4 + 1] << 8) |
           (block[i * 4 + 2] << 16) | (block[i * 4 + 3] << 24);
  }

  // Round 1
  #define FF(a, b, c, d, x, s, ac) { \
    a += f(b, c, d) + x + ac; \
    a = rotate_left(a, s); \
    a += b; \
  }

  FF(a, b, c, d, x[ 0],  7, 0xd76aa478);
  FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
  FF(c, d, a, b, x[ 2], 17, 0x242070db);
  FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
  FF(a, b, c, d, x[ 4],  7, 0xf57c0faf);
  FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
  FF(c, d, a, b, x[ 6], 17, 0xa8304613);
  FF(b, c, d, a, x[ 7], 22, 0xfd469501);
  FF(a, b, c, d, x[ 8],  7, 0x698098d8);
  FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
  FF(c, d, a, b, x[10], 17, 0xffff5bb1);
  FF(b, c, d, a, x[11], 22, 0x895cd7be);
  FF(a, b, c, d, x[12],  7, 0x6b901122);
  FF(d, a, b, c, x[13], 12, 0xfd987193);
  FF(c, d, a, b, x[14], 17, 0xa679438e);
  FF(b, c, d, a, x[15], 22, 0x49b40821);

  // Round 2
  #define GG(a, b, c, d, x, s, ac) { \
    a += g(b, c, d) + x + ac; \
    a = rotate_left(a, s); \
    a += b; \
  }

  GG(a, b, c, d, x[ 1],  5, 0xf61e2562);
  GG(d, a, b, c, x[ 6],  9, 0xc040b340);
  GG(c, d, a, b, x[11], 14, 0x265e5a51);
  GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
  GG(a, b, c, d, x[ 5],  5, 0xd62f105d);
  GG(d, a, b, c, x[10],  9, 0x02441453);
  GG(c, d, a, b, x[15], 14, 0xd8a1e681);
  GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
  GG(a, b, c, d, x[ 9],  5, 0x21e1cde6);
  GG(d, a, b, c, x[14],  9, 0xc33707d6);
  GG(c, d, a, b, x[ 3], 14, 0xf4d50d87);
  GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
  GG(a, b, c, d, x[13],  5, 0xa9e3e905);
  GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);
  GG(c, d, a, b, x[ 7], 14, 0x676f02d9);
  GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

  // Round 3
  #define HH(a, b, c, d, x, s, ac) { \
    a += h(b, c, d) + x + ac; \
    a = rotate_left(a, s); \
    a += b; \
  }

  HH(a, b, c, d, x[ 5],  4, 0xfffa3942);
  HH(d, a, b, c, x[ 8], 11, 0x8771f681);
  HH(c, d, a, b, x[11], 16, 0x6d9d6122);
  HH(b, c, d, a, x[14], 23, 0xfde5380c);
  HH(a, b, c, d, x[ 1],  4, 0xa4beea44);
  HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
  HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60);
  HH(b, c, d, a, x[10], 23, 0xbebfbc70);
  HH(a, b, c, d, x[13],  4, 0x289b7ec6);
  HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
  HH(c, d, a, b, x[ 3], 16, 0xd4ef3085);
  HH(b, c, d, a, x[ 6], 23, 0x04881d05);
  HH(a, b, c, d, x[ 9],  4, 0xd9d4d039);
  HH(d, a, b, c, x[12], 11, 0xe6db99e5);
  HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
  HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

  // Round 4
  #define II(a, b, c, d, x, s, ac) { \
    a += i(b, c, d) + x + ac; \
    a = rotate_left(a, s); \
    a += b; \
  }

  II(a, b, c, d, x[ 0],  6, 0xf4292244);
  II(d, a, b, c, x[ 7], 10, 0x432aff97);
  II(c, d, a, b, x[14], 15, 0xab9423a7);
  II(b, c, d, a, x[ 5], 21, 0xfc93a039);
  II(a, b, c, d, x[12],  6, 0x655b59c3);
  II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
  II(c, d, a, b, x[10], 15, 0xffeff47d);
  II(b, c, d, a, x[ 1], 21, 0x85845dd1);
  II(a, b, c, d, x[ 8],  6, 0x6fa87e4f);
  II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
  II(c, d, a, b, x[ 6], 15, 0xa3014314);
  II(b, c, d, a, x[13], 21, 0x4e0811a1);
  II(a, b, c, d, x[ 4],  6, 0xf7537e82);
  II(d, a, b, c, x[11], 10, 0xbd3af235);
  II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb);
  II(b, c, d, a, x[ 9], 21, 0xeb86d391);

  #undef FF
  #undef GG
  #undef HH
  #undef II

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;

  // Clear sensitive information
  memset(x, 0, sizeof(x));
}

void MD5::update(const uint8_t* data, size_t len) {
  if (finalized || len == 0) {
    return;
  }

  if (data == nullptr) {
    return;
  }

  size_t index = (count / 8) % 64;
  count += len * 8;

  size_t part_len = 64 - index;
  size_t i = 0;

  if (len >= part_len) {
    memcpy(&buffer[index], data, part_len);
    transform(buffer);

    for (i = part_len; i + 63 < len; i += 64) {
      transform(&data[i]);
    }

    index = 0;
  }

  memcpy(&buffer[index], &data[i], len - i);
}

void MD5::finalize() {
  if (finalized) {
    return;
  }

  // Save the original count (in bits) before padding
  uint64_t original_count = count;

  uint8_t padding[64];
  memset(padding, 0, sizeof(padding));
  padding[0] = 0x80;

  uint32_t index = (count / 8) % 64;
  uint32_t pad_len = (index < 56) ? (56 - index) : (120 - index);
  update(padding, pad_len);

  // Append original length (before padding)
  uint8_t bits[8];
  for (int i = 0; i < 8; i++) {
    bits[i] = (original_count >> (i * 8)) & 0xff;
  }
  update(bits, 8);

  // Store digest
  for (int i = 0; i < 4; i++) {
    digest[i * 4] = (state[i]) & 0xff;
    digest[i * 4 + 1] = (state[i] >> 8) & 0xff;
    digest[i * 4 + 2] = (state[i] >> 16) & 0xff;
    digest[i * 4 + 3] = (state[i] >> 24) & 0xff;
  }

  finalized = true;
}

void MD5::get_digest(uint8_t* out) const {
  memcpy(out, digest, 16);
}

void MD5::hash(const uint8_t* data, size_t len, uint8_t* out) {
  MD5 md5;
  md5.update(data, len);
  md5.finalize();
  md5.get_digest(out);
}

SHA256::SHA256() : count(0), finalized(false) {
  std::memcpy(state, kSHA256InitialState, sizeof(state));
  std::memset(buffer, 0, sizeof(buffer));
  std::memset(digest, 0, sizeof(digest));
}

uint32_t SHA256::rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

uint32_t SHA256::ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}

uint32_t SHA256::maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

uint32_t SHA256::ep0(uint32_t x) {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

uint32_t SHA256::ep1(uint32_t x) {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

uint32_t SHA256::sig0(uint32_t x) {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

uint32_t SHA256::sig1(uint32_t x) {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

void SHA256::transform(const uint8_t* block) {
  uint32_t w[64];

  for (int i = 0; i < 16; i++) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }

  for (int i = 16; i < 64; i++) {
    w[i] = sig1(w[i - 2]) + w[i - 7] + sig0(w[i - 15]) + w[i - 16];
  }

  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];

  for (int i = 0; i < 64; i++) {
    uint32_t temp1 = h + ep1(e) + ch(e, f, g) + kSHA256RoundConstants[i] + w[i];
    uint32_t temp2 = ep0(a) + maj(a, b, c);

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void SHA256::update(const uint8_t* data, size_t len) {
  if (finalized || len == 0 || data == nullptr) {
    return;
  }

  size_t index = (count / 8) % 64;
  count += static_cast<uint64_t>(len) * 8;

  size_t part_len = 64 - index;
  size_t i = 0;

  if (len >= part_len) {
    std::memcpy(&buffer[index], data, part_len);
    transform(buffer);

    for (i = part_len; i + 63 < len; i += 64) {
      transform(&data[i]);
    }

    index = 0;
  }

  std::memcpy(&buffer[index], &data[i], len - i);
}

void SHA256::finalize() {
  if (finalized) {
    return;
  }

  // Save original bit count BEFORE adding padding
  uint64_t bit_count = count;

  uint8_t pad_block[64];
  std::memset(pad_block, 0, sizeof(pad_block));
  pad_block[0] = 0x80;

  uint32_t index = (count / 8) % 64;
  uint32_t pad_len = (index < 56) ? (56 - index) : (120 - index);
  update(pad_block, pad_len);

  uint8_t bits[8];
  for (int i = 0; i < 8; i++) {
    bits[7 - i] = static_cast<uint8_t>((bit_count >> (i * 8)) & 0xff);
  }
  update(bits, 8);

  for (int i = 0; i < 8; i++) {
    digest[i * 4] = static_cast<uint8_t>((state[i] >> 24) & 0xff);
    digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xff);
    digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xff);
    digest[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xff);
  }

  finalized = true;
}

void SHA256::get_digest(uint8_t* out) const {
  std::memcpy(out, digest, DIGEST_SIZE);
}

void SHA256::hash(const uint8_t* data, size_t len, uint8_t* out) {
  SHA256 sha;
  sha.update(data, len);
  sha.finalize();
  sha.get_digest(out);
}

// SHA-1 Implementation
SHA1::SHA1() : count(0), finalized(false) {
  state[0] = 0x67452301;
  state[1] = 0xEFCDAB89;
  state[2] = 0x98BADCFE;
  state[3] = 0x10325476;
  state[4] = 0xC3D2E1F0;
  std::memset(buffer, 0, sizeof(buffer));
  std::memset(digest, 0, sizeof(digest));
}

uint32_t SHA1::rotl(uint32_t x, uint32_t n) {
  return (x << n) | (x >> (32 - n));
}

void SHA1::transform(const uint8_t* block) {
  uint32_t w[80];

  for (int i = 0; i < 16; i++) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }

  for (int i = 16; i < 80; i++) {
    w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];

  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }

    uint32_t temp = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

void SHA1::update(const uint8_t* data, size_t len) {
  if (finalized || len == 0 || data == nullptr) {
    return;
  }

  size_t index = (count / 8) % 64;
  count += static_cast<uint64_t>(len) * 8;

  size_t part_len = 64 - index;
  size_t i = 0;

  if (len >= part_len) {
    std::memcpy(&buffer[index], data, part_len);
    transform(buffer);

    for (i = part_len; i + 63 < len; i += 64) {
      transform(&data[i]);
    }

    index = 0;
  }

  std::memcpy(&buffer[index], &data[i], len - i);
}

void SHA1::finalize() {
  if (finalized) {
    return;
  }

  uint64_t bit_count = count;

  uint8_t pad_block[64];
  std::memset(pad_block, 0, sizeof(pad_block));
  pad_block[0] = 0x80;

  uint32_t index = (count / 8) % 64;
  uint32_t pad_len = (index < 56) ? (56 - index) : (120 - index);
  update(pad_block, pad_len);

  uint8_t bits[8];
  for (int i = 0; i < 8; i++) {
    bits[7 - i] = static_cast<uint8_t>((bit_count >> (i * 8)) & 0xff);
  }
  update(bits, 8);

  for (int i = 0; i < 5; i++) {
    digest[i * 4] = static_cast<uint8_t>((state[i] >> 24) & 0xff);
    digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16) & 0xff);
    digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8) & 0xff);
    digest[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xff);
  }

  finalized = true;
}

void SHA1::get_digest(uint8_t* out) const {
  std::memcpy(out, digest, DIGEST_SIZE);
}

void SHA1::hash(const uint8_t* data, size_t len, uint8_t* out) {
  SHA1 sha;
  sha.update(data, len);
  sha.finalize();
  sha.get_digest(out);
}

// SHA-512 constants
namespace {

const uint64_t kSHA512RoundConstants[80] = {
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

const uint64_t kSHA512InitialState[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

const uint64_t kSHA384InitialState[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
    0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
    0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};

}  // namespace

// SHA-512 Implementation
uint64_t SHA512::rotr64(uint64_t x, uint64_t n) {
  return (x >> n) | (x << (64 - n));
}

uint64_t SHA512::ch64(uint64_t x, uint64_t y, uint64_t z) {
  return (x & y) ^ (~x & z);
}

uint64_t SHA512::maj64(uint64_t x, uint64_t y, uint64_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

uint64_t SHA512::ep0_64(uint64_t x) {
  return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39);
}

uint64_t SHA512::ep1_64(uint64_t x) {
  return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41);
}

uint64_t SHA512::sig0_64(uint64_t x) {
  return rotr64(x, 1) ^ rotr64(x, 8) ^ (x >> 7);
}

uint64_t SHA512::sig1_64(uint64_t x) {
  return rotr64(x, 19) ^ rotr64(x, 61) ^ (x >> 6);
}

SHA512::SHA512() : count(0), finalized(false) {
  std::memcpy(state, kSHA512InitialState, sizeof(state));
  std::memset(buffer, 0, sizeof(buffer));
  std::memset(digest, 0, sizeof(digest));
}

void SHA512::transform(const uint8_t* block) {
  uint64_t w[80];

  for (int i = 0; i < 16; i++) {
    w[i] = (static_cast<uint64_t>(block[i * 8]) << 56) |
           (static_cast<uint64_t>(block[i * 8 + 1]) << 48) |
           (static_cast<uint64_t>(block[i * 8 + 2]) << 40) |
           (static_cast<uint64_t>(block[i * 8 + 3]) << 32) |
           (static_cast<uint64_t>(block[i * 8 + 4]) << 24) |
           (static_cast<uint64_t>(block[i * 8 + 5]) << 16) |
           (static_cast<uint64_t>(block[i * 8 + 6]) << 8) |
           static_cast<uint64_t>(block[i * 8 + 7]);
  }

  for (int i = 16; i < 80; i++) {
    w[i] = sig1_64(w[i - 2]) + w[i - 7] + sig0_64(w[i - 15]) + w[i - 16];
  }

  uint64_t a = state[0];
  uint64_t b = state[1];
  uint64_t c = state[2];
  uint64_t d = state[3];
  uint64_t e = state[4];
  uint64_t f = state[5];
  uint64_t g = state[6];
  uint64_t h = state[7];

  for (int i = 0; i < 80; i++) {
    uint64_t temp1 = h + ep1_64(e) + ch64(e, f, g) + kSHA512RoundConstants[i] + w[i];
    uint64_t temp2 = ep0_64(a) + maj64(a, b, c);

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void SHA512::update(const uint8_t* data, size_t len) {
  if (finalized || len == 0 || data == nullptr) {
    return;
  }

  size_t index = static_cast<size_t>((count / 8) % 128);
  count += static_cast<uint64_t>(len) * 8;

  size_t part_len = 128 - index;
  size_t i = 0;

  if (len >= part_len) {
    std::memcpy(&buffer[index], data, part_len);
    transform(buffer);

    for (i = part_len; i + 127 < len; i += 128) {
      transform(&data[i]);
    }

    index = 0;
  }

  std::memcpy(&buffer[index], &data[i], len - i);
}

void SHA512::finalize() {
  if (finalized) {
    return;
  }

  uint64_t bit_count = count;

  uint8_t pad_block[128];
  std::memset(pad_block, 0, sizeof(pad_block));
  pad_block[0] = 0x80;

  size_t index = static_cast<size_t>((count / 8) % 128);
  size_t pad_len = (index < 112) ? (112 - index) : (240 - index);
  update(pad_block, pad_len);

  // SHA-512 uses 128-bit length, but we only track 64 bits
  uint8_t bits[16];
  std::memset(bits, 0, 8);  // Upper 64 bits are zero
  for (int i = 0; i < 8; i++) {
    bits[15 - i] = static_cast<uint8_t>((bit_count >> (i * 8)) & 0xff);
  }
  update(bits, 16);

  for (int i = 0; i < 8; i++) {
    digest[i * 8] = static_cast<uint8_t>((state[i] >> 56) & 0xff);
    digest[i * 8 + 1] = static_cast<uint8_t>((state[i] >> 48) & 0xff);
    digest[i * 8 + 2] = static_cast<uint8_t>((state[i] >> 40) & 0xff);
    digest[i * 8 + 3] = static_cast<uint8_t>((state[i] >> 32) & 0xff);
    digest[i * 8 + 4] = static_cast<uint8_t>((state[i] >> 24) & 0xff);
    digest[i * 8 + 5] = static_cast<uint8_t>((state[i] >> 16) & 0xff);
    digest[i * 8 + 6] = static_cast<uint8_t>((state[i] >> 8) & 0xff);
    digest[i * 8 + 7] = static_cast<uint8_t>(state[i] & 0xff);
  }

  finalized = true;
}

void SHA512::get_digest(uint8_t* out) const {
  std::memcpy(out, digest, DIGEST_SIZE);
}

void SHA512::hash(const uint8_t* data, size_t len, uint8_t* out) {
  SHA512 sha;
  sha.update(data, len);
  sha.finalize();
  sha.get_digest(out);
}

// SHA-384 Implementation (SHA-512 with different IV, truncated to 48 bytes)
uint64_t SHA384::rotr64(uint64_t x, uint64_t n) {
  return (x >> n) | (x << (64 - n));
}

uint64_t SHA384::ch64(uint64_t x, uint64_t y, uint64_t z) {
  return (x & y) ^ (~x & z);
}

uint64_t SHA384::maj64(uint64_t x, uint64_t y, uint64_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

uint64_t SHA384::ep0_64(uint64_t x) {
  return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39);
}

uint64_t SHA384::ep1_64(uint64_t x) {
  return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41);
}

uint64_t SHA384::sig0_64(uint64_t x) {
  return rotr64(x, 1) ^ rotr64(x, 8) ^ (x >> 7);
}

uint64_t SHA384::sig1_64(uint64_t x) {
  return rotr64(x, 19) ^ rotr64(x, 61) ^ (x >> 6);
}

SHA384::SHA384() : count(0), finalized(false) {
  std::memcpy(state, kSHA384InitialState, sizeof(state));
  std::memset(buffer, 0, sizeof(buffer));
  std::memset(digest, 0, sizeof(digest));
}

void SHA384::transform(const uint8_t* block) {
  uint64_t w[80];

  for (int i = 0; i < 16; i++) {
    w[i] = (static_cast<uint64_t>(block[i * 8]) << 56) |
           (static_cast<uint64_t>(block[i * 8 + 1]) << 48) |
           (static_cast<uint64_t>(block[i * 8 + 2]) << 40) |
           (static_cast<uint64_t>(block[i * 8 + 3]) << 32) |
           (static_cast<uint64_t>(block[i * 8 + 4]) << 24) |
           (static_cast<uint64_t>(block[i * 8 + 5]) << 16) |
           (static_cast<uint64_t>(block[i * 8 + 6]) << 8) |
           static_cast<uint64_t>(block[i * 8 + 7]);
  }

  for (int i = 16; i < 80; i++) {
    w[i] = sig1_64(w[i - 2]) + w[i - 7] + sig0_64(w[i - 15]) + w[i - 16];
  }

  uint64_t a = state[0];
  uint64_t b = state[1];
  uint64_t c = state[2];
  uint64_t d = state[3];
  uint64_t e = state[4];
  uint64_t f = state[5];
  uint64_t g = state[6];
  uint64_t h = state[7];

  for (int i = 0; i < 80; i++) {
    uint64_t temp1 = h + ep1_64(e) + ch64(e, f, g) + kSHA512RoundConstants[i] + w[i];
    uint64_t temp2 = ep0_64(a) + maj64(a, b, c);

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

void SHA384::update(const uint8_t* data, size_t len) {
  if (finalized || len == 0 || data == nullptr) {
    return;
  }

  size_t index = static_cast<size_t>((count / 8) % 128);
  count += static_cast<uint64_t>(len) * 8;

  size_t part_len = 128 - index;
  size_t i = 0;

  if (len >= part_len) {
    std::memcpy(&buffer[index], data, part_len);
    transform(buffer);

    for (i = part_len; i + 127 < len; i += 128) {
      transform(&data[i]);
    }

    index = 0;
  }

  std::memcpy(&buffer[index], &data[i], len - i);
}

void SHA384::finalize() {
  if (finalized) {
    return;
  }

  uint64_t bit_count = count;

  uint8_t pad_block[128];
  std::memset(pad_block, 0, sizeof(pad_block));
  pad_block[0] = 0x80;

  size_t index = static_cast<size_t>((count / 8) % 128);
  size_t pad_len = (index < 112) ? (112 - index) : (240 - index);
  update(pad_block, pad_len);

  uint8_t bits[16];
  std::memset(bits, 0, 8);
  for (int i = 0; i < 8; i++) {
    bits[15 - i] = static_cast<uint8_t>((bit_count >> (i * 8)) & 0xff);
  }
  update(bits, 16);

  // SHA-384: take first 48 bytes (6 words) from SHA-512 state
  for (int i = 0; i < 6; i++) {
    digest[i * 8] = static_cast<uint8_t>((state[i] >> 56) & 0xff);
    digest[i * 8 + 1] = static_cast<uint8_t>((state[i] >> 48) & 0xff);
    digest[i * 8 + 2] = static_cast<uint8_t>((state[i] >> 40) & 0xff);
    digest[i * 8 + 3] = static_cast<uint8_t>((state[i] >> 32) & 0xff);
    digest[i * 8 + 4] = static_cast<uint8_t>((state[i] >> 24) & 0xff);
    digest[i * 8 + 5] = static_cast<uint8_t>((state[i] >> 16) & 0xff);
    digest[i * 8 + 6] = static_cast<uint8_t>((state[i] >> 8) & 0xff);
    digest[i * 8 + 7] = static_cast<uint8_t>(state[i] & 0xff);
  }

  finalized = true;
}

void SHA384::get_digest(uint8_t* out) const {
  std::memcpy(out, digest, DIGEST_SIZE);
}

void SHA384::hash(const uint8_t* data, size_t len, uint8_t* out) {
  SHA384 sha;
  sha.update(data, len);
  sha.finalize();
  sha.get_digest(out);
}

// AES-256 Implementation
AES256::AES256() {
  memset(round_keys, 0, sizeof(round_keys));
}

uint8_t AES256::gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  for (int i = 0; i < 8; i++) {
    if (b & 1) {
      p ^= a;
    }
    uint8_t hi_bit = a & 0x80;
    a <<= 1;
    if (hi_bit) {
      a ^= 0x1b;
    }
    b >>= 1;
  }
  return p;
}

void AES256::sub_bytes(uint8_t* state) {
  for (int i = 0; i < 16; i++) {
    state[i] = sbox[state[i]];
  }
}

void AES256::inv_sub_bytes(uint8_t* state) {
  for (int i = 0; i < 16; i++) {
    state[i] = inv_sbox[state[i]];
  }
}

void AES256::shift_rows(uint8_t* state) {
  uint8_t temp;
  temp = state[1];
  state[1] = state[5];
  state[5] = state[9];
  state[9] = state[13];
  state[13] = temp;

  temp = state[2];
  state[2] = state[10];
  state[10] = temp;
  temp = state[6];
  state[6] = state[14];
  state[14] = temp;

  temp = state[15];
  state[15] = state[11];
  state[11] = state[7];
  state[7] = state[3];
  state[3] = temp;
}

void AES256::inv_shift_rows(uint8_t* state) {
  uint8_t temp;
  temp = state[13];
  state[13] = state[9];
  state[9] = state[5];
  state[5] = state[1];
  state[1] = temp;

  temp = state[2];
  state[2] = state[10];
  state[10] = temp;
  temp = state[6];
  state[6] = state[14];
  state[14] = temp;

  temp = state[3];
  state[3] = state[7];
  state[7] = state[11];
  state[11] = state[15];
  state[15] = temp;
}

void AES256::mix_columns(uint8_t* state) {
  for (int i = 0; i < 4; i++) {
    uint8_t a[4];
    uint8_t b[4];
    for (int j = 0; j < 4; j++) {
      a[j] = state[i * 4 + j];
      b[j] = (a[j] << 1) ^ ((a[j] & 0x80) ? 0x1b : 0);
    }
    state[i * 4 + 0] = b[0] ^ a[1] ^ b[1] ^ a[2] ^ a[3];
    state[i * 4 + 1] = a[0] ^ b[1] ^ a[2] ^ b[2] ^ a[3];
    state[i * 4 + 2] = a[0] ^ a[1] ^ b[2] ^ a[3] ^ b[3];
    state[i * 4 + 3] = a[0] ^ b[0] ^ a[1] ^ a[2] ^ b[3];
  }
}

void AES256::inv_mix_columns(uint8_t* state) {
  for (int i = 0; i < 4; i++) {
    uint8_t a[4];
    for (int j = 0; j < 4; j++) {
      a[j] = state[i * 4 + j];
    }
    state[i * 4 + 0] = aes_mul14(a[0]) ^ aes_mul11(a[1]) ^ aes_mul13(a[2]) ^ aes_mul9(a[3]);
    state[i * 4 + 1] = aes_mul9(a[0]) ^ aes_mul14(a[1]) ^ aes_mul11(a[2]) ^ aes_mul13(a[3]);
    state[i * 4 + 2] = aes_mul13(a[0]) ^ aes_mul9(a[1]) ^ aes_mul14(a[2]) ^ aes_mul11(a[3]);
    state[i * 4 + 3] = aes_mul11(a[0]) ^ aes_mul13(a[1]) ^ aes_mul9(a[2]) ^ aes_mul14(a[3]);
  }
}

void AES256::add_round_key(uint8_t* state, const uint8_t* round_key) {
  for (int i = 0; i < 16; i++) {
    state[i] ^= round_key[i];
  }
}

void AES256::key_expansion(const uint8_t* key) {
  // Copy the 32-byte key as the first two round keys
  memcpy(round_keys[0], key, 16);
  memcpy(round_keys[1], key + 16, 16);

  // AES-256 key schedule: 8 words per key, generate words for rounds 2-14
  // We work in terms of 4-byte words; round_keys[i] holds words 4i..4i+3
  for (int i = 2; i < 15; i++) {
    for (int w = 0; w < 4; w++) {
      int word_idx = i * 4 + w;  // absolute word index
      // Previous word (word_idx - 1)
      uint8_t prev[4];
      int prev_round = (word_idx - 1) / 4;
      int prev_off = ((word_idx - 1) % 4) * 4;
      memcpy(prev, &round_keys[prev_round][prev_off], 4);

      if (word_idx % 8 == 0) {
        // RotWord
        uint8_t t = prev[0];
        prev[0] = prev[1];
        prev[1] = prev[2];
        prev[2] = prev[3];
        prev[3] = t;
        // SubWord
        for (int j = 0; j < 4; j++) {
          prev[j] = sbox[prev[j]];
        }
        // XOR with Rcon
        prev[0] ^= (rcon[(word_idx / 8) - 1] >> 24) & 0xFF;
      } else if (word_idx % 8 == 4) {
        // Extra SubWord for AES-256
        for (int j = 0; j < 4; j++) {
          prev[j] = sbox[prev[j]];
        }
      }

      // XOR with word (word_idx - 8)
      int back_round = (word_idx - 8) / 4;
      int back_off = ((word_idx - 8) % 4) * 4;
      for (int j = 0; j < 4; j++) {
        round_keys[i][w * 4 + j] = round_keys[back_round][back_off + j] ^ prev[j];
      }
    }
  }
}

void AES256::set_key(const uint8_t* key) {
  key_expansion(key);
}

void AES256::encrypt_block(const uint8_t* in, uint8_t* out) {
  uint8_t state[16];
  memcpy(state, in, 16);

  add_round_key(state, round_keys[0]);

  for (int round = 1; round < 14; round++) {
    sub_bytes(state);
    shift_rows(state);
    mix_columns(state);
    add_round_key(state, round_keys[round]);
  }

  sub_bytes(state);
  shift_rows(state);
  add_round_key(state, round_keys[14]);

  memcpy(out, state, 16);
}

void AES256::decrypt_block(const uint8_t* in, uint8_t* out) {
  uint8_t state[16];
  memcpy(state, in, 16);

  add_round_key(state, round_keys[14]);

  for (int round = 13; round > 0; round--) {
    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, round_keys[round]);
    inv_mix_columns(state);
  }

  inv_shift_rows(state);
  inv_sub_bytes(state);
  add_round_key(state, round_keys[0]);

  memcpy(out, state, 16);
}

void AES256::encrypt_cbc(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv) {
  uint8_t prev_block[16];
  memcpy(prev_block, iv, 16);

  for (size_t i = 0; i < len; i += 16) {
    uint8_t block[16];
    memcpy(block, &in[i], 16);

    for (int j = 0; j < 16; j++) {
      block[j] ^= prev_block[j];
    }

    encrypt_block(block, &out[i]);
    memcpy(prev_block, &out[i], 16);
  }
}

void AES256::decrypt_cbc(const uint8_t* in, uint8_t* out, size_t len, const uint8_t* iv) {
  uint8_t prev_block[16];
  memcpy(prev_block, iv, 16);

  for (size_t i = 0; i < len; i += 16) {
    uint8_t block[16];
    decrypt_block(&in[i], block);

    for (int j = 0; j < 16; j++) {
      out[i + j] = block[j] ^ prev_block[j];
    }

    memcpy(prev_block, &in[i], 16);
  }
}

// Utility functions
void xor_bytes(uint8_t* dest, const uint8_t* src, size_t len) {
  for (size_t i = 0; i < len; i++) {
    dest[i] ^= src[i];
  }
}

void pad_pkcs7(std::vector<uint8_t>& data, size_t block_size) {
  size_t padding_len = block_size - (data.size() % block_size);
  for (size_t i = 0; i < padding_len; i++) {
    data.push_back(static_cast<uint8_t>(padding_len));
  }
}

size_t unpad_pkcs7(uint8_t* data, size_t len) {
  if (len == 0) {
    return 0;
  }
  uint8_t padding_len = data[len - 1];
  if (padding_len > len || padding_len == 0 || padding_len > 16) {
    return len;  // Invalid padding
  }
  for (size_t i = 1; i <= padding_len; i++) {
    if (data[len - i] != padding_len) {
      return len;  // Invalid padding
    }
  }
  return len - padding_len;
}

} // namespace crypto
} // namespace nanopdf
