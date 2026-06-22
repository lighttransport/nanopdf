/* SPDX-License-Identifier: Apache-2.0
 * CMS / PKCS#7 SignedData (detached, RSA + SHA-256, signed attributes, optional
 * RFC 3161 timestamp). Ported 1:1 from the C++ src/cms.cc. */
#include "ncrypto/nc_cms.h"

#include <stdlib.h>
#include <string.h>

#include "ncrypto/nc_hash.h"
#include "ncrypto/nc_x509.h"

/* ---- OIDs ----------------------------------------------------------------- */
#define OID_SIGNED_DATA "1.2.840.113549.1.7.2"
#define OID_DATA "1.2.840.113549.1.7.1"
#define OID_SHA256 "2.16.840.1.101.3.4.2.1"
#define OID_RSA_ENCRYPTION "1.2.840.113549.1.1.1"
#define OID_CONTENT_TYPE "1.2.840.113549.1.9.3"
#define OID_MESSAGE_DIGEST "1.2.840.113549.1.9.4"
#define OID_SIGNING_TIME "1.2.840.113549.1.9.5"
#define OID_TIMESTAMP_TOKEN "1.2.840.113549.1.9.16.2.14"

/* DER tags not in the public header. */
#define TAG_CONTEXT0_C 0xA0
#define TAG_CONTEXT1_C 0xA1

/* ---- small DER walk helpers ----------------------------------------------- */

/* Append SEQUENCE wrapping @content. */
static void wrap_seq(nc_buf* out, const nc_buf* content) {
  nc_der_tlv(out, NC_ASN1_SEQUENCE, content->data, content->len);
}

/* AlgorithmIdentifier ::= SEQUENCE { OID, NULL }. */
static void algorithm_id(nc_buf* out, const char* oid) {
  nc_buf c;
  nc_buf_init(&c);
  nc_der_oid(&c, oid);
  nc_der_null(&c);
  wrap_seq(out, &c);
  if (c.err) out->err = 1;
  nc_buf_free(&c);
}

/* OID value bytes (the content of der_oid, minus tag+length). OIDs used here
 * all have 1-byte length, matching the C++ helper. */
static void oid_val(const char* dotted, uint8_t* out, size_t* out_len) {
  nc_buf full;
  nc_buf_init(&full);
  nc_der_oid(&full, dotted);
  /* skip tag (1) + length (1) */
  *out_len = full.len >= 2 ? full.len - 2 : 0;
  if (*out_len) memcpy(out, full.data + 2, *out_len);
  nc_buf_free(&full);
}

/* Attribute ::= SEQUENCE { attrType OID, attrValues SET OF value }. @value is a
 * full TLV. */
static void attribute(nc_buf* out, const char* oid, const uint8_t* value,
                      size_t value_len) {
  nc_buf set, seq;
  nc_buf_init(&set);
  nc_der_tlv(&set, NC_ASN1_SET, value, value_len);
  nc_buf_init(&seq);
  nc_der_oid(&seq, oid);
  nc_buf_put(&seq, set.data, set.len);
  wrap_seq(out, &seq);
  if (set.err || seq.err) out->err = 1;
  nc_buf_free(&set);
  nc_buf_free(&seq);
}

/* Lexicographic comparison matching std::vector<uint8_t> ordering: compare up
 * to min length, then shorter < longer. */
static int lex_less(const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = memcmp(a, b, n);
  if (c != 0) return c < 0;
  return an < bn;
}

/* ---- minimal cert walk: issuer Name DER + serialNumber INTEGER body ------- */
typedef struct {
  const uint8_t* issuer;
  size_t issuer_len; /* full DER of issuer Name (tag..value) */
  const uint8_t* serial;
  size_t serial_len; /* serialNumber INTEGER body (no tag/len) */
  int valid;
} cert_info;

