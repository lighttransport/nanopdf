/* SPDX-License-Identifier: Apache-2.0
 * RFC 3161 timestamping: build a TimeStampReq and parse a TimeStampResp. */
#include "ncrypto/nc_rfc3161.h"

#include <string.h>

const char* nc_rfc3161_hash_oid(const char* name) {
  if (!name) return NULL;
  if (strcmp(name, "sha256") == 0) return "2.16.840.1.101.3.4.2.1";
  if (strcmp(name, "sha384") == 0) return "2.16.840.1.101.3.4.2.2";
  if (strcmp(name, "sha512") == 0) return "2.16.840.1.101.3.4.2.3";
  if (strcmp(name, "sha1") == 0) return "1.3.14.3.2.26";
  return NULL;
}

int nc_rfc3161_build_request(nc_buf* out, const uint8_t* imprint,
                             size_t imprint_len, const char* hash_oid_dotted,
                             uint64_t nonce, int cert_req) {
  nc_buf algid, imprint_seq, body;
  uint8_t nonce_be[8];
  int i;

  /* AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters NULL } */
  nc_buf_init(&algid);
  nc_der_oid(&algid, hash_oid_dotted);
  nc_der_null(&algid);

  /* MessageImprint ::= SEQUENCE { hashAlgorithm AlgorithmIdentifier,
   *                               hashedMessage OCTET STRING } */
  nc_buf_init(&imprint_seq);
  nc_der_tlv(&imprint_seq, NC_ASN1_SEQUENCE, algid.data, algid.len);
  nc_der_octet_string(&imprint_seq, imprint, imprint_len);

  /* TimeStampReq ::= SEQUENCE { version INTEGER(1), messageImprint,
   *                             nonce INTEGER OPTIONAL, certReq BOOLEAN } */
  nc_buf_init(&body);
  nc_der_uint(&body, 1);
  nc_der_tlv(&body, NC_ASN1_SEQUENCE, imprint_seq.data, imprint_seq.len);

  if (nonce != 0) {
    for (i = 0; i < 8; i++)
      nonce_be[i] = (uint8_t)((nonce >> (56 - 8 * i)) & 0xFF);
    nc_der_integer(&body, nonce_be, 8);
  }
  if (cert_req) nc_der_bool(&body, 1);

  nc_der_tlv(out, NC_ASN1_SEQUENCE, body.data, body.len);

  nc_buf_free(&algid);
  nc_buf_free(&imprint_seq);
  nc_buf_free(&body);
  return 0;
}

/* Read one TLV: returns tag, sets [*vbeg,*vend) to the value, advances reader. */
int nc_rfc3161_parse_response(const uint8_t* tsr, size_t tsr_len, int* status,
                              nc_buf* token_out) {
  nc_der_reader top, resp, st;
  const uint8_t *vb, *ve;  /* TimeStampResp value */
  const uint8_t *sb, *se;  /* PKIStatusInfo value */
  const uint8_t *ib, *ie;  /* status INTEGER value */
  const uint8_t *tok_start, *tb, *te;
  int s = 0;
  const uint8_t* p;

  if (status) *status = 0;

  /* TimeStampResp ::= SEQUENCE { ... } */
  nc_der_reader_init(&top, tsr, tsr_len);
  if (nc_der_read(&top, &vb, &ve) != NC_ASN1_SEQUENCE) return -1;

  /* status PKIStatusInfo ::= SEQUENCE { status INTEGER, ... } */
  nc_der_reader_init(&resp, vb, (size_t)(ve - vb));
  if (nc_der_read(&resp, &sb, &se) != NC_ASN1_SEQUENCE) return -1;

  nc_der_reader_init(&st, sb, (size_t)(se - sb));
  if (nc_der_read(&st, &ib, &ie) != NC_ASN1_INTEGER) return -1;
  for (p = ib; p < ie; ++p) s = (s << 8) | *p;
  if (status) *status = s;

  /* timeStampToken ContentInfo OPTIONAL: the next element is a SEQUENCE.
   * Append its full DER (tag..value) when present. */
  tok_start = resp.p;
  if (nc_der_read(&resp, &tb, &te) == NC_ASN1_SEQUENCE) {
    if (token_out) nc_buf_put(token_out, tok_start, (size_t)(te - tok_start));
  }
  return 0;
}
