/* SPDX-License-Identifier: Apache-2.0
 * X.509 certificate parsing + chain verification (C11). Ported from the
 * C++ reference (src/x509.cc). Self-contained DER reader (no nc_asn1.h). */

#define _DEFAULT_SOURCE /* timegm */
#define _GNU_SOURCE

#include "ncrypto/nc_x509.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ncrypto/nc_hash.h"

/* ---- minimal DER reader -------------------------------------------------- */
typedef struct {
  const uint8_t* p;
  const uint8_t* end;
  int ok;
} nc_der_reader;

/* Read tag+length; on success [*vbeg,*vend) is the value, r.p advances past. */
static uint8_t read_tl(nc_der_reader* r, const uint8_t** vbeg,
                       const uint8_t** vend) {
  uint8_t tag;
  size_t len;
  if (!r->ok || r->p + 2 > r->end) {
    r->ok = 0;
    return 0;
  }
  tag = *r->p++;
  len = *r->p++;
  if (len & 0x80) {
    size_t nb = len & 0x7F;
    size_t i;
    if (nb == 0 || nb > 4 || r->p + nb > r->end) {
      r->ok = 0;
      return 0;
    }
    len = 0;
    for (i = 0; i < nb; ++i) len = (len << 8) | *r->p++;
  }
  if (r->p + len > r->end) {
    r->ok = 0;
    return 0;
  }
  *vbeg = r->p;
  *vend = r->p + len;
  r->p += len;
  return tag;
}

static nc_der_reader der_enter(const uint8_t* b, const uint8_t* e) {
  nc_der_reader r;
  r.p = b;
  r.end = e;
  r.ok = 1;
  return r;
}

static int oid_eq(const uint8_t* b, const uint8_t* e, const uint8_t* oid,
                  size_t n) {
  return (size_t)(e - b) == n && memcmp(b, oid, n) == 0;
}

/* OID value bytes. */
static const uint8_t kRsaEncryption[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                         0x0D, 0x01, 0x01, 0x01};
static const uint8_t kRsaSha1[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                   0x0D, 0x01, 0x01, 0x05};
static const uint8_t kRsaSha256[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                     0x0D, 0x01, 0x01, 0x0B};
static const uint8_t kRsaSha384[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                     0x0D, 0x01, 0x01, 0x0C};
static const uint8_t kRsaSha512[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                     0x0D, 0x01, 0x01, 0x0D};
static const uint8_t kRsaPss[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                  0x0D, 0x01, 0x01, 0x0A};
static const uint8_t kEcPublicKey[] = {0x2A, 0x86, 0x48, 0xCE,
                                       0x3D, 0x02, 0x01};
static const uint8_t kCurveP256[] = {0x2A, 0x86, 0x48, 0xCE,
                                     0x3D, 0x03, 0x01, 0x07};
static const uint8_t kCurveP384[] = {0x2B, 0x81, 0x04, 0x00, 0x22};
static const uint8_t kEcdsaSha1[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x01};
static const uint8_t kEcdsaSha256[] = {0x2A, 0x86, 0x48, 0xCE,
                                       0x3D, 0x04, 0x03, 0x02};
static const uint8_t kEcdsaSha384[] = {0x2A, 0x86, 0x48, 0xCE,
                                       0x3D, 0x04, 0x03, 0x03};
static const uint8_t kEcdsaSha512[] = {0x2A, 0x86, 0x48, 0xCE,
                                       0x3D, 0x04, 0x03, 0x04};
static const uint8_t kHashSha384[] = {0x60, 0x86, 0x48, 0x01, 0x65,
                                      0x03, 0x04, 0x02, 0x02};
static const uint8_t kHashSha512[] = {0x60, 0x86, 0x48, 0x01, 0x65,
                                      0x03, 0x04, 0x02, 0x03};
static const uint8_t kOidSan[] = {0x55, 0x1D, 0x11};
static const uint8_t kOidBasicConstraints[] = {0x55, 0x1D, 0x13};

#define OIDEQ(b, e, arr) oid_eq((b), (e), (arr), sizeof(arr))

/* Parse the AlgorithmIdentifier at [b,e) into a nc_sig_alg. For RSA-PSS, also
 * recover the message-hash length. */