static cert_info parse_cert_info(const uint8_t* der, size_t len) {
  cert_info info;
  nc_der_reader top, c, tbs;
  const uint8_t *vb, *ve, *tvb, *tve;
  uint8_t t;
  memset(&info, 0, sizeof(info));

  nc_der_reader_init(&top, der, len);
  if (nc_der_read(&top, &vb, &ve) != NC_ASN1_SEQUENCE) return info; /* Cert */
  nc_der_reader_init(&c, vb, (size_t)(ve - vb));
  if (nc_der_read(&c, &vb, &ve) != NC_ASN1_SEQUENCE) return info; /* TBS */
  nc_der_reader_init(&tbs, vb, (size_t)(ve - vb));

  t = nc_der_read(&tbs, &tvb, &tve);
  if (t == TAG_CONTEXT0_C) t = nc_der_read(&tbs, &tvb, &tve); /* [0] version */
  if (!tbs.ok || t != NC_ASN1_INTEGER) return info;
  info.serial = tvb;
  info.serial_len = (size_t)(tve - tvb);
  /* signature AlgorithmIdentifier -- skip */
  if (nc_der_read(&tbs, &tvb, &tve) != NC_ASN1_SEQUENCE) return info;
  /* issuer Name: capture full DER (tag..value) */
  {
    const uint8_t* issuer_start = tbs.p;
    if (nc_der_read(&tbs, &tvb, &tve) != NC_ASN1_SEQUENCE) return info;
    info.issuer = issuer_start;
    info.issuer_len = (size_t)(tve - issuer_start);
  }
  info.valid = tbs.ok;
  return info;
}

