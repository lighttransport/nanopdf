/* SPDX-License-Identifier: Apache-2.0
 * RSA: PKCS#1 v1.5 sign/verify, RSASSA-PSS verify, and private-key parsing
 * (PKCS#1 / PKCS#8, incl. PBES2-encrypted PKCS#8). Pure C11; ported from the
 * C++ reference in src/crypto-pk.cc. */
#include "ncrypto/nc_rsa.h"

#include <stdio.h>
#include <string.h>

#include "ncrypto/nc_aes.h"
#include "ncrypto/nc_hash.h"
#include "ncrypto/nc_kdf.h"

/* ---- minimal DER reader (for RSA key parsing) --------------------------- */

typedef struct {
  const uint8_t* p;
  const uint8_t* end;
  int ok;
} nc_der;

/* Read a tag+length; on success advance past the length and set the
 * content/clen out-params to the value bytes; returns the tag (0 on error). */
static uint8_t der_read_tl(nc_der* d, const uint8_t** content, size_t* clen) {
  if (!d->ok || d->p + 2 > d->end) {
    d->ok = 0;
    return 0;
  }
  uint8_t tag = *d->p++;
  size_t len = *d->p++;
  if (len & 0x80) {
    size_t nb = len & 0x7F;
    if (nb == 0 || nb > 4 || d->p + nb > d->end) {
      d->ok = 0;
      return 0;
    }
    len = 0;
    for (size_t i = 0; i < nb; ++i) len = (len << 8) | *d->p++;
  }
  if (d->p + len > d->end) {
    d->ok = 0;
    return 0;
  }
  *content = d->p;
  *clen = len;
  d->p += len;
  return tag;
}

/* Read an INTEGER into a bigint. On wrong tag, sets d->ok = 0. */
static void der_read_integer(nc_der* d, nc_bigint* out) {
  const uint8_t* c;
  size_t n;
  uint8_t tag = der_read_tl(d, &c, &n);
  if (tag != 0x02) {
    d->ok = 0;
    nc_bi_zero(out);
    return;
  }
  nc_bi_from_bytes(out, c, n);
}

/* PEM base64-decode the body between the given BEGIN/END labels. Writes up to
 * outcap bytes to out; returns the byte count, or 0 if not found. */
static size_t pem_to_der(const char* pem, const char* label, uint8_t* out,
                         size_t outcap) {
  char begin[64];
  char end[64];
  snprintf(begin, sizeof(begin), "-----BEGIN %s-----", label);
  snprintf(end, sizeof(end), "-----END %s-----", label);
  const char* b = strstr(pem, begin);
  if (!b) return 0;
  b += strlen(begin);
  const char* e = strstr(b, end);
  if (!e) return 0;

  static const char* B64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int dtab[256];
  for (int i = 0; i < 256; ++i) dtab[i] = -1;
  for (int i = 0; i < 64; ++i) dtab[(unsigned char)B64[i]] = i;

  size_t n = 0;
  int val = 0, bits = 0;
  for (const char* p = b; p < e; ++p) {
    int dv = dtab[(unsigned char)*p];
    if (dv < 0) continue; /* skip whitespace/newlines/padding */
    val = (val << 6) | dv;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n < outcap) out[n] = (uint8_t)((val >> bits) & 0xFF);
      ++n;
    }
  }
  if (n > outcap) return 0; /* overflow */
  return n;
}

/* ---- private key parsing ------------------------------------------------ */

