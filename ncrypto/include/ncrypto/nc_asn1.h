/* SPDX-License-Identifier: Apache-2.0
 * Minimal ASN.1 DER: a growable byte buffer + DER encoders (for building CMS /
 * RFC 3161 structures) and a lightweight TLV reader (shared by X.509, CMS,
 * PKCS#12, RFC 3161 and the TLS client). */
#ifndef NCRYPTO_NC_ASN1_H_
#define NCRYPTO_NC_ASN1_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DER tag bytes used across the library. */
#define NC_ASN1_BOOLEAN 0x01
#define NC_ASN1_INTEGER 0x02
#define NC_ASN1_BIT_STRING 0x03
#define NC_ASN1_OCTET_STRING 0x04
#define NC_ASN1_NULL 0x05
#define NC_ASN1_OID 0x06
#define NC_ASN1_UTF8STRING 0x0C
#define NC_ASN1_PRINTABLESTRING 0x13
#define NC_ASN1_UTCTIME 0x17
#define NC_ASN1_GENERALIZEDTIME 0x18
#define NC_ASN1_SEQUENCE 0x30
#define NC_ASN1_SET 0x31

/* ---- growable byte buffer ------------------------------------------------- */
typedef struct {
  uint8_t* data;
  size_t len;
  size_t cap;
  int err;  /* set on allocation failure */
} nc_buf;

void nc_buf_init(nc_buf* b);
void nc_buf_free(nc_buf* b);
void nc_buf_put(nc_buf* b, const uint8_t* p, size_t n);
void nc_buf_putc(nc_buf* b, uint8_t c);

/* ---- DER encoders (append to @out) ---------------------------------------- */
/* Encode just the length octets for @len. */
void nc_der_len(nc_buf* out, size_t len);
/* tag || len || value (value may be NULL when len==0). */
void nc_der_tlv(nc_buf* out, uint8_t tag, const uint8_t* val, size_t len);
/* INTEGER from an unsigned big-endian magnitude (a leading 0x00 is inserted if
 * the top bit is set; leading zero bytes are trimmed). */
void nc_der_integer(nc_buf* out, const uint8_t* mag_be, size_t len);
void nc_der_uint(nc_buf* out, uint32_t v);
/* OBJECT IDENTIFIER from a dotted string, e.g. "1.2.840.113549.1.1.11". */
void nc_der_oid(nc_buf* out, const char* dotted);
void nc_der_octet_string(nc_buf* out, const uint8_t* p, size_t n);
/* BIT STRING with 0 unused bits. */
void nc_der_bit_string(nc_buf* out, const uint8_t* p, size_t n);
void nc_der_null(nc_buf* out);
void nc_der_bool(nc_buf* out, int v);
void nc_der_utctime(nc_buf* out, const char* s);          /* "YYMMDDhhmmssZ" */
void nc_der_generalizedtime(nc_buf* out, const char* s);  /* "YYYYMMDD..Z" */

/* ---- TLV reader ----------------------------------------------------------- */
typedef struct {
  const uint8_t* p;
  const uint8_t* end;
  int ok;
} nc_der_reader;

void nc_der_reader_init(nc_der_reader* r, const uint8_t* data, size_t len);
/* Read one TLV: returns the tag and sets [*vbeg,*vend) to the value; advances
 * past the value so siblings can be read. Returns 0 and clears r->ok on error. */
uint8_t nc_der_read(nc_der_reader* r, const uint8_t** vbeg, const uint8_t** vend);

#ifdef __cplusplus
}
#endif

#endif /* NCRYPTO_NC_ASN1_H_ */