/* ---- build ---------------------------------------------------------------- */
int nc_cms_build_signed_data(nc_buf* out, const uint8_t* content,
                             size_t content_len, const uint8_t* signer_cert,
                             size_t signer_cert_len,
                             const uint8_t* const* chain,
                             const size_t* chain_lens, int chain_n,
                             const nc_rsa_privkey* key,
                             const char* signing_time_utc, nc_tsa_cb tsa,
                             void* tsa_user) {
  cert_info ci;
  uint8_t md[NC_SHA256_LEN];
  nc_buf a_ct, a_st, a_md;        /* the three signed attributes */
  uint8_t md_octet[2 + NC_SHA256_LEN];
  const uint8_t* attr_ptr[3];
  size_t attr_len[3];
  int order[3] = {0, 1, 2};
  nc_buf signed_attrs_set;        /* 0x31 ... sorted (to be signed) */
  uint8_t sa_hash[NC_SHA256_LEN];
  uint8_t signature[1024];
  int sig_len;
  nc_buf signer_info, signer_items, sid, signed_attrs_ctx;
  nc_buf certificates, encap, signed_data, digest_algs;
  int i, j, rc = 0;

  if (!out || !key || !key->valid) return 1;
  ci = parse_cert_info(signer_cert, signer_cert_len);
  if (!ci.valid) return 1;

  /* messageDigest = SHA-256(content) */
  nc_sha256(content, content_len, md);

  /* contentType: value is the id-data OID TLV */
  nc_buf_init(&a_ct);
  {
    nc_buf oidtlv;
    nc_buf_init(&oidtlv);
    nc_der_oid(&oidtlv, OID_DATA);
    attribute(&a_ct, OID_CONTENT_TYPE, oidtlv.data, oidtlv.len);
    if (oidtlv.err) a_ct.err = 1;
    nc_buf_free(&oidtlv);
  }

  /* signingTime: value is a UTCTime TLV */
  nc_buf_init(&a_st);
  {
    nc_buf utc;
    nc_buf_init(&utc);
    nc_der_utctime(&utc, signing_time_utc);
    attribute(&a_st, OID_SIGNING_TIME, utc.data, utc.len);
    if (utc.err) a_st.err = 1;
    nc_buf_free(&utc);
  }

  /* messageDigest: value is an OCTET STRING TLV of md */
  nc_buf_init(&a_md);
  md_octet[0] = NC_ASN1_OCTET_STRING;
  md_octet[1] = NC_SHA256_LEN;
  memcpy(md_octet + 2, md, NC_SHA256_LEN);
  attribute(&a_md, OID_MESSAGE_DIGEST, md_octet, sizeof(md_octet));

  attr_ptr[0] = a_ct.data;
  attr_len[0] = a_ct.len;
  attr_ptr[1] = a_st.data;
  attr_len[1] = a_st.len;
  attr_ptr[2] = a_md.data;
  attr_len[2] = a_md.len;

  /* DER SET OF: sort members by encoding (lexicographic). */
  for (i = 0; i < 3; ++i) {
    for (j = i + 1; j < 3; ++j) {
      if (lex_less(attr_ptr[order[j]], attr_len[order[j]], attr_ptr[order[i]],
                   attr_len[order[i]])) {
        int tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
      }
    }
  }

  nc_buf_init(&signed_attrs_set);
  {
    nc_buf body;
    nc_buf_init(&body);
    for (i = 0; i < 3; ++i)
      nc_buf_put(&body, attr_ptr[order[i]], attr_len[order[i]]);
    nc_der_tlv(&signed_attrs_set, NC_ASN1_SET, body.data, body.len);
    if (body.err) signed_attrs_set.err = 1;
    nc_buf_free(&body);
  }

  /* Sign SHA-256(signed_attrs_set). */
  nc_sha256(signed_attrs_set.data, signed_attrs_set.len, sa_hash);
  sig_len = nc_rsa_sign_sha256(key, sa_hash, signature);
  if (sig_len <= 0) {
    rc = 1;
    goto cleanup;
  }

  /* [0] IMPLICIT signedAttrs: same sorted SET body, tag 0xA0. */
  nc_buf_init(&signed_attrs_ctx);
  nc_buf_put(&signed_attrs_ctx, signed_attrs_set.data, signed_attrs_set.len);
  if (signed_attrs_ctx.len > 0) signed_attrs_ctx.data[0] = TAG_CONTEXT0_C;

  /* sid = IssuerAndSerialNumber ::= SEQUENCE { issuer Name, serialNumber }. */
  nc_buf_init(&sid);
  {
    nc_buf body;
    nc_buf_init(&body);
    nc_buf_put(&body, ci.issuer, ci.issuer_len);
    nc_der_integer(&body, ci.serial, ci.serial_len);
    wrap_seq(&sid, &body);
    if (body.err) sid.err = 1;
    nc_buf_free(&body);
  }

  /* SignerInfo items. */
  nc_buf_init(&signer_items);
  nc_der_uint(&signer_items, 1);                         /* version */
  nc_buf_put(&signer_items, sid.data, sid.len);          /* sid */
  algorithm_id(&signer_items, OID_SHA256);               /* digestAlgorithm */
  nc_buf_put(&signer_items, signed_attrs_ctx.data,
             signed_attrs_ctx.len);                      /* [0] signedAttrs */
  algorithm_id(&signer_items, OID_RSA_ENCRYPTION);       /* signatureAlgorithm */
  nc_der_octet_string(&signer_items, signature, (size_t)sig_len); /* signature */

  if (tsa) {
    nc_buf token;
    nc_buf_init(&token);
    if (tsa(signature, (size_t)sig_len, &token, tsa_user) == 0 &&
        token.len > 0) {
      /* [1] unsignedAttrs: SET OF Attribute, but the C++ embeds a single
       * Attribute wrapped as a context [1] constructed value. */
      nc_buf set, attr;
      nc_buf_init(&set);
      nc_der_tlv(&set, NC_ASN1_SET, token.data, token.len);
      nc_buf_init(&attr);
      nc_der_oid(&attr, OID_TIMESTAMP_TOKEN);
      nc_buf_put(&attr, set.data, set.len);
      {
        nc_buf attr_seq;
        nc_buf_init(&attr_seq);
        wrap_seq(&attr_seq, &attr);
        nc_der_tlv(&signer_items, TAG_CONTEXT1_C, attr_seq.data, attr_seq.len);
        if (attr_seq.err) signer_items.err = 1;
        nc_buf_free(&attr_seq);
      }
      if (set.err || attr.err) signer_items.err = 1;
      nc_buf_free(&set);
      nc_buf_free(&attr);
    }
    nc_buf_free(&token);
  }

  nc_buf_init(&signer_info);
  wrap_seq(&signer_info, &signer_items);

  /* certificates [0] IMPLICIT: signer first, then chain. */
  nc_buf_init(&certificates);
  {
    nc_buf blob;
    nc_buf_init(&blob);
    nc_buf_put(&blob, signer_cert, signer_cert_len);
    for (i = 0; i < chain_n; ++i)
      nc_buf_put(&blob, chain[i], chain_lens[i]);
    nc_der_tlv(&certificates, TAG_CONTEXT0_C, blob.data, blob.len);
    if (blob.err) certificates.err = 1;
    nc_buf_free(&blob);
  }

  /* encapContentInfo: detached -> SEQUENCE { id-data }. */
  nc_buf_init(&encap);
  {
    nc_buf body;
    nc_buf_init(&body);
    nc_der_oid(&body, OID_DATA);
    wrap_seq(&encap, &body);
    if (body.err) encap.err = 1;
    nc_buf_free(&body);
  }

  /* digestAlgorithms SET OF AlgorithmIdentifier. */
  nc_buf_init(&digest_algs);
  {
    nc_buf body;
    nc_buf_init(&body);
    algorithm_id(&body, OID_SHA256);
    nc_der_tlv(&digest_algs, NC_ASN1_SET, body.data, body.len);
    if (body.err) digest_algs.err = 1;
    nc_buf_free(&body);
  }

  /* SignedData. */
  nc_buf_init(&signed_data);
  {
    nc_buf body, sis;
    nc_buf_init(&body);
    nc_der_uint(&body, 1);                              /* version */
    nc_buf_put(&body, digest_algs.data, digest_algs.len);
    nc_buf_put(&body, encap.data, encap.len);
    nc_buf_put(&body, certificates.data, certificates.len);
    nc_buf_init(&sis);                                  /* signerInfos SET */
    nc_der_tlv(&sis, NC_ASN1_SET, signer_info.data, signer_info.len);
    nc_buf_put(&body, sis.data, sis.len);
    wrap_seq(&signed_data, &body);
    if (body.err || sis.err) signed_data.err = 1;
    nc_buf_free(&body);
    nc_buf_free(&sis);
  }

  /* ContentInfo: SEQUENCE { signedData OID, [0] EXPLICIT SignedData }. */
  {
    nc_buf body, expl;
    nc_buf_init(&body);
    nc_der_oid(&body, OID_SIGNED_DATA);
    nc_buf_init(&expl);
    nc_der_tlv(&expl, TAG_CONTEXT0_C, signed_data.data, signed_data.len);
    nc_buf_put(&body, expl.data, expl.len);
    wrap_seq(out, &body);
    if (body.err || expl.err) out->err = 1;
    nc_buf_free(&body);
    nc_buf_free(&expl);
  }

  if (a_ct.err || a_st.err || a_md.err || signed_attrs_set.err ||
      signed_attrs_ctx.err || sid.err || signer_items.err || signer_info.err ||
      certificates.err || encap.err || digest_algs.err || signed_data.err ||
      out->err)
    rc = 1;

  nc_buf_free(&signed_attrs_ctx);
  nc_buf_free(&sid);
  nc_buf_free(&signer_items);
  nc_buf_free(&signer_info);
  nc_buf_free(&certificates);
  nc_buf_free(&encap);
  nc_buf_free(&digest_algs);
  nc_buf_free(&signed_data);

cleanup:
  nc_buf_free(&a_ct);
  nc_buf_free(&a_st);
  nc_buf_free(&a_md);
  nc_buf_free(&signed_attrs_set);
  return rc;
}