int nc_rsa_parse_privkey_der(nc_rsa_privkey* key, const uint8_t* der,
                             size_t len) {
  memset(key, 0, sizeof(*key));
  nc_der d = {der, der + len, 1};
  const uint8_t* seq;
  size_t seqlen;
  if (der_read_tl(&d, &seq, &seqlen) != 0x30) return -1; /* outer SEQUENCE */
  nc_der inner = {seq, seq + seqlen, 1};

  /* Could be PKCS#1 RSAPrivateKey or PKCS#8 PrivateKeyInfo. Peek the first
   * INTEGER (version). For PKCS#8 it is followed by an AlgorithmIdentifier
   * SEQUENCE and an OCTET STRING wrapping the PKCS#1 key. */
  const uint8_t* save = inner.p;
  nc_bigint version;
  der_read_integer(&inner, &version);
  const uint8_t* c;
  size_t n;
  uint8_t next = der_read_tl(&inner, &c, &n);
  if (next == 0x30) {
    /* PKCS#8: skip AlgorithmIdentifier, read the OCTET STRING, recurse. */
    uint8_t t = der_read_tl(&inner, &c, &n);
    if (t != 0x04) return -1;
    return nc_rsa_parse_privkey_der(key, c, n);
  }

  /* PKCS#1: rewind and parse version, n, e, d, ... */
  nc_der p1 = {save, seq + seqlen, 1};
  nc_bigint tmp;
  der_read_integer(&p1, &tmp); /* version (0) */
  der_read_integer(&p1, &key->n);
  der_read_integer(&p1, &key->e);
  der_read_integer(&p1, &key->d);
  if (!p1.ok || nc_bi_is_zero(&key->n) || nc_bi_is_zero(&key->d)) {
    memset(key, 0, sizeof(*key));
    return -1;
  }
  key->modulus_bytes = (nc_bi_bitlen(&key->n) + 7) / 8;
  if (key->modulus_bytes > NC_RSA_MAX_MODULUS_BYTES) {
    memset(key, 0, sizeof(*key));
    return -1;
  }
  key->valid = 1;
  return 0;
}

int nc_rsa_parse_privkey_pem(nc_rsa_privkey* key, const char* pem,
                             const char* password) {
  uint8_t der[8192];
  size_t dlen;

  dlen = pem_to_der(pem, "RSA PRIVATE KEY", der, sizeof(der));
  if (dlen == 0) dlen = pem_to_der(pem, "PRIVATE KEY", der, sizeof(der));
  if (dlen != 0) return nc_rsa_parse_privkey_der(key, der, dlen);

  dlen = pem_to_der(pem, "ENCRYPTED PRIVATE KEY", der, sizeof(der));
  if (dlen == 0) {
    memset(key, 0, sizeof(*key));
    return -1;
  }
  uint8_t dec[8192];
  int declen = nc_pbes2_decrypt_pkcs8(der, dlen, password ? password : "", dec,
                                      sizeof(dec));
  if (declen < 0) {
    memset(key, 0, sizeof(*key));
    return -1;
  }
  return nc_rsa_parse_privkey_der(key, dec, (size_t)declen);
}

/* ---- PBES2 -------------------------------------------------------------- */

static size_t unpad_pkcs7(const uint8_t* buf, size_t len) {
  if (len == 0) return 0;
  uint8_t pad = buf[len - 1];
  if (pad == 0 || pad > 16 || pad > len) return 0;
  for (size_t i = 0; i < pad; ++i)
    if (buf[len - 1 - i] != pad) return 0;
  return len - pad;
}

