#include "nanopdf_crypto.h"

#include <string.h>

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | ~(z)))
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define STEP(f, a, b, c, d, x, t, s) \
  (a) += f((b), (c), (d)) + (x) + (uint32_t)(t); \
  (a) = ROTL((a), (s)); \
  (a) += (b)

#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (ROTR32((x), 2) ^ ROTR32((x), 13) ^ ROTR32((x), 22))
#define SHA256_EP1(x) (ROTR32((x), 6) ^ ROTR32((x), 11) ^ ROTR32((x), 25))
#define SHA256_SIG0(x) (ROTR32((x), 7) ^ ROTR32((x), 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (ROTR32((x), 17) ^ ROTR32((x), 19) ^ ((x) >> 10))
#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define SHA512_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA512_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA512_EP0(x) (ROTR64((x), 28) ^ ROTR64((x), 34) ^ ROTR64((x), 39))
#define SHA512_EP1(x) (ROTR64((x), 14) ^ ROTR64((x), 18) ^ ROTR64((x), 41))
#define SHA512_SIG0(x) (ROTR64((x), 1) ^ ROTR64((x), 8) ^ ((x) >> 7))
#define SHA512_SIG1(x) (ROTR64((x), 19) ^ ROTR64((x), 61) ^ ((x) >> 6))

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t x[16];
  int i = 0;

  for (i = 0; i < 16; ++i) {
    x[i] = (uint32_t)block[i * 4] |
           ((uint32_t)block[i * 4 + 1] << 8) |
           ((uint32_t)block[i * 4 + 2] << 16) |
           ((uint32_t)block[i * 4 + 3] << 24);
  }

  STEP(F, a, b, c, d, x[0], 0xd76aa478, 7);
  STEP(F, d, a, b, c, x[1], 0xe8c7b756, 12);
  STEP(F, c, d, a, b, x[2], 0x242070db, 17);
  STEP(F, b, c, d, a, x[3], 0xc1bdceee, 22);
  STEP(F, a, b, c, d, x[4], 0xf57c0faf, 7);
  STEP(F, d, a, b, c, x[5], 0x4787c62a, 12);
  STEP(F, c, d, a, b, x[6], 0xa8304613, 17);
  STEP(F, b, c, d, a, x[7], 0xfd469501, 22);
  STEP(F, a, b, c, d, x[8], 0x698098d8, 7);
  STEP(F, d, a, b, c, x[9], 0x8b44f7af, 12);
  STEP(F, c, d, a, b, x[10], 0xffff5bb1, 17);
  STEP(F, b, c, d, a, x[11], 0x895cd7be, 22);
  STEP(F, a, b, c, d, x[12], 0x6b901122, 7);
  STEP(F, d, a, b, c, x[13], 0xfd987193, 12);
  STEP(F, c, d, a, b, x[14], 0xa679438e, 17);
  STEP(F, b, c, d, a, x[15], 0x49b40821, 22);

  STEP(G, a, b, c, d, x[1], 0xf61e2562, 5);
  STEP(G, d, a, b, c, x[6], 0xc040b340, 9);
  STEP(G, c, d, a, b, x[11], 0x265e5a51, 14);
  STEP(G, b, c, d, a, x[0], 0xe9b6c7aa, 20);
  STEP(G, a, b, c, d, x[5], 0xd62f105d, 5);
  STEP(G, d, a, b, c, x[10], 0x02441453, 9);
  STEP(G, c, d, a, b, x[15], 0xd8a1e681, 14);
  STEP(G, b, c, d, a, x[4], 0xe7d3fbc8, 20);
  STEP(G, a, b, c, d, x[9], 0x21e1cde6, 5);
  STEP(G, d, a, b, c, x[14], 0xc33707d6, 9);
  STEP(G, c, d, a, b, x[3], 0xf4d50d87, 14);
  STEP(G, b, c, d, a, x[8], 0x455a14ed, 20);
  STEP(G, a, b, c, d, x[13], 0xa9e3e905, 5);
  STEP(G, d, a, b, c, x[2], 0xfcefa3f8, 9);
  STEP(G, c, d, a, b, x[7], 0x676f02d9, 14);
  STEP(G, b, c, d, a, x[12], 0x8d2a4c8a, 20);

  STEP(H, a, b, c, d, x[5], 0xfffa3942, 4);
  STEP(H, d, a, b, c, x[8], 0x8771f681, 11);
  STEP(H, c, d, a, b, x[11], 0x6d9d6122, 16);
  STEP(H, b, c, d, a, x[14], 0xfde5380c, 23);
  STEP(H, a, b, c, d, x[1], 0xa4beea44, 4);
  STEP(H, d, a, b, c, x[4], 0x4bdecfa9, 11);
  STEP(H, c, d, a, b, x[7], 0xf6bb4b60, 16);
  STEP(H, b, c, d, a, x[10], 0xbebfbc70, 23);
  STEP(H, a, b, c, d, x[13], 0x289b7ec6, 4);
  STEP(H, d, a, b, c, x[0], 0xeaa127fa, 11);
  STEP(H, c, d, a, b, x[3], 0xd4ef3085, 16);
  STEP(H, b, c, d, a, x[6], 0x04881d05, 23);
  STEP(H, a, b, c, d, x[9], 0xd9d4d039, 4);
  STEP(H, d, a, b, c, x[12], 0xe6db99e5, 11);
  STEP(H, c, d, a, b, x[15], 0x1fa27cf8, 16);
  STEP(H, b, c, d, a, x[2], 0xc4ac5665, 23);

  STEP(I, a, b, c, d, x[0], 0xf4292244, 6);
  STEP(I, d, a, b, c, x[7], 0x432aff97, 10);
  STEP(I, c, d, a, b, x[14], 0xab9423a7, 15);
  STEP(I, b, c, d, a, x[5], 0xfc93a039, 21);
  STEP(I, a, b, c, d, x[12], 0x655b59c3, 6);
  STEP(I, d, a, b, c, x[3], 0x8f0ccc92, 10);
  STEP(I, c, d, a, b, x[10], 0xffeff47d, 15);
  STEP(I, b, c, d, a, x[1], 0x85845dd1, 21);
  STEP(I, a, b, c, d, x[8], 0x6fa87e4f, 6);
  STEP(I, d, a, b, c, x[15], 0xfe2ce6e0, 10);
  STEP(I, c, d, a, b, x[6], 0xa3014314, 15);
  STEP(I, b, c, d, a, x[13], 0x4e0811a1, 21);
  STEP(I, a, b, c, d, x[4], 0xf7537e82, 6);
  STEP(I, d, a, b, c, x[11], 0xbd3af235, 10);
  STEP(I, c, d, a, b, x[2], 0x2ad7d2bb, 15);
  STEP(I, b, c, d, a, x[9], 0xeb86d391, 21);

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

