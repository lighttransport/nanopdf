/* SPDX-License-Identifier: Apache-2.0
 * Tests for the ASN.1 DER module. */
#include "ncrypto/nc_asn1.h"
#include "ncrypto/nc_test.h"

static void check_one(uint32_t v, const char* expect) {
  nc_buf b;
  nc_buf_init(&b);
  nc_der_uint(&b, v);
  NC_CHECK_EQ_HEX(b.data, b.len, expect);
  nc_buf_free(&b);
}

int main(void) {
  /* nc_der_uint */
  check_one(0, "020100");
  check_one(255, "020200ff");
  check_one(0x010001, "0203010001");

  /* nc_der_null */
  {
    nc_buf b;
    nc_buf_init(&b);
    nc_der_null(&b);
    NC_CHECK_EQ_HEX(b.data, b.len, "0500");
    nc_buf_free(&b);
  }

  /* nc_der_bool */
  {
    nc_buf b;
    nc_buf_init(&b);
    nc_der_bool(&b, 1);
    NC_CHECK_EQ_HEX(b.data, b.len, "0101ff");
    nc_buf_free(&b);
  }

  /* nc_der_oid */
  {
    nc_buf b;
    nc_buf_init(&b);
    nc_der_oid(&b, "1.2.840.113549.1.1.11");
    NC_CHECK_EQ_HEX(b.data, b.len, "06092a864886f70d01010b");
    nc_buf_free(&b);
  }
  {
    nc_buf b;
    nc_buf_init(&b);
    nc_der_oid(&b, "2.16.840.1.101.3.4.2.1");
    NC_CHECK_EQ_HEX(b.data, b.len, "0609608648016503040201");
    nc_buf_free(&b);
  }

  /* nc_der_octet_string */
  {
    nc_buf b;
    uint8_t data[4];
    size_t n = nc_test_from_hex("deadbeef", data);
    nc_buf_init(&b);
    nc_der_octet_string(&b, data, n);
    NC_CHECK_EQ_HEX(b.data, b.len, "0404deadbeef");
    nc_buf_free(&b);
  }

  /* SEQUENCE wrapping [uint(1) ++ null] */
  {
    nc_buf inner, outer;
    nc_buf_init(&inner);
    nc_der_uint(&inner, 1);
    nc_der_null(&inner);
    NC_CHECK_EQ_HEX(inner.data, inner.len, "0201010500");
    nc_buf_init(&outer);
    nc_der_tlv(&outer, NC_ASN1_SEQUENCE, inner.data, inner.len);
    NC_CHECK_EQ_HEX(outer.data, outer.len, "30050201010500");
    nc_buf_free(&inner);
    nc_buf_free(&outer);
  }

  /* Reader round-trip */
  {
    uint8_t bytes[16];
    size_t n = nc_test_from_hex("30050201010500", bytes);
    nc_der_reader r, sub;
    const uint8_t *vbeg, *vend;
    uint8_t tag;

    nc_der_reader_init(&r, bytes, n);
    tag = nc_der_read(&r, &vbeg, &vend);
    NC_CHECK(r.ok);
    NC_CHECK(tag == 0x30);
    NC_CHECK((size_t)(vend - vbeg) == 5);

    nc_der_reader_init(&sub, vbeg, (size_t)(vend - vbeg));
    tag = nc_der_read(&sub, &vbeg, &vend);
    NC_CHECK(sub.ok);
    NC_CHECK(tag == 0x02);
    NC_CHECK((size_t)(vend - vbeg) == 1);
    NC_CHECK(vbeg[0] == 0x01);

    tag = nc_der_read(&sub, &vbeg, &vend);
    NC_CHECK(sub.ok);
    NC_CHECK(tag == 0x05);
    NC_CHECK((size_t)(vend - vbeg) == 0);
  }

  return nc_test_report();
}