int nc_pbes2_decrypt(const uint8_t* algid, size_t algidlen, const uint8_t* enc,
                     size_t enclen, const char* password, uint8_t* out,
                     size_t outcap) {
  nc_der alg = {algid, algid + algidlen, 1};
  const uint8_t* oid;
  size_t oidlen;
  if (der_read_tl(&alg, &oid, &oidlen) != 0x06) return -1;
  static const uint8_t PBES2[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                  0x0D, 0x01, 0x05, 0x0D};
  if (oidlen != sizeof(PBES2) || memcmp(oid, PBES2, oidlen) != 0) return -1;

  const uint8_t* pp;
  size_t pplen;
  if (der_read_tl(&alg, &pp, &pplen) != 0x30) return -1; /* PBES2-params */
  nc_der params = {pp, pp + pplen, 1};

  /* keyDerivationFunc: SEQUENCE { OID pbkdf2, PBKDF2-params } */
  const uint8_t* kdf;
  size_t kdflen;
  if (der_read_tl(&params, &kdf, &kdflen) != 0x30) return -1;
  nc_der kd = {kdf, kdf + kdflen, 1};
  const uint8_t* ko;
  size_t kolen;
  if (der_read_tl(&kd, &ko, &kolen) != 0x06) return -1; /* pbkdf2 OID */
  const uint8_t* kp;
  size_t kplen;
  if (der_read_tl(&kd, &kp, &kplen) != 0x30) return -1; /* PBKDF2-params */
  nc_der pk = {kp, kp + kplen, 1};
  const uint8_t* salt;
  size_t saltlen;
  if (der_read_tl(&pk, &salt, &saltlen) != 0x04) return -1; /* salt */
  const uint8_t* itc;
  size_t itclen;
  if (der_read_tl(&pk, &itc, &itclen) != 0x02) return -1; /* iterationCount */
  uint32_t iters = 0;
  for (size_t i = 0; i < itclen; ++i) iters = (iters << 8) | itc[i];
  if (iters == 0 || iters > 10000000u) return -1; /* bound CPU (DoS guard) */
  nc_prf prf = NC_PRF_SHA1; /* PBKDF2 default PRF */
  while (pk.p < pk.end && pk.ok) {
    /* optional keyLength (INTEGER) and prf (AlgorithmIdentifier SEQUENCE) */
    const uint8_t* x;
    size_t xl;
    uint8_t t = der_read_tl(&pk, &x, &xl);
    if (t == 0x30) {
      nc_der pd = {x, x + xl, 1};
      const uint8_t* po;
      size_t pol;
      if (der_read_tl(&pd, &po, &pol) == 0x06 && pol >= 1) {
        uint8_t last = po[pol - 1];
        if (last == 0x09)
          prf = NC_PRF_SHA256;
        else if (last == 0x0b)
          prf = NC_PRF_SHA512;
        else
          prf = NC_PRF_SHA1;
      }
    }
  }

  /* encryptionScheme: SEQUENCE { OID aesNNN-cbc, IV OCTET STRING } */
  const uint8_t* es;
  size_t eslen;
  if (der_read_tl(&params, &es, &eslen) != 0x30) return -1;
  nc_der ed = {es, es + eslen, 1};
  const uint8_t* eo;
  size_t eol;
  if (der_read_tl(&ed, &eo, &eol) != 0x06) return -1;
  size_t keylen = 0;
  if (eol >= 1) {
    uint8_t last = eo[eol - 1];
    if (last == 0x2A)
      keylen = 32; /* aes-256-cbc */
    else if (last == 0x16)
      keylen = 24; /* aes-192-cbc (unsupported below) */
    else if (last == 0x02)
      keylen = 16; /* aes-128-cbc */
  }
  if (keylen != 16 && keylen != 32) return -1; /* unsupported cipher */
  const uint8_t* iv;
  size_t ivlen;
  if (der_read_tl(&ed, &iv, &ivlen) != 0x04 || ivlen != 16) return -1;

  uint8_t key[32];
  if (nc_pbkdf2(prf, (const uint8_t*)password, strlen(password), salt, saltlen,
                iters, key, keylen) != 0)
    return -1;

  if (enclen == 0 || enclen % 16 != 0 || enclen > outcap) return -1;
  if (nc_aes_cbc_decrypt(key, keylen, iv, enc, enclen, out) != 0) return -1;

  size_t unpadded = unpad_pkcs7(out, enclen);
  if (unpadded == 0 || unpadded > enclen) return -1;
  return (int)unpadded;
}

int nc_pbes2_decrypt_pkcs8(const uint8_t* der, size_t len, const char* password,
                           uint8_t* out, size_t outcap) {
  nc_der d = {der, der + len, 1};
  const uint8_t* c;
  size_t n;
  if (der_read_tl(&d, &c, &n) != 0x30) return -1; /* EncryptedPrivateKeyInfo */
  nc_der epki = {c, c + n, 1};
  const uint8_t* ea;
  size_t ealen;
  if (der_read_tl(&epki, &ea, &ealen) != 0x30) return -1; /* encAlgorithm */
  const uint8_t* enc;
  size_t enclen;
  if (der_read_tl(&epki, &enc, &enclen) != 0x04) return -1; /* encryptedData */
  return nc_pbes2_decrypt(ea, ealen, enc, enclen, password, out, outcap);
}