static const uint32_t k_sha256_initial_state[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

static const uint32_t k_sha256_round_constants[64] = {
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

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];
  int i = 0;

  for (i = 0; i < 16; ++i) {
    w[i] = ((uint32_t)block[i * 4] << 24) |
           ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) |
           (uint32_t)block[i * 4 + 3];
  }
  for (i = 16; i < 64; ++i) {
    w[i] = SHA256_SIG1(w[i - 2]) + w[i - 7] + SHA256_SIG0(w[i - 15]) + w[i - 16];
  }
  for (i = 0; i < 64; ++i) {
    uint32_t t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) +
                  k_sha256_round_constants[i] + w[i];
    uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
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

void nanopdf_sha256_hash(const uint8_t* data, size_t len, uint8_t out[32]) {
  uint32_t state[8];
  uint8_t buffer[64];
  uint64_t bit_count = (uint64_t)len * 8u;
  size_t full_blocks = len / 64;
  size_t tail = len % 64;
  size_t i = 0;

  memcpy(state, k_sha256_initial_state, sizeof(state));
  for (i = 0; i < full_blocks; ++i) {
    sha256_transform(state, data + i * 64);
  }

  memset(buffer, 0, sizeof(buffer));
  if (tail > 0) {
    memcpy(buffer, data + full_blocks * 64, tail);
  }
  buffer[tail] = 0x80;
  if (tail >= 56) {
    sha256_transform(state, buffer);
    memset(buffer, 0, sizeof(buffer));
  }
  for (i = 0; i < 8; ++i) {
    buffer[56 + i] = (uint8_t)((bit_count >> ((7 - i) * 8)) & 0xff);
  }
  sha256_transform(state, buffer);

  for (i = 0; i < 8; ++i) {
    out[i * 4] = (uint8_t)((state[i] >> 24) & 0xff);
    out[i * 4 + 1] = (uint8_t)((state[i] >> 16) & 0xff);
    out[i * 4 + 2] = (uint8_t)((state[i] >> 8) & 0xff);
    out[i * 4 + 3] = (uint8_t)(state[i] & 0xff);
  }
}

