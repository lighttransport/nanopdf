/* SPDX-License-Identifier: Apache-2.0 */
#include "ncrypto/nc_rfc3161.h"

#include <string.h>

#include "ncrypto/nc_asn1.h"
#include "ncrypto/nc_test.h"

static void test_build_request(void) {
  uint8_t imprint[32];
  size_t imprint_len;
  nc_buf out;
  const char* oid;
  nc_der_reader top, body, mi, algid;
  const uint8_t *vb, *ve;   /* TimeStampReq body */
  const uint8_t *ib, *ie;   /* version INTEGER */
  const uint8_t *mb, *me;   /* messageImprint SEQUENCE */
  const uint8_t *ab, *ae;   /* algid SEQUENCE */
  const uint8_t *ob, *oe;   /* hashedMessage OCTET STRING */
  uint8_t tag;

  imprint_len = nc_test_from_hex(
      "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20",
      imprint);
  NC_CHECK(imprint_len == 32);
  oid = nc_rfc3161_hash_oid("sha256");
  NC_CHECK(oid != NULL);

  nc_buf_init(&out);
  NC_CHECK(nc_rfc3161_build_request(&out, imprint, imprint_len, oid,
                                    0x1122334455667788ULL, 1) == 0);
  NC_CHECK(out.len > 0);
  NC_CHECK(out.data[0] == 0x30); /* outer SEQUENCE */

  /* Re-parse the output. */
  nc_der_reader_init(&top, out.data, out.len);
  tag = nc_der_read(&top, &vb, &ve);
  NC_CHECK(tag == NC_ASN1_SEQUENCE);

  nc_der_reader_init(&body, vb, (size_t)(ve - vb));
  /* version INTEGER == 1 */
  tag = nc_der_read(&body, &ib, &ie);
  NC_CHECK(tag == NC_ASN1_INTEGER);
  NC_CHECK((ie - ib) == 1);
  NC_CHECK(ib[0] == 0x01);

  /* messageImprint SEQUENCE */
  tag = nc_der_read(&body, &mb, &me);
  NC_CHECK(tag == NC_ASN1_SEQUENCE);

  nc_der_reader_init(&mi, mb, (size_t)(me - mb));
  /* algid SEQUENCE */
  tag = nc_der_read(&mi, &ab, &ae);
  NC_CHECK(tag == NC_ASN1_SEQUENCE);

  /* algid first child is an OID */
  nc_der_reader_init(&algid, ab, (size_t)(ae - ab));
  {
    const uint8_t *xb, *xe;
    tag = nc_der_read(&algid, &xb, &xe);
    NC_CHECK(tag == NC_ASN1_OID);
    /* sha256 OID DER (full TLV): 0609 608648016503040201 */
    NC_CHECK_EQ_HEX(xb - 2, (size_t)(xe - xb) + 2, "0609608648016503040201");
  }

  /* hashedMessage OCTET STRING of length 32 == imprint */
  tag = nc_der_read(&mi, &ob, &oe);
  NC_CHECK(tag == NC_ASN1_OCTET_STRING);
  NC_CHECK((oe - ob) == 32);
  NC_CHECK(memcmp(ob, imprint, 32) == 0);

  nc_buf_free(&out);
}

static void test_parse_response_granted(void) {
  nc_buf statusinfo, token, body, resp;
  int status = -1;
  nc_buf token_out;

  /* PKIStatusInfo ::= SEQUENCE { INTEGER 0 } */
  nc_buf_init(&statusinfo);
  nc_der_uint(&statusinfo, 0);

  /* dummy token: SEQUENCE { OID, NULL } */
  nc_buf_init(&token);
  nc_der_oid(&token, "1.2.840.113549.1.7.2");
  nc_der_null(&token);

  nc_buf_init(&body);
  nc_der_tlv(&body, NC_ASN1_SEQUENCE, statusinfo.data, statusinfo.len);
  nc_der_tlv(&body, NC_ASN1_SEQUENCE, token.data, token.len);

  nc_buf_init(&resp);
  nc_der_tlv(&resp, NC_ASN1_SEQUENCE, body.data, body.len);

  nc_buf_init(&token_out);
  NC_CHECK(nc_rfc3161_parse_response(resp.data, resp.len, &status,
                                     &token_out) == 0);
  NC_CHECK(status == 0);
  NC_CHECK(token_out.len > 0);
  NC_CHECK(token_out.data[0] == 0x30);

  nc_buf_free(&statusinfo);
  nc_buf_free(&token);
  nc_buf_free(&body);
  nc_buf_free(&resp);
  nc_buf_free(&token_out);
}

static void test_parse_response_rejected(void) {
  nc_buf statusinfo, body, resp;
  int status = -1;
  nc_buf token_out;

  /* PKIStatusInfo ::= SEQUENCE { INTEGER 2 } (rejection) */
  nc_buf_init(&statusinfo);
  nc_der_uint(&statusinfo, 2);

  nc_buf_init(&body);
  nc_der_tlv(&body, NC_ASN1_SEQUENCE, statusinfo.data, statusinfo.len);

  nc_buf_init(&resp);
  nc_der_tlv(&resp, NC_ASN1_SEQUENCE, body.data, body.len);

  nc_buf_init(&token_out);
  NC_CHECK(nc_rfc3161_parse_response(resp.data, resp.len, &status,
                                     &token_out) == 0);
  NC_CHECK(status == 2);
  NC_CHECK(token_out.len == 0);

  nc_buf_free(&statusinfo);
  nc_buf_free(&body);
  nc_buf_free(&resp);
  nc_buf_free(&token_out);
}

int main(void) {
  test_build_request();
  test_parse_response_granted();
  test_parse_response_rejected();
  return nc_test_report();
}