static nc_sig_alg parse_sig_alg(const uint8_t* b, const uint8_t* e,
                                size_t* pss_hlen) {
  nc_der_reader r = der_enter(b, e);
  const uint8_t *ob, *oe;
  if (read_tl(&r, &ob, &oe) != 0x06) return NC_SIG_UNKNOWN;
  if (OIDEQ(ob, oe, kRsaSha256)) return NC_SIG_RSA_PKCS1_SHA256;
  if (OIDEQ(ob, oe, kRsaSha384)) return NC_SIG_RSA_PKCS1_SHA384;
  if (OIDEQ(ob, oe, kRsaSha512)) return NC_SIG_RSA_PKCS1_SHA512;
  if (OIDEQ(ob, oe, kRsaSha1)) return NC_SIG_RSA_PKCS1_SHA1;
  if (OIDEQ(ob, oe, kEcdsaSha256)) return NC_SIG_ECDSA_SHA256;
  if (OIDEQ(ob, oe, kEcdsaSha384)) return NC_SIG_ECDSA_SHA384;
  if (OIDEQ(ob, oe, kEcdsaSha512)) return NC_SIG_ECDSA_SHA512;
  if (OIDEQ(ob, oe, kEcdsaSha1)) return NC_SIG_ECDSA_SHA1;
  if (OIDEQ(ob, oe, kRsaPss)) {
    const uint8_t *pb, *pe;
    *pss_hlen = 32; /* default SHA-256 if params absent */
    if (read_tl(&r, &pb, &pe) == 0x30) {
      nc_der_reader pr = der_enter(pb, pe);
      const uint8_t *cb, *ce;
      if (read_tl(&pr, &cb, &ce) == 0xA0) { /* [0] hashAlgorithm */
        nc_der_reader hr = der_enter(cb, ce);
        const uint8_t *hb, *he;
        if (read_tl(&hr, &hb, &he) == 0x30) {
          nc_der_reader ar = der_enter(hb, he);
          const uint8_t *hob, *hoe;
          if (read_tl(&ar, &hob, &hoe) == 0x06) {
            if (OIDEQ(hob, hoe, kHashSha384))
              *pss_hlen = 48;
            else if (OIDEQ(hob, hoe, kHashSha512))
              *pss_hlen = 64;
            else
              *pss_hlen = 32;
          }
        }
      }
    }
    return NC_SIG_RSA_PSS;
  }
  return NC_SIG_UNKNOWN;
}

/* ASN.1 time (UTCTime / GeneralizedTime) -> epoch seconds. 0 on failure. */
static int64_t parse_time(uint8_t tag, const uint8_t* b, const uint8_t* e) {
  char s[32];
  size_t slen = (size_t)(e - b);
  struct tm tmv;
  size_t i = 0;
  int year, mon, day, hour, min, sec = 0;
  if (slen >= sizeof(s)) slen = sizeof(s) - 1;
  memcpy(s, b, slen);
  s[slen] = 0;
  memset(&tmv, 0, sizeof(tmv));

#define TWO(out)                                              \
  do {                                                        \
    if (i + 1 >= slen) {                                      \
      (out) = -1;                                             \
    } else {                                                  \
      (out) = (s[i] - '0') * 10 + (s[i + 1] - '0');           \
      i += 2;                                                 \
    }                                                         \
  } while (0)

  if (tag == 0x17) { /* UTCTime YY... */
    int yy;
    TWO(yy);
    if (yy < 0) return 0;
    year = (yy < 50) ? 2000 + yy : 1900 + yy;
  } else if (tag == 0x18) { /* GeneralizedTime YYYY... */
    int hi, lo;
    TWO(hi);
    TWO(lo);
    if (hi < 0 || lo < 0) return 0;
    year = hi * 100 + lo;
  } else {
    return 0;
  }
  TWO(mon);
  TWO(day);
  TWO(hour);
  TWO(min);
  if (i + 1 < slen && s[i] >= '0' && s[i] <= '9') TWO(sec);
#undef TWO
  if (mon < 1 || day < 0 || hour < 0 || min < 0) return 0;
  tmv.tm_year = year - 1900;
  tmv.tm_mon = mon - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = min;
  tmv.tm_sec = sec;
  return (int64_t)timegm(&tmv);
}

