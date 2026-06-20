/* SPDX-License-Identifier: Apache-2.0
 * Minimal ASN.1 DER encoders + TLV reader. Ported from the C++ asn1-der.cc. */
#include "ncrypto/nc_asn1.h"

#include <stdlib.h>
#include <string.h>

/* ---- growable byte buffer ------------------------------------------------- */
void nc_buf_init(nc_buf* b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  b->err = 0;
}

void nc_buf_free(nc_buf* b) {
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  b->err = 0;
}

static int nc_buf_reserve(nc_buf* b, size_t need) {
  size_t cap;
  uint8_t* p;
  if (b->err) return 0;
  if (need <= b->cap) return 1;
  cap = b->cap ? b->cap : 16;
  while (cap < need) {
    size_t next = cap * 2;
    if (next < cap) { /* overflow */
      b->err = 1;
      return 0;
    }
    cap = next;
  }
  p = (uint8_t*)realloc(b->data, cap);
  if (!p) {
    b->err = 1;
    return 0;
  }
  b->data = p;
  b->cap = cap;
  return 1;
}

void nc_buf_put(nc_buf* b, const uint8_t* p, size_t n) {
  if (b->err) return;
  if (n == 0) return;
  if (!nc_buf_reserve(b, b->len + n)) return;
  memcpy(b->data + b->len, p, n);
  b->len += n;
}

void nc_buf_putc(nc_buf* b, uint8_t c) {
  if (b->err) return;
  if (!nc_buf_reserve(b, b->len + 1)) return;
  b->data[b->len++] = c;
}

/* ---- DER encoders --------------------------------------------------------- */
void nc_der_len(nc_buf* out, size_t len) {
  if (len < 0x80) {
    nc_buf_putc(out, (uint8_t)len);
    return;
  }
  /* long form: collect big-endian bytes */
  uint8_t tmp[sizeof(size_t)];
  size_t n = 0;
  size_t v = len;
  while (v) {
    tmp[n++] = (uint8_t)(v & 0xFF);
    v >>= 8;
  }
  nc_buf_putc(out, (uint8_t)(0x80 | n));
  while (n) {
    nc_buf_putc(out, tmp[--n]);
  }
}

void nc_der_tlv(nc_buf* out, uint8_t tag, const uint8_t* val, size_t len) {
  nc_buf_putc(out, tag);
  nc_der_len(out, len);
  if (len) nc_buf_put(out, val, len);
}

void nc_der_integer(nc_buf* out, const uint8_t* mag_be, size_t len) {
  size_t i = 0;
  nc_buf content;
  /* trim leading zero bytes, keeping at least one byte */
  while (i + 1 < len && mag_be[i] == 0) ++i;
  nc_buf_init(&content);
  if (len == 0) {
    nc_buf_putc(&content, 0x00);
  } else {
    if (mag_be[i] & 0x80) nc_buf_putc(&content, 0x00);
    nc_buf_put(&content, mag_be + i, len - i);
  }
  nc_der_tlv(out, NC_ASN1_INTEGER, content.data, content.len);
  if (content.err) out->err = 1;
  nc_buf_free(&content);
}

void nc_der_uint(nc_buf* out, uint32_t v) {
  uint8_t be[4];
  size_t n = 0;
  int s;
  for (s = 24; s >= 0; s -= 8) {
    uint8_t b = (uint8_t)((v >> s) & 0xFF);
    if (n != 0 || b || s == 0) be[n++] = b;
  }
  nc_der_integer(out, be, n);
}

void nc_der_oid(nc_buf* out, const char* dotted) {
  uint64_t arcs[64];
  size_t narcs = 0;
  uint64_t cur = 0;
  int have = 0;
  const char* c;
  nc_buf content;

  for (c = dotted; *c; ++c) {
    if (*c == '.') {
      if (narcs < 64) arcs[narcs++] = cur;
      cur = 0;
      have = 0;
    } else if (*c >= '0' && *c <= '9') {
      cur = cur * 10 + (uint64_t)(*c - '0');
      have = 1;
    }
  }
  if (have && narcs < 64) arcs[narcs++] = cur;

  nc_buf_init(&content);
  if (narcs >= 2) {
    size_t i;
    nc_buf_putc(&content, (uint8_t)(40 * arcs[0] + arcs[1]));
    for (i = 2; i < narcs; ++i) {
      uint64_t v = arcs[i];
      uint8_t tmp[10];
      size_t n = 0;
      tmp[n++] = (uint8_t)(v & 0x7F);
      v >>= 7;
      while (v) {
        tmp[n++] = (uint8_t)((v & 0x7F) | 0x80);
        v >>= 7;
      }
      while (n) nc_buf_putc(&content, tmp[--n]); /* big-endian base128 */
    }
  }
  nc_der_tlv(out, NC_ASN1_OID, content.data, content.len);
  if (content.err) out->err = 1;
  nc_buf_free(&content);
}

void nc_der_octet_string(nc_buf* out, const uint8_t* p, size_t n) {
  nc_der_tlv(out, NC_ASN1_OCTET_STRING, p, n);
}

void nc_der_bit_string(nc_buf* out, const uint8_t* p, size_t n) {
  nc_buf_putc(out, NC_ASN1_BIT_STRING);
  nc_der_len(out, n + 1);
  nc_buf_putc(out, 0x00); /* unused bits */
  if (n) nc_buf_put(out, p, n);
}

void nc_der_null(nc_buf* out) {
  nc_buf_putc(out, NC_ASN1_NULL);
  nc_buf_putc(out, 0x00);
}

void nc_der_bool(nc_buf* out, int v) {
  nc_buf_putc(out, NC_ASN1_BOOLEAN);
  nc_buf_putc(out, 0x01);
  nc_buf_putc(out, v ? 0xFF : 0x00);
}

void nc_der_utctime(nc_buf* out, const char* s) {
  nc_der_tlv(out, NC_ASN1_UTCTIME, (const uint8_t*)s, strlen(s));
}

void nc_der_generalizedtime(nc_buf* out, const char* s) {
  nc_der_tlv(out, NC_ASN1_GENERALIZEDTIME, (const uint8_t*)s, strlen(s));
}

/* ---- TLV reader ----------------------------------------------------------- */
void nc_der_reader_init(nc_der_reader* r, const uint8_t* data, size_t len) {
  r->p = data;
  r->end = data + len;
  r->ok = 1;
}

uint8_t nc_der_read(nc_der_reader* r, const uint8_t** vbeg, const uint8_t** vend) {
  uint8_t tag;
  size_t len;

  if (!r->ok) return 0;
  if (r->p >= r->end) {
    r->ok = 0;
    return 0;
  }
  tag = *r->p++;

  if (r->p >= r->end) {
    r->ok = 0;
    return 0;
  }
  {
    uint8_t b = *r->p++;
    if (b < 0x80) {
      len = b;
    } else {
      size_t nbytes = (size_t)(b & 0x7F);
      size_t i;
      if (nbytes == 0 || nbytes > sizeof(size_t)) {
        r->ok = 0;
        return 0;
      }
      if ((size_t)(r->end - r->p) < nbytes) {
        r->ok = 0;
        return 0;
      }
      len = 0;
      for (i = 0; i < nbytes; ++i) {
        len = (len << 8) | (size_t)(*r->p++);
      }
    }
  }

  if ((size_t)(r->end - r->p) < len) {
    r->ok = 0;
    return 0;
  }
  if (vbeg) *vbeg = r->p;
  if (vend) *vend = r->p + len;
  r->p += len;
  return tag;
}