/* ---- verify --------------------------------------------------------------- */

/* SHA-256 DigestInfo prefix. */
static const uint8_t kSha256DiPrefix[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
    0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

/* Within a SET-of-Attribute content range [b,e), find the Attribute whose type
 * OID equals @oid and return the FULL TLV of its first value. */
static int find_attr_value(const uint8_t* b, const uint8_t* e,
                           const uint8_t* oid, size_t oid_len,
                           const uint8_t** vbeg, const uint8_t** vend) {
  nc_der_reader r;
  const uint8_t *ab, *ae;
  nc_der_reader_init(&r, b, (size_t)(e - b));
  while (r.p < r.end && r.ok) {
    nc_der_reader a;
    const uint8_t *ob, *oe;
    if (nc_der_read(&r, &ab, &ae) != NC_ASN1_SEQUENCE) break;
    nc_der_reader_init(&a, ab, (size_t)(ae - ab));
    if (nc_der_read(&a, &ob, &oe) != NC_ASN1_OID) continue;
    if ((size_t)(oe - ob) == oid_len && memcmp(ob, oid, oid_len) == 0) {
      const uint8_t *sb, *se;
      nc_der_reader sv;
      const uint8_t* start;
      const uint8_t *xb, *xe;
      if (nc_der_read(&a, &sb, &se) != NC_ASN1_SET) return 0;
      nc_der_reader_init(&sv, sb, (size_t)(se - sb));
      start = sv.p;
      if (nc_der_read(&sv, &xb, &xe) == 0) return 0;
      *vbeg = start;
      *vend = sv.p;
      return 1;
    }
  }
  return 0;
}

/* Extract genTime + TSA commonName from an RFC 3161 TimeStampToken. */
static void parse_tst(const uint8_t* token, size_t token_len, char* gen_time,
                      size_t gen_cap, char* tsa, size_t tsa_cap) {
  nc_der_reader top, ci, c0, sd, eci, e0, ti, t;
  const uint8_t *vb, *ve, *ob, *oe, *gb, *ge;

  nc_der_reader_init(&top, token, token_len);
  if (nc_der_read(&top, &vb, &ve) != NC_ASN1_SEQUENCE) return; /* ContentInfo */
  nc_der_reader_init(&ci, vb, (size_t)(ve - vb));
  if (nc_der_read(&ci, &ob, &oe) != NC_ASN1_OID) return;       /* contentType */
  if (nc_der_read(&ci, &vb, &ve) != TAG_CONTEXT0_C) return;    /* [0] */
  nc_der_reader_init(&c0, vb, (size_t)(ve - vb));
  if (nc_der_read(&c0, &vb, &ve) != NC_ASN1_SEQUENCE) return;  /* SignedData */
  nc_der_reader_init(&sd, vb, (size_t)(ve - vb));
  nc_der_read(&sd, &vb, &ve); /* version */
  nc_der_read(&sd, &vb, &ve); /* digestAlgorithms */
  if (nc_der_read(&sd, &vb, &ve) != NC_ASN1_SEQUENCE) return;  /* encapCI */
  nc_der_reader_init(&eci, vb, (size_t)(ve - vb));
  nc_der_read(&eci, &ob, &oe); /* eContentType */
  if (nc_der_read(&eci, &vb, &ve) != TAG_CONTEXT0_C) return;   /* [0] */
  nc_der_reader_init(&e0, vb, (size_t)(ve - vb));
  if (nc_der_read(&e0, &vb, &ve) != NC_ASN1_OCTET_STRING) return; /* TSTInfo */
  nc_der_reader_init(&ti, vb, (size_t)(ve - vb));
  if (nc_der_read(&ti, &vb, &ve) != NC_ASN1_SEQUENCE) return;  /* TSTInfo SEQ */
  nc_der_reader_init(&t, vb, (size_t)(ve - vb));
  nc_der_read(&t, &vb, &ve); /* version */
  nc_der_read(&t, &vb, &ve); /* policy OID */
  nc_der_read(&t, &vb, &ve); /* messageImprint */
  nc_der_read(&t, &vb, &ve); /* serialNumber */
  if (nc_der_read(&t, &gb, &ge) == NC_ASN1_GENERALIZEDTIME) {
    size_t n = (size_t)(ge - gb);
    if (n >= gen_cap) n = gen_cap - 1;
    memcpy(gen_time, gb, n);
    gen_time[n] = 0;
  }
  /* TSA name: first certificate's subject CN (scan the [0] certs in sd). */
  while (sd.p < sd.end && sd.ok) {
    uint8_t tag = nc_der_read(&sd, &vb, &ve);
    if (tag == TAG_CONTEXT0_C) {
      nc_der_reader cr;
      const uint8_t* cstart;
      const uint8_t *cb, *ce2;
      nc_der_reader_init(&cr, vb, (size_t)(ve - vb));
      cstart = cr.p;
      if (nc_der_read(&cr, &cb, &ce2) == NC_ASN1_SEQUENCE) {
        nc_x509_cert cert;
        if (nc_x509_parse(&cert, cstart, (size_t)(cr.p - cstart)) == 0)
          nc_x509_subject_cn(&cert, tsa, tsa_cap);
      }
      break;
    } else if (tag == NC_ASN1_SET || tag == 0) {
      break;
    }
  }
}

int nc_cms_verify_signed_data(nc_cms_verify_info* info, const uint8_t* cms_der,
                              size_t cms_len, const uint8_t* content,
                              size_t content_len) {
  nc_der_reader top, ci, c0, sd, sis, si;
  const uint8_t *vb, *ve, *ob, *oe;
  const uint8_t *si_b = NULL, *si_e = NULL;
  const uint8_t* certs_ptr[32];
  size_t certs_len[32];
  int certs_n = 0;
  const uint8_t *sid_b, *sid_e;
  const uint8_t *sa_start, *sa_b, *sa_e, *sa_end;
  const uint8_t *sig_b, *sig_e;
  const uint8_t *ua_b = NULL, *ua_e = NULL;
  const uint8_t *mv_b, *mv_e, *tv_b, *tv_e;
  uint8_t oidbuf[16];
  size_t oidlen;
  const uint8_t* signer_cert = NULL;
  size_t signer_cert_len = 0;
  int i;

  if (!info) return 1;
  memset(info, 0, sizeof(*info));

  nc_der_reader_init(&top, cms_der, cms_len);
  if (nc_der_read(&top, &vb, &ve) != NC_ASN1_SEQUENCE) {
    strcpy(info->error, "not DER SEQUENCE");
    return 1;
  }
  nc_der_reader_init(&ci, vb, (size_t)(ve - vb));
  if (nc_der_read(&ci, &ob, &oe) != NC_ASN1_OID) {
    strcpy(info->error, "no contentType");
    return 1;
  }
  if (nc_der_read(&ci, &vb, &ve) != TAG_CONTEXT0_C) {
    strcpy(info->error, "no content [0]");
    return 1;
  }
  nc_der_reader_init(&c0, vb, (size_t)(ve - vb));
  if (nc_der_read(&c0, &vb, &ve) != NC_ASN1_SEQUENCE) {
    strcpy(info->error, "no SignedData");
    return 1;
  }
  nc_der_reader_init(&sd, vb, (size_t)(ve - vb));
  nc_der_read(&sd, &vb, &ve); /* version */
  nc_der_read(&sd, &vb, &ve); /* digestAlgorithms */
  nc_der_read(&sd, &vb, &ve); /* encapContentInfo */

  while (sd.p < sd.end && sd.ok) {
    uint8_t t = nc_der_read(&sd, &vb, &ve);
    if (t == TAG_CONTEXT0_C) { /* certificates */
      nc_der_reader cr;
      const uint8_t *cb, *ce2;
      nc_der_reader_init(&cr, vb, (size_t)(ve - vb));
      while (cr.p < cr.end && cr.ok && certs_n < 32) {
        const uint8_t* cstart = cr.p;
        if (nc_der_read(&cr, &cb, &ce2) != NC_ASN1_SEQUENCE) break;
        certs_ptr[certs_n] = cstart;
        certs_len[certs_n] = (size_t)(cr.p - cstart);
        certs_n++;
      }
    } else if (t == NC_ASN1_SET) { /* signerInfos */
      si_b = vb;
      si_e = ve;
      break;
    } else if (t == 0) {
      break;
    }
  }
  if (!si_b) {
    strcpy(info->error, "no signerInfos");
    return 1;
  }
  info->parsed = 1;

  nc_der_reader_init(&sis, si_b, (size_t)(si_e - si_b));
  if (nc_der_read(&sis, &vb, &ve) != NC_ASN1_SEQUENCE) {
    strcpy(info->error, "bad SignerInfo");
    return 0;
  }
  nc_der_reader_init(&si, vb, (size_t)(ve - vb));
  nc_der_read(&si, &vb, &ve);          /* version */
  nc_der_read(&si, &sid_b, &sid_e);    /* sid */
  nc_der_read(&si, &vb, &ve);          /* digestAlgorithm */
  strcpy(info->digest_algorithm, "SHA-256");

  sa_start = si.p;
  if (nc_der_read(&si, &sa_b, &sa_e) != TAG_CONTEXT0_C) {
    strcpy(info->error, "no signedAttrs");
    return 0;
  }
  sa_end = si.p;
  nc_der_read(&si, &vb, &ve); /* signatureAlgorithm */
  if (nc_der_read(&si, &sig_b, &sig_e) != NC_ASN1_OCTET_STRING) {
    strcpy(info->error, "no signature");
    return 0;
  }
  if (si.p < si.end) {
    const uint8_t* save = si.p;
    if (nc_der_read(&si, &vb, &ve) == TAG_CONTEXT1_C) {
      ua_b = vb;
      ua_e = ve;
    } else {
      si.p = save;
    }
  }

  /* messageDigest attr == SHA-256(content) */
  oid_val(OID_MESSAGE_DIGEST, oidbuf, &oidlen);
  if (find_attr_value(sa_b, sa_e, oidbuf, oidlen, &mv_b, &mv_e)) {
    nc_der_reader mr;
    const uint8_t *xb, *xe;
    nc_der_reader_init(&mr, mv_b, (size_t)(mv_e - mv_b));
    if (nc_der_read(&mr, &xb, &xe) == NC_ASN1_OCTET_STRING &&
        (xe - xb) == NC_SHA256_LEN) {
      uint8_t h[NC_SHA256_LEN];
      nc_sha256(content, content_len, h);
      info->digest_valid = memcmp(h, xb, NC_SHA256_LEN) == 0;
    }
  }

  /* signingTime */
  oid_val(OID_SIGNING_TIME, oidbuf, &oidlen);
  if (find_attr_value(sa_b, sa_e, oidbuf, oidlen, &tv_b, &tv_e)) {
    nc_der_reader sr;
    const uint8_t *xb, *xe;
    uint8_t tag;
    nc_der_reader_init(&sr, tv_b, (size_t)(tv_e - tv_b));
    tag = nc_der_read(&sr, &xb, &xe);
    if (tag == NC_ASN1_UTCTIME || tag == NC_ASN1_GENERALIZEDTIME) {
      size_t n = (size_t)(xe - xb);
      if (n >= sizeof(info->signing_time)) n = sizeof(info->signing_time) - 1;
      memcpy(info->signing_time, xb, n);
      info->signing_time[n] = 0;
    }
  }

  /* Pick the signer certificate (match by IssuerAndSerialNumber; else first). */
  {
    nc_der_reader sidr, s2;
    const uint8_t *ib, *ie;
    const uint8_t* want_issuer = NULL;
    size_t want_issuer_len = 0;
    const uint8_t* want_serial = NULL;
    size_t want_serial_len = 0;
    nc_der_reader_init(&sidr, sid_b, (size_t)(sid_e - sid_b));
    if (nc_der_read(&sidr, &ib, &ie) == NC_ASN1_SEQUENCE) {
      const uint8_t* iss_start;
      const uint8_t *xb, *xe;
      nc_der_reader_init(&s2, ib, (size_t)(ie - ib));
      iss_start = s2.p;
      if (nc_der_read(&s2, &xb, &xe) == NC_ASN1_SEQUENCE) {
        want_issuer = iss_start;
        want_issuer_len = (size_t)(s2.p - iss_start);
      }
      if (nc_der_read(&s2, &xb, &xe) == NC_ASN1_INTEGER) {
        want_serial = xb;
        want_serial_len = (size_t)(xe - xb);
      }
    }
    for (i = 0; i < certs_n; ++i) {
      cert_info got = parse_cert_info(certs_ptr[i], certs_len[i]);
      if (got.valid && want_issuer && got.issuer_len == want_issuer_len &&
          memcmp(got.issuer, want_issuer, want_issuer_len) == 0 &&
          got.serial_len == want_serial_len &&
          memcmp(got.serial, want_serial, want_serial_len) == 0) {
        signer_cert = certs_ptr[i];
        signer_cert_len = certs_len[i];
        break;
      }
    }
    if (!signer_cert && certs_n > 0) {
      signer_cert = certs_ptr[0];
      signer_cert_len = certs_len[0];
    }
  }

  /* signer CN + RSA public key via nc_x509_parse. */
  if (signer_cert) {
    nc_x509_cert cert;
    if (nc_x509_parse(&cert, signer_cert, signer_cert_len) == 0) {
      nc_x509_subject_cn(&cert, info->signer_cn, sizeof(info->signer_cn));
      if (cert.key_type == NC_KEY_RSA && cert.rsa_pub.valid) {
        /* RSA-verify the signature over signedAttrs re-encoded as a SET. */
        size_t sa_len = (size_t)(sa_end - sa_start);
        uint8_t* sa = (uint8_t*)malloc(sa_len ? sa_len : 1);
        if (sa) {
          uint8_t h[NC_SHA256_LEN];
          uint8_t di[sizeof(kSha256DiPrefix) + NC_SHA256_LEN];
          memcpy(sa, sa_start, sa_len);
          if (sa_len > 0) sa[0] = NC_ASN1_SET; /* [0] IMPLICIT -> SET */
          nc_sha256(sa, sa_len, h);
          memcpy(di, kSha256DiPrefix, sizeof(kSha256DiPrefix));
          memcpy(di + sizeof(kSha256DiPrefix), h, NC_SHA256_LEN);
          info->signature_valid = nc_rsa_verify_pkcs1v15(
              &cert.rsa_pub, sig_b, (size_t)(sig_e - sig_b), di, sizeof(di));
          free(sa);
        }
      }
    }
  }

  /* RFC 3161 signature timestamp (unsigned attribute). */
  if (ua_b) {
    const uint8_t *zb, *ze;
    oid_val(OID_TIMESTAMP_TOKEN, oidbuf, &oidlen);
    if (find_attr_value(ua_b, ua_e, oidbuf, oidlen, &zb, &ze)) {
      info->has_timestamp = 1;
      parse_tst(zb, (size_t)(ze - zb), info->timestamp_time,
                sizeof(info->timestamp_time), info->timestamp_authority,
                sizeof(info->timestamp_authority));
    }
  }
  return 0;
}