/* Pull RSA (n,e) or EC point from a SubjectPublicKeyInfo value [b,e). */
static void parse_spki(const uint8_t* b, const uint8_t* e, nc_x509_cert* c) {
  nc_der_reader spki = der_enter(b, e);
  const uint8_t *ab, *ae;
  nc_der_reader alg;
  const uint8_t *ob, *oe;
  int is_rsa, is_ec;
  const nc_ec_curve* curve = NULL;
  const uint8_t *kb, *ke;
  const uint8_t* key;
  size_t key_len;
  if (read_tl(&spki, &ab, &ae) != 0x30) return; /* AlgorithmIdentifier */
  alg = der_enter(ab, ae);
  if (read_tl(&alg, &ob, &oe) != 0x06) return; /* OID */
  is_rsa = OIDEQ(ob, oe, kRsaEncryption);
  is_ec = OIDEQ(ob, oe, kEcPublicKey);
  if (is_ec) {
    const uint8_t *cb, *ce;
    if (read_tl(&alg, &cb, &ce) == 0x06) { /* named curve OID */
      if (OIDEQ(cb, ce, kCurveP256))
        curve = nc_curve_p256();
      else if (OIDEQ(cb, ce, kCurveP384))
        curve = nc_curve_p384();
    }
  }
  if (read_tl(&spki, &kb, &ke) != 0x03) return; /* BIT STRING */
  if (kb >= ke || *kb != 0x00) return;          /* unused-bits byte must be 0 */
  key = kb + 1;
  key_len = (size_t)(ke - (kb + 1));
  if (is_rsa) {
    nc_der_reader rsaw = der_enter(key, ke);
    const uint8_t *sb, *se;
    nc_der_reader rsa;
    const uint8_t *nb, *ne, *eb, *ee;
    if (read_tl(&rsaw, &sb, &se) != 0x30) return; /* RSAPublicKey SEQUENCE */
    rsa = der_enter(sb, se);
    if (read_tl(&rsa, &nb, &ne) != 0x02) return;
    if (read_tl(&rsa, &eb, &ee) != 0x02) return;
    nc_bi_from_bytes(&c->rsa_pub.n, nb, (size_t)(ne - nb));
    nc_bi_from_bytes(&c->rsa_pub.e, eb, (size_t)(ee - eb));
    c->rsa_pub.modulus_bytes = (nc_bi_bitlen(&c->rsa_pub.n) + 7) / 8;
    /* Reject implausibly large moduli: they would overflow the fixed-size
       bignum during verification (see NC_RSA_MAX_MODULUS_BYTES). */
    c->rsa_pub.valid = !nc_bi_is_zero(&c->rsa_pub.n) &&
                       c->rsa_pub.modulus_bytes <= NC_RSA_MAX_MODULUS_BYTES;
    c->key_type = NC_KEY_RSA;
  } else if (is_ec && curve) {
    c->key_type = NC_KEY_EC;
    c->ec_curve = curve;
    c->ec_point = key;
    c->ec_point_len = key_len;
  }
}

/* Walk the extensions SEQUENCE value for SAN dNSNames and basicConstraints CA. */
static void parse_extensions(const uint8_t* b, const uint8_t* e,
                             nc_x509_cert* c) {
  nc_der_reader exts = der_enter(b, e);
  const uint8_t *xb, *xe;
  while (exts.ok && exts.p < exts.end) {
    nc_der_reader ext;
    const uint8_t *ob, *oe;
    const uint8_t *vb, *ve;
    uint8_t t;
    if (read_tl(&exts, &xb, &xe) != 0x30) break; /* Extension SEQUENCE */
    ext = der_enter(xb, xe);
    if (read_tl(&ext, &ob, &oe) != 0x06) continue; /* extnID */
    t = read_tl(&ext, &vb, &ve);
    if (t == 0x01) t = read_tl(&ext, &vb, &ve); /* skip critical BOOLEAN */
    if (t != 0x04) continue;                    /* extnValue OCTET STRING */
    if (OIDEQ(ob, oe, kOidSan)) {
      nc_der_reader san = der_enter(vb, ve);
      const uint8_t *gb, *ge;
      nc_der_reader names;
      const uint8_t *nb, *ne;
      if (read_tl(&san, &gb, &ge) != 0x30) continue; /* GeneralNames SEQUENCE */
      names = der_enter(gb, ge);
      while (names.ok && names.p < names.end) {
        uint8_t gt = read_tl(&names, &nb, &ne);
        if (!names.ok) break;
        if (gt == 0x82 && c->san_count < NC_X509_MAX_SAN) { /* [2] dNSName */
          c->san[c->san_count] = (const char*)nb;
          c->san_len[c->san_count] = (size_t)(ne - nb);
          c->san_count++;
        }
      }
    } else if (OIDEQ(ob, oe, kOidBasicConstraints)) {
      nc_der_reader bc = der_enter(vb, ve);
      const uint8_t *sb, *se;
      nc_der_reader seq;
      const uint8_t *cb, *ce;
      if (read_tl(&bc, &sb, &se) != 0x30) continue; /* SEQUENCE */
      seq = der_enter(sb, se);
      if (read_tl(&seq, &cb, &ce) == 0x01) /* cA BOOLEAN */
        c->is_ca = (ce > cb && cb[0] != 0x00);
    }
  }
}

/* DER ECDSA-Sig-Value SEQUENCE { INTEGER r, INTEGER s } -> r,s magnitudes
 * (pointers into the sig buffer). */