static const uint64_t k_sha384_initial_state[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
    0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
    0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};

static const uint64_t k_sha512_initial_state[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

static const uint64_t k_sha512_round_constants[80] = {
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

static void sha512_transform(uint64_t state[8], const uint8_t block[128]) {
  uint64_t w[80];
  uint64_t a = state[0];
  uint64_t b = state[1];
  uint64_t c = state[2];
  uint64_t d = state[3];
  uint64_t e = state[4];
  uint64_t f = state[5];
  uint64_t g = state[6];
  uint64_t h = state[7];
  int i = 0;

  for (i = 0; i < 16; ++i) {
    w[i] = ((uint64_t)block[i * 8] << 56) |
           ((uint64_t)block[i * 8 + 1] << 48) |
           ((uint64_t)block[i * 8 + 2] << 40) |
           ((uint64_t)block[i * 8 + 3] << 32) |
           ((uint64_t)block[i * 8 + 4] << 24) |
           ((uint64_t)block[i * 8 + 5] << 16) |
           ((uint64_t)block[i * 8 + 6] << 8) |
           (uint64_t)block[i * 8 + 7];
  }
  for (i = 16; i < 80; ++i) {
    w[i] = SHA512_SIG1(w[i - 2]) + w[i - 7] + SHA512_SIG0(w[i - 15]) + w[i - 16];
  }
  for (i = 0; i < 80; ++i) {
    uint64_t t1 = h + SHA512_EP1(e) + SHA512_CH(e, f, g) +
                  k_sha512_round_constants[i] + w[i];
    uint64_t t2 = SHA512_EP0(a) + SHA512_MAJ(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
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

static void sha512_family_hash(
    const uint8_t* data,
    size_t len,
    const uint64_t initial_state[8],
    uint8_t* out,
    size_t out_len) {
  uint64_t state[8];
  uint8_t buffer[128];
  uint64_t bit_count = (uint64_t)len * 8u;
  size_t full_blocks = len / 128;
  size_t tail = len % 128;
  size_t i = 0;

  memcpy(state, initial_state, sizeof(state));
  for (i = 0; i < full_blocks; ++i) {
    sha512_transform(state, data + i * 128);
  }

  memset(buffer, 0, sizeof(buffer));
  if (tail > 0) {
    memcpy(buffer, data + full_blocks * 128, tail);
  }
  buffer[tail] = 0x80;
  if (tail >= 112) {
    sha512_transform(state, buffer);
    memset(buffer, 0, sizeof(buffer));
  }
  for (i = 0; i < 8; ++i) {
    buffer[120 + i] = (uint8_t)((bit_count >> ((7 - i) * 8)) & 0xff);
  }
  sha512_transform(state, buffer);

  for (i = 0; i < out_len; ++i) {
    out[i] = (uint8_t)((state[i / 8] >> ((7 - (i % 8)) * 8)) & 0xff);
  }
}

void nanopdf_sha384_hash(const uint8_t* data, size_t len, uint8_t out[48]) {
  sha512_family_hash(data, len, k_sha384_initial_state, out, 48);
}

void nanopdf_sha512_hash(const uint8_t* data, size_t len, uint8_t out[64]) {
  sha512_family_hash(data, len, k_sha512_initial_state, out, 64);
}

void nanopdf_md5_init(nanopdf_md5* md5) {
  md5->count = 0;
  md5->state[0] = 0x67452301u;
  md5->state[1] = 0xefcdab89u;
  md5->state[2] = 0x98badcfeu;
  md5->state[3] = 0x10325476u;
}

void nanopdf_md5_update(nanopdf_md5* md5, const uint8_t* data, size_t len) {
  size_t index = (size_t)((md5->count >> 3) & 0x3f);
  size_t part_len = 64 - index;
  size_t i = 0;

  md5->count += ((uint64_t)len << 3);
  if (len >= part_len) {
    memcpy(md5->buffer + index, data, part_len);
    md5_transform(md5->state, md5->buffer);
    for (i = part_len; i + 63 < len; i += 64) {
      md5_transform(md5->state, data + i);
    }
    index = 0;
  }
  memcpy(md5->buffer + index, data + i, len - i);
}

void nanopdf_md5_final(nanopdf_md5* md5, uint8_t out[16]) {
  static const uint8_t padding[64] = {0x80};
  uint8_t bits[8];
  size_t index = 0;
  size_t pad_len = 0;
  int i = 0;

  for (i = 0; i < 8; ++i) {
    bits[i] = (uint8_t)((md5->count >> (i * 8)) & 0xff);
  }
  index = (size_t)((md5->count >> 3) & 0x3f);
  pad_len = (index < 56) ? (56 - index) : (120 - index);
  nanopdf_md5_update(md5, padding, pad_len);
  nanopdf_md5_update(md5, bits, 8);

  for (i = 0; i < 4; ++i) {
    out[i * 4] = (uint8_t)(md5->state[i] & 0xff);
    out[i * 4 + 1] = (uint8_t)((md5->state[i] >> 8) & 0xff);
    out[i * 4 + 2] = (uint8_t)((md5->state[i] >> 16) & 0xff);
    out[i * 4 + 3] = (uint8_t)((md5->state[i] >> 24) & 0xff);
  }
}

void nanopdf_md5_hash(const uint8_t* data, size_t len, uint8_t out[16]) {
  nanopdf_md5 md5;
  nanopdf_md5_init(&md5);
  nanopdf_md5_update(&md5, data, len);
  nanopdf_md5_final(&md5, out);
}

void nanopdf_rc4_init(nanopdf_rc4* rc4, const uint8_t* key, size_t key_len) {
  int i = 0;
  uint8_t j = 0;
  for (i = 0; i < 256; ++i) {
    rc4->s[i] = (uint8_t)i;
  }
  rc4->i = 0;
  rc4->j = 0;
  for (i = 0; i < 256; ++i) {
    uint8_t tmp = 0;
    j = (uint8_t)(j + rc4->s[i] + key[i % key_len]);
    tmp = rc4->s[i];
    rc4->s[i] = rc4->s[j];
    rc4->s[j] = tmp;
  }
}

void nanopdf_rc4_crypt(nanopdf_rc4* rc4, uint8_t* data, size_t len) {
  size_t k = 0;
  for (k = 0; k < len; ++k) {
    uint8_t tmp = 0;
    rc4->i = (uint8_t)(rc4->i + 1);
    rc4->j = (uint8_t)(rc4->j + rc4->s[rc4->i]);
    tmp = rc4->s[rc4->i];
    rc4->s[rc4->i] = rc4->s[rc4->j];
    rc4->s[rc4->j] = tmp;
    data[k] ^= rc4->s[(uint8_t)(rc4->s[rc4->i] + rc4->s[rc4->j])];
  }
}

static const uint8_t k_aes_sbox[256] = {
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

static const uint8_t k_aes_inv_sbox[256] = {
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

static const uint32_t k_aes_rcon[10] = {
    0x01000000u, 0x02000000u, 0x04000000u, 0x08000000u, 0x10000000u,
    0x20000000u, 0x40000000u, 0x80000000u, 0x1b000000u, 0x36000000u};

static uint32_t aes_sub_word(uint32_t word) {
  return ((uint32_t)k_aes_sbox[(word >> 24) & 0xff] << 24) |
         ((uint32_t)k_aes_sbox[(word >> 16) & 0xff] << 16) |
         ((uint32_t)k_aes_sbox[(word >> 8) & 0xff] << 8) |
         (uint32_t)k_aes_sbox[word & 0xff];
}

static uint32_t aes_rot_word(uint32_t word) {
  return (word << 8) | (word >> 24);
}

static uint8_t aes128_gmul(uint8_t a, uint8_t b) {
  uint8_t p = 0;
  int i = 0;
  for (i = 0; i < 8; ++i) {
    if (b & 1) {
      p ^= a;
    }
    {
      uint8_t hi_bit = (uint8_t)(a & 0x80);
      a <<= 1;
      if (hi_bit) {
        a ^= 0x1b;
      }
    }
    b >>= 1;
  }
  return p;
}

static void aes128_add_round_key(uint8_t* state, const uint8_t* round_key) {
  int i = 0;
  for (i = 0; i < 16; ++i) {
    state[i] ^= round_key[i];
  }
}

static void aes128_sub_bytes(uint8_t* state) {
  int i = 0;
  for (i = 0; i < 16; ++i) {
    state[i] = k_aes_sbox[state[i]];
  }
}

static void aes128_inv_sub_bytes(uint8_t* state) {
  int i = 0;
  for (i = 0; i < 16; ++i) {
    state[i] = k_aes_inv_sbox[state[i]];
  }
}

static void aes128_shift_rows(uint8_t* state) {
  uint8_t temp = 0;

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

static void aes128_inv_shift_rows(uint8_t* state) {
  uint8_t temp = 0;

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

static void aes128_mix_columns(uint8_t* state) {
  int i = 0;
  for (i = 0; i < 4; ++i) {
    uint8_t a[4];
    uint8_t b[4];
    int j = 0;
    for (j = 0; j < 4; ++j) {
      a[j] = state[i * 4 + j];
      b[j] = (uint8_t)((a[j] << 1) ^ ((a[j] & 0x80) ? 0x1b : 0));
    }
    state[i * 4 + 0] = (uint8_t)(b[0] ^ a[1] ^ b[1] ^ a[2] ^ a[3]);
    state[i * 4 + 1] = (uint8_t)(a[0] ^ b[1] ^ a[2] ^ b[2] ^ a[3]);
    state[i * 4 + 2] = (uint8_t)(a[0] ^ a[1] ^ b[2] ^ a[3] ^ b[3]);
    state[i * 4 + 3] = (uint8_t)(a[0] ^ b[0] ^ a[1] ^ a[2] ^ b[3]);
  }
}

static void aes128_inv_mix_columns(uint8_t* state) {
  int i = 0;
  for (i = 0; i < 4; ++i) {
    uint8_t a[4];
    int j = 0;
    for (j = 0; j < 4; ++j) {
      a[j] = state[i * 4 + j];
    }
    state[i * 4 + 0] = (uint8_t)(
        aes128_gmul(a[0], 0x0e) ^ aes128_gmul(a[1], 0x0b) ^
        aes128_gmul(a[2], 0x0d) ^ aes128_gmul(a[3], 0x09));
    state[i * 4 + 1] = (uint8_t)(
        aes128_gmul(a[0], 0x09) ^ aes128_gmul(a[1], 0x0e) ^
        aes128_gmul(a[2], 0x0b) ^ aes128_gmul(a[3], 0x0d));
    state[i * 4 + 2] = (uint8_t)(
        aes128_gmul(a[0], 0x0d) ^ aes128_gmul(a[1], 0x09) ^
        aes128_gmul(a[2], 0x0e) ^ aes128_gmul(a[3], 0x0b));
    state[i * 4 + 3] = (uint8_t)(
        aes128_gmul(a[0], 0x0b) ^ aes128_gmul(a[1], 0x0d) ^
        aes128_gmul(a[2], 0x09) ^ aes128_gmul(a[3], 0x0e));
  }
}

static void aes128_encrypt_block(nanopdf_aes128* aes, const uint8_t* in, uint8_t* out) {
  uint8_t state[16];
  int round = 0;
  memcpy(state, in, 16);

  aes128_add_round_key(state, aes->round_keys[0]);
  for (round = 1; round < 10; ++round) {
    aes128_sub_bytes(state);
    aes128_shift_rows(state);
    aes128_mix_columns(state);
    aes128_add_round_key(state, aes->round_keys[round]);
  }
  aes128_sub_bytes(state);
  aes128_shift_rows(state);
  aes128_add_round_key(state, aes->round_keys[10]);
  memcpy(out, state, 16);
}

static void aes128_decrypt_block(nanopdf_aes128* aes, const uint8_t* in, uint8_t* out) {
  uint8_t state[16];
  int round = 0;
  memcpy(state, in, 16);

  aes128_add_round_key(state, aes->round_keys[10]);
  for (round = 9; round > 0; --round) {
    aes128_inv_shift_rows(state);
    aes128_inv_sub_bytes(state);
    aes128_add_round_key(state, aes->round_keys[round]);
    aes128_inv_mix_columns(state);
  }
  aes128_inv_shift_rows(state);
  aes128_inv_sub_bytes(state);
  aes128_add_round_key(state, aes->round_keys[0]);
  memcpy(out, state, 16);
}

void nanopdf_aes128_set_key(nanopdf_aes128* aes, const uint8_t* key) {
  int round = 0;

  memcpy(aes->round_keys[0], key, 16);
  for (round = 1; round < 11; ++round) {
    uint8_t temp[4];
    int i = 0;
    memcpy(temp, &aes->round_keys[round - 1][12], 4);
    {
      uint8_t saved = temp[0];
      temp[0] = temp[1];
      temp[1] = temp[2];
      temp[2] = temp[3];
      temp[3] = saved;
    }
    for (i = 0; i < 4; ++i) {
      temp[i] = k_aes_sbox[temp[i]];
    }
    temp[0] ^= (uint8_t)((k_aes_rcon[round - 1] >> 24) & 0xff);
    for (i = 0; i < 4; ++i) {
      aes->round_keys[round][i] =
          (uint8_t)(aes->round_keys[round - 1][i] ^ temp[i]);
    }
    for (i = 4; i < 16; ++i) {
      aes->round_keys[round][i] = (uint8_t)(
          aes->round_keys[round - 1][i] ^ aes->round_keys[round][i - 4]);
    }
  }
}

void nanopdf_aes128_encrypt_cbc(
    nanopdf_aes128* aes,
    const uint8_t* in,
    uint8_t* out,
    size_t len,
    const uint8_t iv[16]) {
  uint8_t prev[16];
  size_t offset = 0;
  memcpy(prev, iv, 16);

  while (offset < len) {
    uint8_t block[16];
    int i = 0;
    memcpy(block, in + offset, 16);
    for (i = 0; i < 16; ++i) {
      block[i] ^= prev[i];
    }
    aes128_encrypt_block(aes, block, out + offset);
    memcpy(prev, out + offset, 16);
    offset += 16;
  }
}

void nanopdf_aes128_decrypt_cbc(
    nanopdf_aes128* aes,
    const uint8_t* in,
    uint8_t* out,
    size_t len,
    const uint8_t iv[16]) {
  uint8_t prev[16];
  size_t offset = 0;
  memcpy(prev, iv, 16);

  while (offset < len) {
    uint8_t block[16];
    int i = 0;
    aes128_decrypt_block(aes, in + offset, block);
    for (i = 0; i < 16; ++i) {
      out[offset + i] = (uint8_t)(block[i] ^ prev[i]);
    }
    memcpy(prev, in + offset, 16);
    offset += 16;
  }
}

void nanopdf_aes256_set_key(nanopdf_aes256* aes, const uint8_t* key) {
  uint32_t words[60];
  int i = 0;

  for (i = 0; i < 8; ++i) {
    words[i] = ((uint32_t)key[i * 4] << 24) |
               ((uint32_t)key[i * 4 + 1] << 16) |
               ((uint32_t)key[i * 4 + 2] << 8) |
               (uint32_t)key[i * 4 + 3];
  }
  for (i = 8; i < 60; ++i) {
    uint32_t temp = words[i - 1];
    if ((i % 8) == 0) {
      temp = aes_sub_word(aes_rot_word(temp)) ^ k_aes_rcon[(i / 8) - 1];
    } else if ((i % 8) == 4) {
      temp = aes_sub_word(temp);
    }
    words[i] = words[i - 8] ^ temp;
  }

  for (i = 0; i < 15; ++i) {
    int j = 0;
    for (j = 0; j < 4; ++j) {
      uint32_t word = words[i * 4 + j];
      aes->round_keys[i][j * 4] = (uint8_t)((word >> 24) & 0xff);
      aes->round_keys[i][j * 4 + 1] = (uint8_t)((word >> 16) & 0xff);
      aes->round_keys[i][j * 4 + 2] = (uint8_t)((word >> 8) & 0xff);
      aes->round_keys[i][j * 4 + 3] = (uint8_t)(word & 0xff);
    }
  }
}

static void aes256_encrypt_block(nanopdf_aes256* aes, const uint8_t* in, uint8_t* out) {
  uint8_t state[16];
  int round = 0;
  memcpy(state, in, 16);

  aes128_add_round_key(state, aes->round_keys[0]);
  for (round = 1; round < 14; ++round) {
    aes128_sub_bytes(state);
    aes128_shift_rows(state);
    aes128_mix_columns(state);
    aes128_add_round_key(state, aes->round_keys[round]);
  }
  aes128_sub_bytes(state);
  aes128_shift_rows(state);
  aes128_add_round_key(state, aes->round_keys[14]);
  memcpy(out, state, 16);
}

static void aes256_decrypt_block(nanopdf_aes256* aes, const uint8_t* in, uint8_t* out) {
  uint8_t state[16];
  int round = 0;
  memcpy(state, in, 16);

  aes128_add_round_key(state, aes->round_keys[14]);
  for (round = 13; round > 0; --round) {
    aes128_inv_shift_rows(state);
    aes128_inv_sub_bytes(state);
    aes128_add_round_key(state, aes->round_keys[round]);
    aes128_inv_mix_columns(state);
  }
  aes128_inv_shift_rows(state);
  aes128_inv_sub_bytes(state);
  aes128_add_round_key(state, aes->round_keys[0]);
  memcpy(out, state, 16);
}

void nanopdf_aes256_encrypt_block(
    nanopdf_aes256* aes,
    const uint8_t* in,
    uint8_t* out) {
  aes256_encrypt_block(aes, in, out);
}

void nanopdf_aes256_encrypt_cbc(
    nanopdf_aes256* aes,
    const uint8_t* in,
    uint8_t* out,
    size_t len,
    const uint8_t iv[16]) {
  uint8_t prev[16];
  size_t offset = 0;
  memcpy(prev, iv, 16);

  while (offset < len) {
    uint8_t block[16];
    int i = 0;
    memcpy(block, in + offset, 16);
    for (i = 0; i < 16; ++i) {
      block[i] ^= prev[i];
    }
    aes256_encrypt_block(aes, block, out + offset);
    memcpy(prev, out + offset, 16);
    offset += 16;
  }
}

void nanopdf_aes256_decrypt_cbc(
    nanopdf_aes256* aes,
    const uint8_t* in,
    uint8_t* out,
    size_t len,
    const uint8_t iv[16]) {
  uint8_t prev[16];
  size_t offset = 0;
  memcpy(prev, iv, 16);

  while (offset < len) {
    uint8_t block[16];
    int i = 0;
    aes256_decrypt_block(aes, in + offset, block);
    for (i = 0; i < 16; ++i) {
      out[offset + i] = (uint8_t)(block[i] ^ prev[i]);
    }
    memcpy(prev, in + offset, 16);
    offset += 16;
  }
}

int nanopdf_pkcs7_unpad(
    const uint8_t* data,
    size_t len,
    size_t block_size,
    size_t* out_len) {
  uint8_t pad = 0;
  size_t i = 0;

  if (!data || !out_len || len == 0 || block_size == 0 || (len % block_size) != 0) {
    return 0;
  }

  pad = data[len - 1];
  if (pad == 0 || (size_t)pad > block_size || (size_t)pad > len) {
    return 0;
  }
  for (i = len - (size_t)pad; i < len; ++i) {
    if (data[i] != pad) {
      return 0;
    }
  }
  *out_len = len - (size_t)pad;
  return 1;
}
