/* SPDX-License-Identifier: Apache-2.0
 * HMAC, HKDF (incl. TLS 1.3 HKDF-Expand-Label) and PBKDF2 over the ncrypto
 * SHA family. Ported from nanopdf src/crypto.cc and src/tls-crypto.cc. */
#include "ncrypto/nc_kdf.h"

#include <string.h>

#include "ncrypto/nc_hash.h"

#define NC_MAX_DIGEST 64
#define NC_MAX_BLOCK 128

size_t nc_prf_len(nc_prf prf) {
  switch (prf) {
    case NC_PRF_SHA1:
      return NC_SHA1_LEN;
    case NC_PRF_SHA384:
      return NC_SHA384_LEN;
    case NC_PRF_SHA512:
      return NC_SHA512_LEN;
    case NC_PRF_SHA256:
    default:
      return NC_SHA256_LEN;
  }
}

static size_t nc_prf_block(nc_prf prf) {
  return (prf == NC_PRF_SHA384 || prf == NC_PRF_SHA512) ? 128 : 64;
}

static void nc_prf_hash(nc_prf prf, const uint8_t* data, size_t len,
                        uint8_t* out) {
  switch (prf) {
    case NC_PRF_SHA1:
      nc_sha1(data, len, out);
      break;
    case NC_PRF_SHA384:
      nc_sha384(data, len, out);
      break;
    case NC_PRF_SHA512:
      nc_sha512(data, len, out);
      break;
    case NC_PRF_SHA256:
    default:
      nc_sha256(data, len, out);
      break;
  }
}

/* Streaming hash over (a||b) without allocation, used by HMAC. */
static void nc_prf_hash2(nc_prf prf, const uint8_t* a, size_t alen,
                         const uint8_t* b, size_t blen, uint8_t* out) {
  switch (prf) {
    case NC_PRF_SHA1: {
      nc_sha1_ctx c;
      nc_sha1_init(&c);
      nc_sha1_update(&c, a, alen);
      nc_sha1_update(&c, b, blen);
      nc_sha1_final(&c, out);
      break;
    }
    case NC_PRF_SHA384: {
      nc_sha512_ctx c;
      nc_sha384_init(&c);
      nc_sha384_update(&c, a, alen);
      nc_sha384_update(&c, b, blen);
      nc_sha384_final(&c, out);
      break;
    }
    case NC_PRF_SHA512: {
      nc_sha512_ctx c;
      nc_sha512_init(&c);
      nc_sha512_update(&c, a, alen);
      nc_sha512_update(&c, b, blen);
      nc_sha512_final(&c, out);
      break;
    }
    case NC_PRF_SHA256:
    default: {
      nc_sha256_ctx c;
      nc_sha256_init(&c);
      nc_sha256_update(&c, a, alen);
      nc_sha256_update(&c, b, blen);
      nc_sha256_final(&c, out);
      break;
    }
  }
}

size_t nc_hmac(nc_prf prf, const uint8_t* key, size_t keylen,
               const uint8_t* msg, size_t msglen, uint8_t* out) {
  const size_t B = nc_prf_block(prf);
  const size_t L = nc_prf_len(prf);
  uint8_t k0[NC_MAX_BLOCK];
  uint8_t ipad[NC_MAX_BLOCK];
  uint8_t opad[NC_MAX_BLOCK];
  uint8_t ih[NC_MAX_DIGEST];
  size_t i;

  memset(k0, 0, B);
  if (keylen > B) {
    nc_prf_hash(prf, key, keylen, k0); /* K0 = H(key), zero-padded */
  } else if (keylen > 0) {
    memcpy(k0, key, keylen);
  }

  for (i = 0; i < B; i++) {
    ipad[i] = k0[i] ^ 0x36;
    opad[i] = k0[i] ^ 0x5c;
  }

  /* inner = H((K0 ^ ipad) || msg) */
  nc_prf_hash2(prf, ipad, B, msg, msglen, ih);
  /* out = H((K0 ^ opad) || inner) */
  nc_prf_hash2(prf, opad, B, ih, L, out);

  return L;
}