static int parse_ecdsa_sig(const uint8_t* sig, size_t len, const uint8_t** rb,
                           size_t* rlen, const uint8_t** sb, size_t* slen) {
  nc_der_reader top = der_enter(sig, sig + len);
  const uint8_t *seqb, *seqe;
  nc_der_reader seq;
  const uint8_t *rbeg, *rend, *sbeg, *send;
  if (read_tl(&top, &seqb, &seqe) != 0x30) return 0;
  seq = der_enter(seqb, seqe);
  if (read_tl(&seq, &rbeg, &rend) != 0x02) return 0;
  if (read_tl(&seq, &sbeg, &send) != 0x02) return 0;
  while (rbeg < rend && *rbeg == 0x00) ++rbeg;
  while (sbeg < send && *sbeg == 0x00) ++sbeg;
  *rb = rbeg;
  *rlen = (size_t)(rend - rbeg);
  *sb = sbeg;
  *slen = (size_t)(send - sbeg);
  return 1;
}

/* Hash @data with the algorithm implied by digest length (20/32/48/64). */
static int hash_len(const uint8_t* data, size_t n, size_t hlen,
                    uint8_t* out) {
  if (hlen == 20)
    nc_sha1(data, n, out);
  else if (hlen == 32)
    nc_sha256(data, n, out);
  else if (hlen == 48)
    nc_sha384(data, n, out);
  else if (hlen == 64)
    nc_sha512(data, n, out);
  else
    return 0;
  return 1;
}

/* DigestInfo prefixes for RSA PKCS#1 v1.5. */
static const uint8_t kDiSha1[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B,
                                  0x0E, 0x03, 0x02, 0x1A, 0x05, 0x00, 0x04,
                                  0x14};
static const uint8_t kDiSha256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60,
                                    0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                                    0x01, 0x05, 0x00, 0x04, 0x20};
static const uint8_t kDiSha384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60,
                                    0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                                    0x02, 0x05, 0x00, 0x04, 0x30};
static const uint8_t kDiSha512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60,
                                    0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                                    0x03, 0x05, 0x00, 0x04, 0x40};

static int rsa_pkcs1_verify(const nc_rsa_pubkey* key, const uint8_t* sig,
                            size_t sig_len, const uint8_t* hash, size_t hlen,
                            const uint8_t* di_prefix, size_t di_len) {
  uint8_t di[128];
  if (di_len + hlen > sizeof(di)) return 0;
  memcpy(di, di_prefix, di_len);
  memcpy(di + di_len, hash, hlen);
  return nc_rsa_verify_pkcs1v15(key, sig, sig_len, di, di_len + hlen);
}

static int to_lower_eq(const char* a, size_t alen, const char* b, size_t blen) {
  size_t i;
  if (alen != blen) return 0;
  for (i = 0; i < alen; ++i)
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
  return 1;
}

/* Match @host against a SAN pattern (case-insensitive; leftmost-label
 * wildcard). */
static int host_match(const char* host, size_t hlen, const char* pat,
                      size_t plen) {
  if (plen == 0) return 0;
  if (pat[0] == '*' && plen > 2 && pat[1] == '.') {
    const char* dot = (const char*)memchr(host, '.', hlen);
    size_t off;
    if (!dot) return 0;
    off = (size_t)(dot - host);
    return to_lower_eq(host + off, hlen - off, pat + 1, plen - 1);
  }
  return to_lower_eq(host, hlen, pat, plen);
}

static const char* kBundlePaths[] = {
    "/etc/ssl/certs/ca-certificates.crt", /* Debian/Ubuntu */
    "/etc/pki/tls/certs/ca-bundle.crt",   /* RHEL/Fedora */
    "/etc/ssl/ca-bundle.pem",             /* openSUSE */
    "/etc/ssl/cert.pem",                  /* Alpine/macOS/BSD */
    NULL};

/* ---- public API ---------------------------------------------------------- */

int nc_x509_subject_cn(const nc_x509_cert* c, char* out, size_t outcap) {
  static const uint8_t kCn[] = {0x06, 0x03, 0x55, 0x04, 0x03};
  const uint8_t* b = c->subject;
  const uint8_t* e;
  const uint8_t* p;
  if (!b || outcap == 0) {
    if (outcap) out[0] = 0;
    return 0;
  }
  e = b + c->subject_len;
  for (p = b; p + 7 < e; ++p) {
    if (memcmp(p, kCn, sizeof(kCn)) == 0) {
      const uint8_t* q = p + sizeof(kCn);
      uint8_t tag = q[0];
      size_t len = q[1];
      if ((tag == 0x0c || tag == 0x13 || tag == 0x16) && q + 2 + len <= e) {
        size_t n = len;
        if (n > outcap - 1) n = outcap - 1;
        memcpy(out, q + 2, n);
        out[n] = 0;
        return (int)n;
      }
    }
  }
  out[0] = 0;
  return 0;
}

int nc_x509_self_issued(const nc_x509_cert* c) {
  return c->issuer && c->issuer_len > 0 && c->issuer_len == c->subject_len &&
         memcmp(c->issuer, c->subject, c->issuer_len) == 0;
}

