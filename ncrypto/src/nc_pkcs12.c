/* SPDX-License-Identifier: Apache-2.0
 * PKCS#12 (PFX) parser: extract the private key + certificates from a .p12,
 * decrypting PBES2/AES SafeBags. Ported from the C++ nanopdf::pkcs12 reader.
 * Only PBES2 (PBKDF2 + AES-CBC) is supported; the MAC is not verified. */

#include "ncrypto/nc_pkcs12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ncrypto/nc_asn1.h"
#include "ncrypto/nc_bigint.h"
#include "ncrypto/nc_rsa.h"

/* Context-specific constructed [0] EXPLICIT and [0] IMPLICIT primitive tags. */
#define TAG_CTX0_C 0xA0
#define TAG_CTX0_P 0x80

static const uint8_t OID_DATA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                   0x0D, 0x01, 0x07, 0x01};
static const uint8_t OID_ENCRYPTED[] = {0x2A, 0x86, 0x48, 0x86, 0xF7,
                                        0x0D, 0x01, 0x07, 0x06};
static const uint8_t OID_KEYBAG[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
                                     0x01, 0x0C, 0x0A, 0x01, 0x01};
static const uint8_t OID_SHROUDED[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
                                       0x01, 0x0C, 0x0A, 0x01, 0x02};
static const uint8_t OID_CERTBAG[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D,
                                      0x01, 0x0C, 0x0A, 0x01, 0x03};

static int oid_is(const uint8_t* b, const uint8_t* e, const uint8_t* want,
                  size_t n) {
  return (size_t)(e - b) == n && memcmp(b, want, n) == 0;
}

static void reader_sub(nc_der_reader* r, const uint8_t* b, const uint8_t* e) {
  r->p = b;
  r->end = e;
  r->ok = 1;
}

/* Extract the RSA modulus from an X.509 certificate DER into @mod.
 * Returns 1 on success, 0 otherwise. */
static int cert_modulus(const uint8_t* cert, size_t len, nc_bigint* mod) {
  nc_der_reader top, tbs, spki, bs, rsa;
  const uint8_t *vb, *ve;

  nc_der_reader_init(&top, cert, len);
  if (nc_der_read(&top, &vb, &ve) != NC_ASN1_SEQUENCE) return 0; /* Certificate */
  reader_sub(&top, vb, ve);
  if (nc_der_read(&top, &vb, &ve) != NC_ASN1_SEQUENCE) return 0; /* tbsCertificate */
  reader_sub(&tbs, vb, ve);

  /* tbs fields: [0] version OPTIONAL, serialNumber, signature, issuer,
   * validity, subject, subjectPublicKeyInfo. */
  if (nc_der_read(&tbs, &vb, &ve) == 0) return 0;
  if (!tbs.ok) return 0;
  /* If the first field was the explicit version tag, read serialNumber next. */
  /* We re-read by peeking: simplest is to track and skip remaining fields. */
  /* The TLV just read is either [0] version or serialNumber. */
  /* Skip until we reach the 5th SEQUENCE which is SPKI. */
  {
    /* We've consumed one field above. Determine how many SEQUENCEs we still
     * need: signature(1) issuer(2) validity(3) subject(4) SPKI(5). The field
     * we read was version([0]) or serialNumber(INTEGER); neither is one of
     * those SEQUENCEs, so we need to read forward and count SEQUENCEs. */
    int seq_count = 0;
    spki.ok = 0;
    for (;;) {
      uint8_t tag = nc_der_read(&tbs, &vb, &ve);
      if (tag == 0 || !tbs.ok) break;
      if (tag == NC_ASN1_SEQUENCE) {
        seq_count++;
        if (seq_count == 5) {
          reader_sub(&spki, vb, ve);
          break;
        }
      }
    }
    if (!spki.ok) return 0;
  }

  /* SubjectPublicKeyInfo: algorithm SEQUENCE, subjectPublicKey BIT STRING. */
  if (nc_der_read(&spki, &vb, &ve) != NC_ASN1_SEQUENCE) return 0; /* algorithm */
  if (nc_der_read(&spki, &vb, &ve) != NC_ASN1_BIT_STRING) return 0;
  if (ve <= vb) return 0;
  /* First byte = unused bits (expect 0); the rest is RSAPublicKey. */
  reader_sub(&bs, vb + 1, ve);
  if (nc_der_read(&bs, &vb, &ve) != NC_ASN1_SEQUENCE) return 0; /* RSAPublicKey */
  reader_sub(&rsa, vb, ve);
  if (nc_der_read(&rsa, &vb, &ve) != NC_ASN1_INTEGER) return 0; /* modulus */
  /* Strip a possible leading 0x00 sign byte. */
  while (ve > vb && *vb == 0x00) vb++;
  if (ve <= vb) return 0;
  nc_bi_from_bytes(mod, vb, (size_t)(ve - vb));
  return 1;
}