/* ---- RSA PKCS#1 v1.5 ---------------------------------------------------- */

static const uint8_t kSha256DigestInfoPrefix[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48,
    0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

int nc_rsa_sign_pkcs1v15(const nc_rsa_privkey* key, const uint8_t* digest_info,
                         size_t di_len, uint8_t* sig) {
  if (!key->valid) return -1;
  size_t k = key->modulus_bytes;
  if (di_len + 11 > k) return -1; /* message too long for modulus */
  if (k > NC_RSA_MAX_MODULUS_BYTES) return -1;

  /* EM = 0x00 || 0x01 || PS(0xFF...) || 0x00 || DigestInfo */
  uint8_t em[NC_BIGINT_MAX_LIMBS * 4];
  memset(em, 0xFF, k);
  em[0] = 0x00;
  em[1] = 0x01;
  size_t ps_len = k - di_len - 3;
  em[2 + ps_len] = 0x00;
  memcpy(em + 3 + ps_len, digest_info, di_len);

  nc_bigint m, s;
  nc_bi_from_bytes(&m, em, k);
  nc_bi_modexp(&s, &m, &key->d, &key->n);
  if (nc_bi_to_bytes(&s, sig, k) < 0) return -1;
  return (int)k;
}

int nc_rsa_sign_sha256(const nc_rsa_privkey* key, const uint8_t hash[32],
                       uint8_t* sig) {
  uint8_t di[sizeof(kSha256DigestInfoPrefix) + 32];
  memcpy(di, kSha256DigestInfoPrefix, sizeof(kSha256DigestInfoPrefix));
  memcpy(di + sizeof(kSha256DigestInfoPrefix), hash, 32);
  return nc_rsa_sign_pkcs1v15(key, di, sizeof(di), sig);
}

int nc_rsa_verify_pkcs1v15(const nc_rsa_pubkey* key, const uint8_t* sig,
                           size_t sig_len, const uint8_t* digest_info,
                           size_t di_len) {
  if (!key->valid || sig_len != key->modulus_bytes) return 0;
  size_t k = key->modulus_bytes;
  if (di_len + 11 > k) return 0;
  if (k > NC_RSA_MAX_MODULUS_BYTES) return 0;

  nc_bigint s, m;
  nc_bi_from_bytes(&s, sig, sig_len);
  nc_bi_modexp(&m, &s, &key->e, &key->n);
  uint8_t em[NC_BIGINT_MAX_LIMBS * 4];
  if (nc_bi_to_bytes(&m, em, k) < 0) return 0;

  /* Rebuild the expected EM and compare. */
  uint8_t exp[NC_BIGINT_MAX_LIMBS * 4];
  memset(exp, 0xFF, k);
  exp[0] = 0x00;
  exp[1] = 0x01;
  size_t ps_len = k - di_len - 3;
  exp[2 + ps_len] = 0x00;
  memcpy(exp + 3 + ps_len, digest_info, di_len);
  return memcmp(em, exp, k) == 0 ? 1 : 0;
}

/* ---- RSASSA-PSS verify -------------------------------------------------- */

/* Hash @data with the algorithm selected by digest length (32/48/64). */
static int hash_by_len(const uint8_t* data, size_t len, size_t hlen,
                       uint8_t* out) {
  if (hlen == 32) {
    nc_sha256(data, len, out);
    return 1;
  }
  if (hlen == 48) {
    nc_sha384(data, len, out);
    return 1;
  }
  if (hlen == 64) {
    nc_sha512(data, len, out);
    return 1;
  }
  return 0;
}

/* MGF1 (RFC 8017 B.2.1) with the hlen-selected hash. Writes mask_len bytes. */
static int mgf1(const uint8_t* seed, size_t seed_len, uint8_t* mask,
                size_t mask_len, size_t hlen) {
  uint8_t buf[64 + 4];
  if (seed_len > 64) return -1;
  memcpy(buf, seed, seed_len);
  uint8_t digest[64];
  size_t done = 0;
  uint32_t counter = 0;
  while (done < mask_len) {
    buf[seed_len + 0] = (uint8_t)(counter >> 24);
    buf[seed_len + 1] = (uint8_t)(counter >> 16);
    buf[seed_len + 2] = (uint8_t)(counter >> 8);
    buf[seed_len + 3] = (uint8_t)(counter);
    if (!hash_by_len(buf, seed_len + 4, hlen, digest)) return -1;
    size_t take = hlen < mask_len - done ? hlen : mask_len - done;
    memcpy(mask + done, digest, take);
    done += take;
    ++counter;
  }
  return 0;
}

int nc_rsa_verify_pss(const nc_rsa_pubkey* key, const uint8_t* sig,
                      size_t sig_len, const uint8_t* mhash, size_t hlen) {
  if (!key->valid || sig_len != key->modulus_bytes) return 0;
  if (hlen != 32 && hlen != 48 && hlen != 64) return 0;

  size_t mod_bits = nc_bi_bitlen(&key->n);
  if (mod_bits == 0) return 0;
  size_t em_bits = mod_bits - 1;
  size_t em_len = (em_bits + 7) / 8;
  if (em_len < hlen + 2) return 0;
  if (em_len > NC_RSA_MAX_MODULUS_BYTES) return 0;

  nc_bigint s;
  nc_bi_from_bytes(&s, sig, sig_len);
  if (nc_bi_cmp(&s, &key->n) >= 0) return 0;
  nc_bigint m;
  nc_bi_modexp(&m, &s, &key->e, &key->n);
  uint8_t em[NC_BIGINT_MAX_LIMBS * 4];
  if (nc_bi_to_bytes(&m, em, em_len) < 0) return 0;

  /* EM = maskedDB || H || 0xbc */
  if (em[em_len - 1] != 0xbc) return 0;
  size_t db_len = em_len - hlen - 1;
  const uint8_t* masked_db = em;
  const uint8_t* H = em + db_len;

  /* The leftmost (8*emLen - emBits) bits of maskedDB must be zero. */
  size_t zero_bits = 8 * em_len - em_bits;
  if (zero_bits && (masked_db[0] & (uint8_t)~(0xFFu >> zero_bits))) return 0;

  /* DB = maskedDB XOR MGF1(H, db_len) */
  uint8_t db[NC_BIGINT_MAX_LIMBS * 4];
  if (mgf1(H, hlen, db, db_len, hlen) != 0) return 0;
  for (size_t i = 0; i < db_len; ++i) db[i] ^= masked_db[i];
  if (zero_bits) db[0] &= (uint8_t)(0xFF >> zero_bits);

  /* DB = PS(0x00..) || 0x01 || salt -- find the 0x01 separator. */
  size_t i = 0;
  while (i < db_len && db[i] == 0x00) ++i;
  if (i == db_len || db[i] != 0x01) return 0;
  ++i;
  const uint8_t* salt = db + i;
  size_t salt_len = db_len - i;

  /* M' = (0x00 * 8) || mHash || salt ; H' = Hash(M') ; compare to H. */
  uint8_t mprime[8 + 64 + NC_BIGINT_MAX_LIMBS * 4];
  memset(mprime, 0x00, 8);
  memcpy(mprime + 8, mhash, hlen);
  memcpy(mprime + 8 + hlen, salt, salt_len);
  uint8_t hprime[64];
  if (!hash_by_len(mprime, 8 + hlen + salt_len, hlen, hprime)) return 0;
  return memcmp(hprime, H, hlen) == 0 ? 1 : 0;
}