int nc_x509_parse(nc_x509_cert* c, const uint8_t* der, size_t len) {
  nc_der_reader top;
  const uint8_t *vb, *ve;
  nc_der_reader cert;
  const uint8_t* tbs_start;
  const uint8_t *tb, *te;
  nc_der_reader tbs;
  const uint8_t *xb, *xe;
  uint8_t t;
  const uint8_t* issuer_start;
  const uint8_t *valb, *vale;
  const uint8_t* subject_start;
  const uint8_t *spkib, *spkie;
  const uint8_t *sab, *sae;
  const uint8_t *sigb, *sige;

  memset(c, 0, sizeof(*c));
  c->pss_hash_len = 32;

  top = der_enter(der, der + len);
  if (read_tl(&top, &vb, &ve) != 0x30) return -1; /* Certificate SEQUENCE */
  cert = der_enter(vb, ve);

  /* tbsCertificate — capture full DER (tag..value). */
  tbs_start = cert.p;
  if (read_tl(&cert, &tb, &te) != 0x30) return -1;
  c->tbs = tbs_start;
  c->tbs_len = (size_t)(te - tbs_start);
  tbs = der_enter(tb, te);

  t = read_tl(&tbs, &xb, &xe);
  if (t == 0xA0) { /* [0] version */
    t = read_tl(&tbs, &xb, &xe);
  }
  if (t != 0x02) return -1; /* serialNumber INTEGER */
  read_tl(&tbs, &xb, &xe);  /* signature AlgorithmIdentifier */
  /* issuer Name */
  issuer_start = tbs.p;
  if (read_tl(&tbs, &xb, &xe) != 0x30) return -1;
  c->issuer = issuer_start;
  c->issuer_len = (size_t)(xe - issuer_start);
  /* validity */
  if (read_tl(&tbs, &valb, &vale) != 0x30) return -1;
  {
    nc_der_reader v = der_enter(valb, vale);
    const uint8_t *nb = NULL, *ne = NULL;
    uint8_t nt = read_tl(&v, &nb, &ne);
    c->not_before = parse_time(nt, nb, ne);
    nt = read_tl(&v, &nb, &ne);
    c->not_after = parse_time(nt, nb, ne);
  }
  /* subject Name */
  subject_start = tbs.p;
  if (read_tl(&tbs, &xb, &xe) != 0x30) return -1;
  c->subject = subject_start;
  c->subject_len = (size_t)(xe - subject_start);
  /* subjectPublicKeyInfo */
  if (read_tl(&tbs, &spkib, &spkie) != 0x30) return -1;
  parse_spki(spkib, spkie, c);
  /* optional [1] issuerUID, [2] subjectUID, [3] extensions */
  while (tbs.ok && tbs.p < tbs.end) {
    const uint8_t *eb, *ee;
    uint8_t et = read_tl(&tbs, &eb, &ee);
    if (et == 0xA3) { /* [3] extensions EXPLICIT */
      nc_der_reader ex = der_enter(eb, ee);
      const uint8_t *sb, *se;
      if (read_tl(&ex, &sb, &se) == 0x30) parse_extensions(sb, se, c);
      break;
    }
  }

  /* signatureAlgorithm + signatureValue (siblings of tbsCertificate) */
  if (read_tl(&cert, &sab, &sae) != 0x30) return -1;
  c->sig_alg = parse_sig_alg(sab, sae, &c->pss_hash_len);
  if (read_tl(&cert, &sigb, &sige) != 0x03) return -1; /* BIT STRING */
  if (sigb < sige && *sigb == 0x00) {
    c->sig = sigb + 1;
    c->sig_len = (size_t)(sige - (sigb + 1));
  } else {
    c->sig = sigb;
    c->sig_len = (size_t)(sige - sigb);
  }

  c->parsed = 1;
  return 0;
}