static int add_cert(nc_pkcs12_bundle* out, const uint8_t* b, const uint8_t* e) {
  size_t n;
  uint8_t* copy;
  if (out->cert_count >= NC_PKCS12_MAX_CERTS) return 0;
  if (e <= b) return 0;
  n = (size_t)(e - b);
  copy = (uint8_t*)malloc(n);
  if (!copy) return 0;
  memcpy(copy, b, n);
  out->certs[out->cert_count] = copy;
  out->cert_lens[out->cert_count] = n;
  out->cert_count++;
  return 1;
}

/* Parse a SafeContents (SEQUENCE OF SafeBag), collecting certs/key into @out. */
static void parse_safe_contents(const uint8_t* b, const uint8_t* e,
                                const char* pw, nc_pkcs12_bundle* out) {
  nc_der_reader sc, bags;
  const uint8_t *vb, *ve;

  nc_der_reader_init(&sc, b, (size_t)(e - b));
  if (nc_der_read(&sc, &vb, &ve) != NC_ASN1_SEQUENCE) return; /* SafeContents */
  reader_sub(&bags, vb, ve);

  while (bags.p < bags.end && bags.ok) {
    nc_der_reader bag, val;
    const uint8_t *bb, *be, *ob, *oe, *cb, *ce;
    if (nc_der_read(&bags, &bb, &be) != NC_ASN1_SEQUENCE) break; /* SafeBag */
    reader_sub(&bag, bb, be);
    if (nc_der_read(&bag, &ob, &oe) != NC_ASN1_OID) continue;    /* bagId */
    if (nc_der_read(&bag, &cb, &ce) != TAG_CTX0_C) continue;     /* [0] value */
    reader_sub(&val, cb, ce);

    if (oid_is(ob, oe, OID_CERTBAG, sizeof(OID_CERTBAG))) {
      /* CertBag ::= SEQUENCE { certId OID, certValue [0] OCTET STRING } */
      nc_der_reader cbag, cv;
      const uint8_t *xb, *xe;
      if (nc_der_read(&val, &xb, &xe) != NC_ASN1_SEQUENCE) continue;
      reader_sub(&cbag, xb, xe);
      nc_der_read(&cbag, &xb, &xe);                          /* certId OID */
      if (nc_der_read(&cbag, &xb, &xe) != TAG_CTX0_C) continue; /* [0] */
      reader_sub(&cv, xb, xe);
      if (nc_der_read(&cv, &xb, &xe) != NC_ASN1_OCTET_STRING) continue;
      add_cert(out, xb, xe);
    } else if (oid_is(ob, oe, OID_KEYBAG, sizeof(OID_KEYBAG))) {
      /* bagValue = PrivateKeyInfo (PKCS#8, plaintext) */
      if (!out->key.valid) {
        nc_rsa_privkey k;
        memset(&k, 0, sizeof(k));
        if (nc_rsa_parse_privkey_der(&k, cb, (size_t)(ce - cb)) == 0 && k.valid)
          out->key = k;
      }
    } else if (oid_is(ob, oe, OID_SHROUDED, sizeof(OID_SHROUDED))) {
      /* bagValue = EncryptedPrivateKeyInfo (PBES2) */
      if (!out->key.valid) {
        size_t enclen = (size_t)(ce - cb);
        uint8_t* p8 = (uint8_t*)malloc(enclen ? enclen : 1);
        if (p8) {
          int n = nc_pbes2_decrypt_pkcs8(cb, enclen, pw, p8, enclen);
          if (n > 0) {
            nc_rsa_privkey k;
            memset(&k, 0, sizeof(k));
            if (nc_rsa_parse_privkey_der(&k, p8, (size_t)n) == 0 && k.valid)
              out->key = k;
          }
          free(p8);
        }
      }
    }
  }
}

/* Parse one AuthenticatedSafe ContentInfo (data or encryptedData). */
static void parse_content_info(const uint8_t* b, const uint8_t* e,
                               const char* pw, nc_pkcs12_bundle* out) {
  nc_der_reader ci, c;
  const uint8_t *vb, *ve, *ob, *oe;

  nc_der_reader_init(&ci, b, (size_t)(e - b));
  if (nc_der_read(&ci, &vb, &ve) != NC_ASN1_SEQUENCE) return; /* ContentInfo */
  reader_sub(&c, vb, ve);
  if (nc_der_read(&c, &ob, &oe) != NC_ASN1_OID) return;       /* contentType */

  if (oid_is(ob, oe, OID_DATA, sizeof(OID_DATA))) {
    nc_der_reader c0;
    if (nc_der_read(&c, &vb, &ve) != TAG_CTX0_C) return;        /* [0] */
    reader_sub(&c0, vb, ve);
    if (nc_der_read(&c0, &vb, &ve) != NC_ASN1_OCTET_STRING) return;
    parse_safe_contents(vb, ve, pw, out);
  } else if (oid_is(ob, oe, OID_ENCRYPTED, sizeof(OID_ENCRYPTED))) {
    nc_der_reader c0, ed, eci;
    const uint8_t *xb, *xe, *ab, *ae;
    uint8_t* plain;
    int n;
    if (nc_der_read(&c, &vb, &ve) != TAG_CTX0_C) return;        /* [0] */
    reader_sub(&c0, vb, ve);
    if (nc_der_read(&c0, &vb, &ve) != NC_ASN1_SEQUENCE) return; /* EncryptedData */
    reader_sub(&ed, vb, ve);
    nc_der_read(&ed, &vb, &ve);                                 /* version */
    if (nc_der_read(&ed, &vb, &ve) != NC_ASN1_SEQUENCE) return; /* EncryptedContentInfo */
    reader_sub(&eci, vb, ve);
    nc_der_read(&eci, &xb, &xe);                                /* contentType */
    if (nc_der_read(&eci, &ab, &ae) != NC_ASN1_SEQUENCE) return; /* contentEncAlg */
    if (nc_der_read(&eci, &xb, &xe) != TAG_CTX0_P) return;      /* [0] encryptedContent */
    {
      size_t enclen = (size_t)(xe - xb);
      plain = (uint8_t*)malloc(enclen ? enclen : 1);
      if (!plain) return;
      n = nc_pbes2_decrypt(ab, (size_t)(ae - ab), xb, enclen, pw, plain, enclen);
      if (n > 0)
        parse_safe_contents(plain, plain + n, pw, out);
      free(plain);
    }
  }
}