size_t nc_hkdf_extract(nc_prf prf, const uint8_t* salt, size_t saltlen,
                       const uint8_t* ikm, size_t ikmlen, uint8_t* prk) {
  const size_t L = nc_prf_len(prf);
  uint8_t zero[NC_MAX_DIGEST];
  if (salt == NULL || saltlen == 0) {
    memset(zero, 0, L);
    salt = zero;
    saltlen = L;
  }
  return nc_hmac(prf, salt, saltlen, ikm, ikmlen, prk);
}

int nc_hkdf_expand(nc_prf prf, const uint8_t* prk, size_t prklen,
                   const uint8_t* info, size_t infolen, uint8_t* out,
                   size_t outlen) {
  const size_t L = nc_prf_len(prf);
  uint8_t t[NC_MAX_DIGEST];
  size_t tlen = 0;
  size_t done = 0;
  uint8_t counter = 1;

  while (done < outlen) {
    /* T(i) = HMAC(prk, T(i-1) || info || counter) */
    uint8_t blk[NC_MAX_DIGEST + 4096 + 1];
    size_t pos = 0;
    size_t take;

    if (infolen > 4096) return 1; /* keep the on-stack buffer bounded */

    if (tlen > 0) {
      memcpy(blk, t, tlen);
      pos += tlen;
    }
    if (infolen > 0) {
      memcpy(blk + pos, info, infolen);
      pos += infolen;
    }
    blk[pos++] = counter;

    nc_hmac(prf, prk, prklen, blk, pos, t);
    tlen = L;

    take = (outlen - done) < L ? (outlen - done) : L;
    memcpy(out + done, t, take);
    done += take;
    counter++;
  }
  return 0;
}

int nc_hkdf_expand_label(nc_prf prf, const uint8_t* secret, size_t secretlen,
                         const char* label, const uint8_t* context,
                         size_t contextlen, uint8_t* out, size_t outlen) {
  uint8_t info[2 + 1 + 255 + 1 + 255];
  size_t pos = 0;
  size_t labellen = label ? strlen(label) : 0;
  size_t fulllen = 6 + labellen; /* "tls13 " prefix */

  if (fulllen > 255 || contextlen > 255) return 1;

  info[pos++] = (uint8_t)((outlen >> 8) & 0xff);
  info[pos++] = (uint8_t)(outlen & 0xff);
  info[pos++] = (uint8_t)fulllen;
  memcpy(info + pos, "tls13 ", 6);
  pos += 6;
  if (labellen > 0) {
    memcpy(info + pos, label, labellen);
    pos += labellen;
  }
  info[pos++] = (uint8_t)contextlen;
  if (contextlen > 0) {
    memcpy(info + pos, context, contextlen);
    pos += contextlen;
  }

  return nc_hkdf_expand(prf, secret, secretlen, info, pos, out, outlen);
}

int nc_pbkdf2(nc_prf prf, const uint8_t* pass, size_t passlen,
              const uint8_t* salt, size_t saltlen, uint32_t iterations,
              uint8_t* out, size_t outlen) {
  const size_t L = nc_prf_len(prf);
  uint32_t block = 1;
  size_t done = 0;

  if (iterations == 0) return 1;
  if (saltlen > 4096) return 1; /* bound the stack buffer below */

  while (done < outlen) {
    uint8_t saltblk[4096 + 4];
    uint8_t u[NC_MAX_DIGEST];
    uint8_t f[NC_MAX_DIGEST];
    size_t take;
    uint32_t it;
    size_t j;

    memcpy(saltblk, salt, saltlen);
    saltblk[saltlen] = (uint8_t)(block >> 24);
    saltblk[saltlen + 1] = (uint8_t)(block >> 16);
    saltblk[saltlen + 2] = (uint8_t)(block >> 8);
    saltblk[saltlen + 3] = (uint8_t)(block);

    nc_hmac(prf, pass, passlen, saltblk, saltlen + 4, u);
    memcpy(f, u, L);

    for (it = 1; it < iterations; it++) {
      nc_hmac(prf, pass, passlen, u, L, u);
      for (j = 0; j < L; j++) f[j] ^= u[j];
    }

    take = (outlen - done) < L ? (outlen - done) : L;
    memcpy(out + done, f, take);
    done += take;
    block++;
  }
  return 0;
}