int nc_x509_verify_signature(const nc_x509_cert* child,
                             const nc_x509_cert* issuer) {
  if (!child->parsed || !issuer->parsed) return 0;
  switch (child->sig_alg) {
    case NC_SIG_RSA_PKCS1_SHA1:
    case NC_SIG_RSA_PKCS1_SHA256:
    case NC_SIG_RSA_PKCS1_SHA384:
    case NC_SIG_RSA_PKCS1_SHA512: {
      uint8_t h[64];
      size_t hlen;
      const uint8_t* pfx;
      size_t pfx_len;
      if (issuer->key_type != NC_KEY_RSA || !issuer->rsa_pub.valid) return 0;
      hlen = child->sig_alg == NC_SIG_RSA_PKCS1_SHA1     ? 20
             : child->sig_alg == NC_SIG_RSA_PKCS1_SHA256 ? 32
             : child->sig_alg == NC_SIG_RSA_PKCS1_SHA384 ? 48
                                                         : 64;
      if (!hash_len(child->tbs, child->tbs_len, hlen, h)) return 0;
      pfx = hlen == 20   ? kDiSha1
            : hlen == 32 ? kDiSha256
            : hlen == 48 ? kDiSha384
                         : kDiSha512;
      pfx_len = hlen == 20   ? sizeof(kDiSha1)
                : hlen == 32 ? sizeof(kDiSha256)
                : hlen == 48 ? sizeof(kDiSha384)
                             : sizeof(kDiSha512);
      return rsa_pkcs1_verify(&issuer->rsa_pub, child->sig, child->sig_len, h,
                              hlen, pfx, pfx_len);
    }
    case NC_SIG_RSA_PSS: {
      uint8_t h[64];
      if (issuer->key_type != NC_KEY_RSA || !issuer->rsa_pub.valid) return 0;
      if (!hash_len(child->tbs, child->tbs_len, child->pss_hash_len, h))
        return 0;
      return nc_rsa_verify_pss(&issuer->rsa_pub, child->sig, child->sig_len, h,
                               child->pss_hash_len);
    }
    case NC_SIG_ECDSA_SHA1:
    case NC_SIG_ECDSA_SHA256:
    case NC_SIG_ECDSA_SHA384:
    case NC_SIG_ECDSA_SHA512: {
      uint8_t h[64];
      size_t hlen;
      const uint8_t *r, *s;
      size_t rlen, slen;
      if (issuer->key_type != NC_KEY_EC || !issuer->ec_curve) return 0;
      hlen = child->sig_alg == NC_SIG_ECDSA_SHA1     ? 20
             : child->sig_alg == NC_SIG_ECDSA_SHA256 ? 32
             : child->sig_alg == NC_SIG_ECDSA_SHA384 ? 48
                                                     : 64;
      if (!hash_len(child->tbs, child->tbs_len, hlen, h)) return 0;
      if (!parse_ecdsa_sig(child->sig, child->sig_len, &r, &rlen, &s, &slen))
        return 0;
      return nc_ecdsa_verify(issuer->ec_curve, issuer->ec_point,
                             issuer->ec_point_len, h, hlen, r, rlen, s, slen);
    }
    default:
      return 0;
  }
}

int nc_x509_verify_tls13(const nc_x509_cert* leaf, uint16_t scheme,
                         const uint8_t* msg, size_t msg_len, const uint8_t* sig,
                         size_t sig_len) {
  if (!leaf->parsed) return 0;
  switch (scheme) {
    case 0x0804: /* rsa_pss_rsae_sha256 */
    case 0x0809: /* rsa_pss_pss_sha256 */
    case 0x0805: /* rsa_pss_rsae_sha384 */
    case 0x080a: /* rsa_pss_pss_sha384 */
    case 0x0806: /* rsa_pss_rsae_sha512 */
    case 0x080b: /* rsa_pss_pss_sha512 */
    {
      uint8_t h[64];
      size_t hlen;
      if (leaf->key_type != NC_KEY_RSA || !leaf->rsa_pub.valid) return 0;
      hlen = (scheme == 0x0804 || scheme == 0x0809)   ? 32
             : (scheme == 0x0805 || scheme == 0x080a) ? 48
                                                      : 64;
      if (!hash_len(msg, msg_len, hlen, h)) return 0;
      return nc_rsa_verify_pss(&leaf->rsa_pub, sig, sig_len, h, hlen);
    }
    case 0x0403:   /* ecdsa_secp256r1_sha256 */
    case 0x0503: { /* ecdsa_secp384r1_sha384 */
      uint8_t h[64];
      size_t hlen;
      const nc_ec_curve* want;
      const uint8_t *r, *s;
      size_t rlen, slen;
      if (leaf->key_type != NC_KEY_EC || !leaf->ec_curve) return 0;
      hlen = scheme == 0x0403 ? 32 : 48;
      want = scheme == 0x0403 ? nc_curve_p256() : nc_curve_p384();
      if (leaf->ec_curve != want) return 0; /* cert curve must match scheme */
      if (!hash_len(msg, msg_len, hlen, h)) return 0;
      if (!parse_ecdsa_sig(sig, sig_len, &r, &rlen, &s, &slen)) return 0;
      return nc_ecdsa_verify(leaf->ec_curve, leaf->ec_point, leaf->ec_point_len,
                             h, hlen, r, rlen, s, slen);
    }
    default:
      return 0; /* ed25519/ed448, secp521r1, legacy pkcs1 unsupported */
  }
}

/* ---- trust store --------------------------------------------------------- */