int nc_pkcs12_parse(nc_pkcs12_bundle* bundle, const uint8_t* data, size_t len,
                    const char* password) {
  nc_der_reader top, pfx, as, a0, authsafe, cis;
  const uint8_t *vb, *ve, *ob, *oe;
  int i;

  if (!bundle) return -1;
  memset(bundle, 0, sizeof(*bundle));
  if (!data || len == 0) {
    snprintf(bundle->error, sizeof(bundle->error), "empty input");
    return -1;
  }

  nc_der_reader_init(&top, data, len);
  if (nc_der_read(&top, &vb, &ve) != NC_ASN1_SEQUENCE) {
    snprintf(bundle->error, sizeof(bundle->error), "not a PFX SEQUENCE");
    return -1;
  }
  reader_sub(&pfx, vb, ve);
  nc_der_read(&pfx, &vb, &ve); /* version INTEGER */

  /* authSafe ContentInfo: SEQUENCE { OID(data), [0] OCTET STRING(AuthSafe) } */
  if (nc_der_read(&pfx, &vb, &ve) != NC_ASN1_SEQUENCE) {
    snprintf(bundle->error, sizeof(bundle->error), "no authSafe");
    return -1;
  }
  reader_sub(&as, vb, ve);
  if (nc_der_read(&as, &ob, &oe) != NC_ASN1_OID) {
    snprintf(bundle->error, sizeof(bundle->error), "bad authSafe");
    return -1;
  }
  if (nc_der_read(&as, &vb, &ve) != TAG_CTX0_C) {
    snprintf(bundle->error, sizeof(bundle->error), "no authSafe content");
    return -1;
  }
  reader_sub(&a0, vb, ve);
  if (nc_der_read(&a0, &vb, &ve) != NC_ASN1_OCTET_STRING) {
    snprintf(bundle->error, sizeof(bundle->error), "authSafe not OCTET STRING");
    return -1;
  }

  /* AuthenticatedSafe ::= SEQUENCE OF ContentInfo */
  nc_der_reader_init(&authsafe, vb, (size_t)(ve - vb));
  if (nc_der_read(&authsafe, &vb, &ve) != NC_ASN1_SEQUENCE) {
    snprintf(bundle->error, sizeof(bundle->error), "bad AuthenticatedSafe");
    return -1;
  }
  reader_sub(&cis, vb, ve);
  while (cis.p < cis.end && cis.ok) {
    const uint8_t* start = cis.p;
    const uint8_t *cb, *ce;
    if (nc_der_read(&cis, &cb, &ce) != NC_ASN1_SEQUENCE) break; /* ContentInfo */
    parse_content_info(start, cis.p, password, bundle);
  }

  if (!bundle->key.valid) {
    snprintf(bundle->error, sizeof(bundle->error),
             "no decryptable private key (wrong password or unsupported PBE)");
    nc_pkcs12_bundle_free(bundle);
    return -1;
  }

  /* Put the cert whose public key matches the private key first. */
  for (i = 0; i < bundle->cert_count; ++i) {
    nc_bigint mod;
    if (cert_modulus(bundle->certs[i], bundle->cert_lens[i], &mod) &&
        nc_bi_cmp(&mod, &bundle->key.n) == 0) {
      if (i != 0) {
        uint8_t* tc = bundle->certs[0];
        size_t tl = bundle->cert_lens[0];
        bundle->certs[0] = bundle->certs[i];
        bundle->cert_lens[0] = bundle->cert_lens[i];
        bundle->certs[i] = tc;
        bundle->cert_lens[i] = tl;
      }
      break;
    }
  }

  bundle->ok = 1;
  return 0;
}

void nc_pkcs12_bundle_free(nc_pkcs12_bundle* bundle) {
  int i;
  if (!bundle) return;
  for (i = 0; i < bundle->cert_count; ++i) {
    free(bundle->certs[i]);
    bundle->certs[i] = NULL;
    bundle->cert_lens[i] = 0;
  }
  bundle->cert_count = 0;
}