/* Base64 decode (skips whitespace). Returns output length, or -1 on error. */
static int b64_decode(const char* in, size_t in_len, uint8_t* out,
                      size_t outcap) {
  int8_t tbl[256];
  size_t i;
  int bits = 0;
  uint32_t acc = 0;
  size_t outn = 0;
  for (i = 0; i < 256; ++i) tbl[i] = -1;
  for (i = 0; i < 26; ++i) {
    tbl['A' + i] = (int8_t)i;
    tbl['a' + i] = (int8_t)(26 + i);
  }
  for (i = 0; i < 10; ++i) tbl['0' + i] = (int8_t)(52 + i);
  tbl['+'] = 62;
  tbl['/'] = 63;
  for (i = 0; i < in_len; ++i) {
    unsigned char ch = (unsigned char)in[i];
    int8_t v;
    if (ch == '=') break;
    v = tbl[ch];
    if (v < 0) continue; /* skip whitespace/newlines */
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outn >= outcap) return -1;
      out[outn++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return (int)outn;
}

/* Parse all PEM "CERTIFICATE" blocks from @pem; for each, malloc a DER copy and
 * append it to *ders / *der_lens (grown via realloc). Returns count. */
static int pem_to_certs(const char* pem, size_t pem_len, uint8_t*** ders,
                        size_t** der_lens) {
  static const char kBegin[] = "-----BEGIN CERTIFICATE-----";
  static const char kEnd[] = "-----END CERTIFICATE-----";
  const char* p = pem;
  const char* end = pem + pem_len;
  int count = 0;
  int cap = 0;
  uint8_t** arr = NULL;
  size_t* lens = NULL;
  while (p < end) {
    const char* b = strstr(p, kBegin);
    const char* body;
    const char* e;
    size_t blen;
    uint8_t* der;
    int dlen;
    if (!b) break;
    body = b + (sizeof(kBegin) - 1);
    e = strstr(body, kEnd);
    if (!e) break;
    blen = (size_t)(e - body);
    der = (uint8_t*)malloc(blen); /* decoded is smaller than encoded */
    if (!der) break;
    dlen = b64_decode(body, blen, der, blen);
    if (dlen <= 0) {
      free(der);
      p = e + (sizeof(kEnd) - 1);
      continue;
    }
    if (count >= cap) {
      int ncap = cap ? cap * 2 : 16;
      uint8_t** na = (uint8_t**)realloc(arr, (size_t)ncap * sizeof(*na));
      size_t* nl = (size_t*)realloc(lens, (size_t)ncap * sizeof(*nl));
      if (!na || !nl) {
        free(der);
        if (na) arr = na;
        if (nl) lens = nl;
        break;
      }
      arr = na;
      lens = nl;
      cap = ncap;
    }
    arr[count] = der;
    lens[count] = (size_t)dlen;
    count++;
    p = e + (sizeof(kEnd) - 1);
  }
  *ders = arr;
  *der_lens = lens;
  return count;
}

int nc_trust_store_load(nc_trust_store* s, const char* path) {
  const char* chosen = path;
  FILE* f;
  char* pem = NULL;
  size_t pem_len = 0, pem_cap = 0;
  uint8_t** ders = NULL;
  size_t* der_lens = NULL;
  int n, i;
  nc_x509_cert* roots = NULL;
  int root_count = 0;

  memset(s, 0, sizeof(*s));

  if (!chosen || !chosen[0]) {
    int k;
    chosen = NULL;
    for (k = 0; kBundlePaths[k]; ++k) {
      FILE* t = fopen(kBundlePaths[k], "rb");
      if (t) {
        fclose(t);
        chosen = kBundlePaths[k];
        break;
      }
    }
  }
  if (!chosen) return -1;
  f = fopen(chosen, "rb");
  if (!f) return -1;
  for (;;) {
    char buf[8192];
    size_t r = fread(buf, 1, sizeof(buf), f);
    if (r == 0) break;
    if (pem_len + r + 1 > pem_cap) {
      size_t ncap = pem_cap ? pem_cap * 2 : 65536;
      char* np;
      while (ncap < pem_len + r + 1) ncap *= 2;
      np = (char*)realloc(pem, ncap);
      if (!np) {
        free(pem);
        fclose(f);
        return -1;
      }
      pem = np;
      pem_cap = ncap;
    }
    memcpy(pem + pem_len, buf, r);
    pem_len += r;
  }
  fclose(f);
  if (!pem) return -1;
  pem[pem_len] = 0;

  n = pem_to_certs(pem, pem_len, &ders, &der_lens);
  free(pem);
  if (n <= 0) {
    free(ders);
    free(der_lens);
    return -1;
  }
  roots = (nc_x509_cert*)malloc((size_t)n * sizeof(*roots));
  if (!roots) {
    for (i = 0; i < n; ++i) free(ders[i]);
    free(ders);
    free(der_lens);
    return -1;
  }
  for (i = 0; i < n; ++i) {
    nc_x509_cert c;
    if (nc_x509_parse(&c, ders[i], der_lens[i]) == 0 && c.parsed) {
      roots[root_count] = c;
      /* compact the DER arrays to align indices with roots[] */
      ders[root_count] = ders[i];
      der_lens[root_count] = der_lens[i];
      if (root_count != i) ders[i] = NULL;
      root_count++;
    } else {
      free(ders[i]);
      ders[i] = NULL;
    }
  }
  if (root_count == 0) {
    free(roots);
    free(ders);
    free(der_lens);
    return -1;
  }
  s->roots = roots;
  s->root_der = ders;
  s->root_der_len = der_lens;
  s->count = root_count;
  s->loaded = 1;
  return 0;
}

void nc_trust_store_free(nc_trust_store* s) {
  int i;
  if (!s) return;
  if (s->root_der) {
    for (i = 0; i < s->count; ++i) free(s->root_der[i]);
    free(s->root_der);
  }
  free(s->root_der_len);
  free(s->roots);
  memset(s, 0, sizeof(*s));
}

/* ---- chain verification -------------------------------------------------- */

static int names_eq(const nc_x509_cert* a, const nc_x509_cert* b) {
  return a->subject && b->issuer && a->subject_len == b->issuer_len &&
         memcmp(a->subject, b->issuer, a->subject_len) == 0;
}

int nc_x509_verify_chain(nc_verify_result* res, const uint8_t* const* der_chain,
                         const size_t* der_lens, int n,
                         const nc_trust_store* store, const char* hostname,
                         int64_t now_epoch) {
  nc_x509_cert chain[16];
  const nc_x509_cert* cur;
  int i, depth;

  memset(res, 0, sizeof(*res));
  if (n <= 0) {
    snprintf(res->error, sizeof(res->error), "empty certificate chain");
    return -1;
  }
  if (n > 16) {
    snprintf(res->error, sizeof(res->error), "certificate chain too long");
    return -1;
  }
  for (i = 0; i < n; ++i) {
    if (nc_x509_parse(&chain[i], der_chain[i], der_lens[i]) != 0 ||
        !chain[i].parsed) {
      snprintf(res->error, sizeof(res->error), "failed to parse a certificate");
      return -1;
    }
  }
  nc_x509_subject_cn(&chain[0], res->subject_cn, sizeof(res->subject_cn));

  /* Hostname (SAN, with CN fallback). */
  if (hostname && hostname[0]) {
    size_t hlen = strlen(hostname);
    int ok = 0;
    int j;
    for (j = 0; j < chain[0].san_count; ++j) {
      if (host_match(hostname, hlen, chain[0].san[j], chain[0].san_len[j])) {
        ok = 1;
        break;
      }
    }
    if (!ok && chain[0].san_count == 0) {
      char cn[256];
      int cnlen = nc_x509_subject_cn(&chain[0], cn, sizeof(cn));
      if (cnlen > 0) ok = host_match(hostname, hlen, cn, (size_t)cnlen);
    }
    if (!ok) {
      snprintf(res->error, sizeof(res->error), "hostname mismatch for %s",
               hostname);
      return -1;
    }
  }

  /* Walk from the leaf to a trusted root. */
  cur = &chain[0];
  for (depth = 0; depth < 16; ++depth) {
    int r;
    const nc_x509_cert* next = NULL;
    /* Validity. Treat an unparseable date (not_before/not_after == 0) as a
       failure rather than "no constraint" (fail closed). */
    if (now_epoch &&
        (cur->not_before == 0 || cur->not_after == 0 ||
         now_epoch < cur->not_before || now_epoch > cur->not_after)) {
      snprintf(res->error, sizeof(res->error),
               "certificate expired or not yet valid");
      return -1;
    }
    /* Trust anchor: a CA root whose subject is this cert's issuer. */
    for (r = 0; r < store->count; ++r) {
      const nc_x509_cert* root = &store->roots[r];
      if (!root->is_ca) continue;  /* issuer must be a CA (basicConstraints) */
      if (names_eq(root, cur) && nc_x509_verify_signature(cur, root)) {
        if (now_epoch && root->not_after && now_epoch > root->not_after)
          continue;
        res->ok = 1;
        return 0;
      }
    }
    /* Otherwise find a CA intermediate in the presented chain. An end-entity
       (CA:FALSE) cert must never be accepted as an issuer (basicConstraints
       bypass / cert forgery). */
    for (i = 0; i < n; ++i) {
      const nc_x509_cert* cand = &chain[i];
      if (cand == cur) continue;
      if (!cand->is_ca) continue;
      if (names_eq(cand, cur) && nc_x509_verify_signature(cur, cand)) {
        next = cand;
        break;
      }
    }
    if (!next) {
      snprintf(res->error, sizeof(res->error),
               "unable to build chain to a trusted root");
      return -1;
    }
    cur = next;
  }
  snprintf(res->error, sizeof(res->error), "certificate chain too long");
  return -1;
}
