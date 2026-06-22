/* SPDX-License-Identifier: Apache-2.0
 * Minimal TLS 1.3 client (TLS_AES_128_GCM_SHA256, X25519) in C11. Ported from
 * examples/pdfview/tls_client.cc. POSIX sockets. */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200112L
#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "ncrypto/nc_tls.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ncrypto/nc_aes.h"
#include "ncrypto/nc_asn1.h"
#include "ncrypto/nc_hash.h"
#include "ncrypto/nc_kdf.h"
#include "ncrypto/nc_x25519.h"
#include "ncrypto/nc_x509.h"

/* ---- small helpers -------------------------------------------------------- */

static void set_err(char* err, size_t cap, const char* msg) {
  if (err && cap) {
    size_t n = strlen(msg);
    if (n >= cap) n = cap - 1;
    memcpy(err, msg, n);
    err[n] = 0;
  }
}

static void put16(nc_buf* b, uint16_t v) {
  nc_buf_putc(b, (uint8_t)(v >> 8));
  nc_buf_putc(b, (uint8_t)(v & 0xff));
}
static void put24(nc_buf* b, uint32_t v) {
  nc_buf_putc(b, (uint8_t)((v >> 16) & 0xff));
  nc_buf_putc(b, (uint8_t)((v >> 8) & 0xff));
  nc_buf_putc(b, (uint8_t)(v & 0xff));
}

static int rand_bytes(uint8_t* p, size_t n) {
  FILE* f = fopen("/dev/urandom", "rb");
  if (!f) return -1;
  size_t r = fread(p, 1, n, f);
  fclose(f);
  return r == n ? 0 : -1;
}

static int send_all(int fd, const uint8_t* p, size_t n) {
  size_t s = 0;
  while (s < n) {
    ssize_t k = send(fd, p + s, n - s, 0);
    if (k <= 0) return 0;
    s += (size_t)k;
  }
  return 1;
}
static int recv_n(int fd, uint8_t* p, size_t n) {
  size_t r = 0;
  while (r < n) {
    ssize_t k = recv(fd, p + r, n - r, 0);
    if (k <= 0) return 0;
    r += (size_t)k;
  }
  return 1;
}

/* derive_secret(secret, label, thash) -> 32 bytes out. */
static void derive_secret(const uint8_t secret[32], const char* label,
                          const uint8_t* thash, size_t thash_len,
                          uint8_t out[32]) {
  nc_hkdf_expand_label(NC_PRF_SHA256, secret, 32, label, thash, thash_len, out,
                       32);
}

static void key_iv_from(const uint8_t ts[32], uint8_t key[16], uint8_t iv[12]) {
  nc_hkdf_expand_label(NC_PRF_SHA256, ts, 32, "key", NULL, 0, key, 16);
  nc_hkdf_expand_label(NC_PRF_SHA256, ts, 32, "iv", NULL, 0, iv, 12);
}

static void make_nonce(const uint8_t iv[12], uint64_t seq, uint8_t out[12]) {
  int i;
  memcpy(out, iv, 12);
  for (i = 0; i < 8; ++i) out[11 - i] ^= (uint8_t)((seq >> (8 * i)) & 0xff);
}

static void rec_aad(size_t payload_len, uint8_t aad[5]) {
  aad[0] = 23;
  aad[1] = 0x03;
  aad[2] = 0x03;
  aad[3] = (uint8_t)((payload_len >> 8) & 0xff);
  aad[4] = (uint8_t)(payload_len & 0xff);
}

/* ---- connection state ----------------------------------------------------- */

typedef struct {
  int fd;
  nc_buf transcript;
  uint8_t s_hs_key[16], s_hs_iv[12], c_hs_key[16], c_hs_iv[12];
  uint8_t s_app_key[16], s_app_iv[12], c_app_key[16], c_app_iv[12];
  uint64_t s_seq, c_seq;
  nc_buf hs_buf;
} Conn;

/* AEAD seal: encrypt content||inner_type with key/iv/seq, append the record to
 * fd. Returns 1 on success. */
static int send_encrypted(Conn* c, uint8_t inner_type, const uint8_t* content,
                          size_t content_len, const uint8_t key[16],
                          const uint8_t iv[12], uint64_t* seq) {
  size_t inner_len = content_len + 1;
  uint8_t* inner = (uint8_t*)malloc(inner_len);
  uint8_t* ct;
  uint8_t tag[16], nonce[12], aad[5];
  size_t rec_len;
  uint8_t* rec;
  int ok;
  if (!inner) return 0;
  memcpy(inner, content, content_len);
  inner[content_len] = inner_type;

  rec_aad(inner_len + 16, aad);
  make_nonce(iv, *seq, nonce);

  ct = (uint8_t*)malloc(inner_len);
  if (!ct) {
    free(inner);
    return 0;
  }
  nc_aes_gcm_seal(key, 16, nonce, aad, 5, inner, inner_len, ct, tag);
  (*seq)++;
  free(inner);

  rec_len = 5 + inner_len + 16;
  rec = (uint8_t*)malloc(rec_len);
  if (!rec) {
    free(ct);
    return 0;
  }
  memcpy(rec, aad, 5);
  memcpy(rec + 5, ct, inner_len);
  memcpy(rec + 5 + inner_len, tag, 16);
  free(ct);
  ok = send_all(c->fd, rec, rec_len);
  free(rec);
  return ok;
}

static int send_plaintext_handshake(Conn* c, const uint8_t* msg,
                                    size_t msg_len) {
  nc_buf rec;
  int ok;
  nc_buf_init(&rec);
  nc_buf_putc(&rec, 22);
  put16(&rec, 0x0303);
  put16(&rec, (uint16_t)msg_len);
  nc_buf_put(&rec, msg, msg_len);
  ok = !rec.err && send_all(c->fd, rec.data, rec.len);
  nc_buf_free(&rec);
  return ok;
}

/* Read one raw record. *payload is malloc'd (caller frees). Returns 1 on
 * success, 0 on EOF/error. */
static int read_record(int fd, uint8_t* type, uint8_t** payload, size_t* plen) {
  uint8_t hdr[5];
  size_t len;
  uint8_t* buf;
  if (!recv_n(fd, hdr, 5)) return 0;
  *type = hdr[0];
  len = ((size_t)hdr[3] << 8) | hdr[4];
  buf = (uint8_t*)malloc(len ? len : 1);
  if (!buf) return 0;
  if (len && !recv_n(fd, buf, len)) {
    free(buf);
    return 0;
  }
  *payload = buf;
  *plen = len;
  return 1;
}

/* Decrypt one application/handshake record payload with the given key/iv/seq.
 * On success returns 1, sets *inner (malloc'd), *inner_len, *content_type. */
static int decrypt_record(const uint8_t key[16], const uint8_t iv[12],
                          uint64_t seq, const uint8_t* payload, size_t plen,
                          uint8_t** inner_out, size_t* inner_len_out,
                          uint8_t* content_type) {
  uint8_t nonce[12], aad[5];
  size_t ct_len;
  uint8_t* pt;
  size_t n;
  if (plen < 16) return 0;
  ct_len = plen - 16;
  rec_aad(plen, aad);
  make_nonce(iv, seq, nonce);
  pt = (uint8_t*)malloc(ct_len ? ct_len : 1);
  if (!pt) return 0;
  if (!nc_aes_gcm_open(key, 16, nonce, aad, 5, payload, ct_len,
                       payload + ct_len, pt)) {
    free(pt);
    return 0;
  }
  /* strip trailing zero padding */
  n = ct_len;
  while (n > 0 && pt[n - 1] == 0) n--;
  if (n == 0) {
    free(pt);
    return 0;
  }
  *content_type = pt[n - 1];
  n--;
  *inner_out = pt;
  *inner_len_out = n;
  return 1;
}

/* Pull the next handshake message (type+body) from buffered/decrypted records.
 * Records after ServerHello are AEAD-protected with the server handshake keys.
 * *body is malloc'd (caller frees). Returns 1 on success. */
static int next_handshake(Conn* c, uint8_t* htype, uint8_t** body,
                          size_t* body_len, char* err, size_t errcap) {
  for (;;) {
    if (c->hs_buf.len >= 4) {
      size_t mlen = ((size_t)c->hs_buf.data[1] << 16) |
                    ((size_t)c->hs_buf.data[2] << 8) | c->hs_buf.data[3];
      if (c->hs_buf.len >= 4 + mlen) {
        uint8_t* b = (uint8_t*)malloc(mlen ? mlen : 1);
        if (!b) {
          set_err(err, errcap, "out of memory");
          return 0;
        }
        *htype = c->hs_buf.data[0];
        memcpy(b, c->hs_buf.data + 4, mlen);
        *body = b;
        *body_len = mlen;
        /* record this message into the transcript */
        nc_buf_put(&c->transcript, c->hs_buf.data, 4 + mlen);
        /* erase consumed bytes from hs_buf */
        memmove(c->hs_buf.data, c->hs_buf.data + 4 + mlen,
                c->hs_buf.len - (4 + mlen));
        c->hs_buf.len -= (4 + mlen);
        return 1;
      }
    }
    {
      uint8_t rtype;
      uint8_t* payload = NULL;
      size_t plen = 0;
      if (!read_record(c->fd, &rtype, &payload, &plen)) {
        set_err(err, errcap, "connection closed");
        return 0;
      }
      if (rtype == 20) { /* ChangeCipherSpec - ignore */
        free(payload);
        continue;
      }
      if (rtype == 22) { /* plaintext handshake (ServerHello) */
        nc_buf_put(&c->hs_buf, payload, plen);
        free(payload);
        continue;
      }
      if (rtype == 23) { /* encrypted record */
        uint8_t* inner = NULL;
        size_t inner_len = 0;
        uint8_t ct = 0;
        if (!decrypt_record(c->s_hs_key, c->s_hs_iv, c->s_seq, payload, plen,
                            &inner, &inner_len, &ct)) {
          free(payload);
          set_err(err, errcap, "handshake record decrypt failed");
          return 0;
        }
        free(payload);
        c->s_seq++;
        if (ct == 21) {
          free(inner);
          set_err(err, errcap, "TLS alert during handshake");
          return 0;
        }
        if (ct == 22) nc_buf_put(&c->hs_buf, inner, inner_len);
        free(inner);
        continue;
      }
      free(payload);
      set_err(err, errcap, "unexpected record type");
      return 0;
    }
  }
}

/* ---- ClientHello / ServerHello -------------------------------------------- */

static void build_client_hello(nc_buf* msg, const char* host,
                               const uint8_t client_random[32],
                               const uint8_t* session_id, size_t session_id_len,
                               const uint8_t x25519_pub[32]) {
  static const uint16_t algs[] = {0x0804, 0x0805, 0x0806, 0x0403, 0x0503};
  size_t hostlen = strlen(host);
  nc_buf ch, ext, sni, sa, ks;
  size_t i;

  nc_buf_init(&ch);
  put16(&ch, 0x0303);
  nc_buf_put(&ch, client_random, 32);
  nc_buf_putc(&ch, (uint8_t)session_id_len);
  nc_buf_put(&ch, session_id, session_id_len);
  put16(&ch, 2);
  put16(&ch, 0x1301);
  nc_buf_putc(&ch, 1);
  nc_buf_putc(&ch, 0);

  nc_buf_init(&ext);
  /* server_name (0) */
  nc_buf_init(&sni);
  put16(&sni, (uint16_t)(hostlen + 3));
  nc_buf_putc(&sni, 0);
  put16(&sni, (uint16_t)hostlen);
  nc_buf_put(&sni, (const uint8_t*)host, hostlen);
  put16(&ext, 0);
  put16(&ext, (uint16_t)sni.len);
  nc_buf_put(&ext, sni.data, sni.len);
  nc_buf_free(&sni);
  /* supported_versions (43): TLS 1.3 */
  put16(&ext, 43);
  put16(&ext, 3);
  nc_buf_putc(&ext, 2);
  put16(&ext, 0x0304);
  /* supported_groups (10): x25519 */
  put16(&ext, 10);
  put16(&ext, 4);
  put16(&ext, 2);
  put16(&ext, 0x001d);
  /* signature_algorithms (13) */
  nc_buf_init(&sa);
  put16(&sa, (uint16_t)(sizeof(algs)));
  for (i = 0; i < sizeof(algs) / sizeof(algs[0]); ++i) put16(&sa, algs[i]);
  put16(&ext, 13);
  put16(&ext, (uint16_t)sa.len);
  nc_buf_put(&ext, sa.data, sa.len);
  nc_buf_free(&sa);
  /* key_share (51): x25519 */
  nc_buf_init(&ks);
  put16(&ks, 36);
  put16(&ks, 0x001d);
  put16(&ks, 32);
  nc_buf_put(&ks, x25519_pub, 32);
  put16(&ext, 51);
  put16(&ext, (uint16_t)ks.len);
  nc_buf_put(&ext, ks.data, ks.len);
  nc_buf_free(&ks);

  put16(&ch, (uint16_t)ext.len);
  nc_buf_put(&ch, ext.data, ext.len);
  nc_buf_free(&ext);

  nc_buf_putc(msg, 1); /* client_hello */
  put24(msg, (uint32_t)ch.len);
  nc_buf_put(msg, ch.data, ch.len);
  nc_buf_free(&ch);
}

static int parse_server_hello(const uint8_t* sh, size_t shlen,
                              uint8_t server_pub[32], char* err, size_t errcap) {
  static const uint8_t kHRR[32] = {
      0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c,
      0x02, 0x1e, 0x65, 0xb8, 0x91, 0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb,
      0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};
  size_t p = 2 + 32;
  size_t ext_len, ext_end;
  uint8_t sid_len;
  if (shlen > 2 && memcmp(sh + 2, kHRR, 32) == 0) {
    set_err(err, errcap, "HelloRetryRequest not supported");
    return 0;
  }
  if (p >= shlen) {
    set_err(err, errcap, "short ServerHello");
    return 0;
  }
  sid_len = sh[p++];
  p += sid_len;
  p += 2; /* cipher_suite */
  p += 1; /* compression */
  if (p + 2 > shlen) {
    set_err(err, errcap, "no extensions");
    return 0;
  }
  ext_len = ((size_t)sh[p] << 8) | sh[p + 1];
  p += 2;
  ext_end = p + ext_len;
  while (p + 4 <= ext_end && ext_end <= shlen) {
    uint16_t etype = (uint16_t)(((size_t)sh[p] << 8) | sh[p + 1]);
    uint16_t elen = (uint16_t)(((size_t)sh[p + 2] << 8) | sh[p + 3]);
    p += 4;
    if (etype == 51 && elen >= 4) {
      uint16_t klen = (uint16_t)(((size_t)sh[p + 2] << 8) | sh[p + 3]);
      if (klen == 32 && p + 4 + 32 <= shlen) {
        memcpy(server_pub, sh + p + 4, 32);
        return 1;
      }
    }
    p += elen;
  }
  set_err(err, errcap, "no x25519 key_share in ServerHello");
  return 0;
}

/* ---- Certificate / CertificateVerify parsing ------------------------------ */

#define MAX_CERTS 16

typedef struct {
  uint8_t* der[MAX_CERTS];
  size_t der_len[MAX_CERTS];
  int count;
} CertList;

static void cert_list_free(CertList* cl) {
  int i;
  for (i = 0; i < cl->count; ++i) free(cl->der[i]);
  cl->count = 0;
}

static int parse_certificate_msg(const uint8_t* body, size_t body_len,
                                 CertList* cl) {
  size_t i = 0, list_len, list_end;
  if (i >= body_len) return 0;
  {
    size_t ctx_len = body[i++];
    i += ctx_len;
  }
  if (i + 3 > body_len) return 0;
  list_len = ((size_t)body[i] << 16) | ((size_t)body[i + 1] << 8) | body[i + 2];
  i += 3;
  list_end = i + list_len;
  if (list_end > body_len) return 0;
  while (i + 3 <= list_end) {
    size_t clen =
        ((size_t)body[i] << 16) | ((size_t)body[i + 1] << 8) | body[i + 2];
    size_t elen;
    i += 3;
    if (i + clen > list_end) return 0;
    if (cl->count < MAX_CERTS) {
      uint8_t* d = (uint8_t*)malloc(clen ? clen : 1);
      if (!d) return 0;
      memcpy(d, body + i, clen);
      cl->der[cl->count] = d;
      cl->der_len[cl->count] = clen;
      cl->count++;
    }
    i += clen;
    if (i + 2 > list_end) return 0;
    elen = ((size_t)body[i] << 8) | body[i + 1];
    i += 2 + elen;
  }
  return cl->count > 0;
}

static int parse_certificate_verify(const uint8_t* body, size_t body_len,
                                    uint16_t* scheme, const uint8_t** sig,
                                    size_t* sig_len) {
  size_t slen;
  if (body_len < 4) return 0;
  *scheme = (uint16_t)(((size_t)body[0] << 8) | body[1]);
  slen = ((size_t)body[2] << 8) | body[3];
  if (4 + slen > body_len) return 0;
  *sig = body + 4;
  *sig_len = slen;
  return 1;
}

/* ---- public entry point --------------------------------------------------- */

int nc_tls_https_post(const char* host, const char* port, const char* path,
                      const char* content_type, const char* accept,
                      const uint8_t* body, size_t body_len, int verify_cert,
                      uint8_t** resp_out, size_t* resp_len, char* err,
                      size_t errcap) {
  static nc_trust_store g_store;
  static int g_store_tried = 0;

  struct addrinfo hints, *res = NULL, *a;
  int fd = -1;
  Conn c;
  uint8_t priv[32], cpub[32], crand[32], session_id[32], spub[32], shared[32];
  uint8_t zero32[32];
  uint8_t empty_hash[32];
  uint8_t early[32], derived[32], hs[32], master[32];
  uint8_t th_hello[32], c_hs[32], s_hs[32], derived2[32];
  uint8_t th_sfin[32], c_app[32], s_app[32];
  uint8_t htype = 0;
  uint8_t* msgbody = NULL;
  size_t msgbody_len = 0;
  nc_buf clienthello;
  CertList certs;
  uint8_t th_through_cert[32];
  uint8_t cv_sig_buf[1024];
  size_t cv_sig_len = 0;
  uint16_t cv_scheme = 0;
  int have_cert = 0, have_cv = 0;
  nc_buf reqbuf, resp;
  int rc = -1;

  if (resp_out) *resp_out = NULL;
  if (resp_len) *resp_len = 0;
  if (errcap) err[0] = 0;

  memset(&c, 0, sizeof(c));
  c.fd = -1;
  nc_buf_init(&c.transcript);
  nc_buf_init(&c.hs_buf);
  certs.count = 0;

  /* ---- TCP connect ---- */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
    set_err(err, errcap, "DNS failed");
    goto done;
  }
  for (a = res; a; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    set_err(err, errcap, "connect failed");
    goto done;
  }
  c.fd = fd;

  /* ---- key material ---- */
  if (rand_bytes(priv, 32) != 0 || rand_bytes(crand, 32) != 0 ||
      rand_bytes(session_id, 32) != 0) {
    set_err(err, errcap, "RNG failed");
    goto done;
  }
  nc_x25519_base(cpub, priv);

  /* ---- ClientHello ---- */
  nc_buf_init(&clienthello);
  build_client_hello(&clienthello, host, crand, session_id, 32, cpub);
  nc_buf_put(&c.transcript, clienthello.data, clienthello.len);
  if (!send_plaintext_handshake(&c, clienthello.data, clienthello.len)) {
    nc_buf_free(&clienthello);
    set_err(err, errcap, "send ClientHello failed");
    goto done;
  }
  nc_buf_free(&clienthello);

  /* ---- ServerHello ---- */
  if (!next_handshake(&c, &htype, &msgbody, &msgbody_len, err, errcap) ||
      htype != 2) {
    if (errcap && err[0] == 0) set_err(err, errcap, "expected ServerHello");
    free(msgbody);
    goto done;
  }
  if (!parse_server_hello(msgbody, msgbody_len, spub, err, errcap)) {
    free(msgbody);
    goto done;
  }
  free(msgbody);
  msgbody = NULL;

  /* ---- key schedule (handshake secrets) ---- */
  nc_x25519(shared, priv, spub);
  memset(zero32, 0, 32);
  nc_sha256(NULL, 0, empty_hash);
  nc_hkdf_extract(NC_PRF_SHA256, NULL, 0, zero32, 32, early);
  derive_secret(early, "derived", empty_hash, 32, derived);
  nc_hkdf_extract(NC_PRF_SHA256, derived, 32, shared, 32, hs);
  nc_sha256(c.transcript.data, c.transcript.len, th_hello);
  derive_secret(hs, "c hs traffic", th_hello, 32, c_hs);
  derive_secret(hs, "s hs traffic", th_hello, 32, s_hs);
  key_iv_from(s_hs, c.s_hs_key, c.s_hs_iv);
  key_iv_from(c_hs, c.c_hs_key, c.c_hs_iv);
  c.s_seq = 0;
  derive_secret(hs, "derived", empty_hash, 32, derived2);
  nc_hkdf_extract(NC_PRF_SHA256, derived2, 32, zero32, 32, master);

  /* ---- read encrypted handshake flight until server Finished ---- */
  for (;;) {
    if (!next_handshake(&c, &htype, &msgbody, &msgbody_len, err, errcap))
      goto done;
    if (htype == 20) { /* server Finished */
      free(msgbody);
      msgbody = NULL;
      break;
    }
    if (htype == 11) { /* Certificate */
      if (!parse_certificate_msg(msgbody, msgbody_len, &certs)) {
        free(msgbody);
        set_err(err, errcap, "malformed Certificate message");
        goto done;
      }
      nc_sha256(c.transcript.data, c.transcript.len, th_through_cert);
      have_cert = 1;
    } else if (htype == 15) { /* CertificateVerify */
      const uint8_t* sig = NULL;
      if (!parse_certificate_verify(msgbody, msgbody_len, &cv_scheme, &sig,
                                    &cv_sig_len) ||
          cv_sig_len > sizeof(cv_sig_buf)) {
        free(msgbody);
        set_err(err, errcap, "malformed CertificateVerify message");
        goto done;
      }
      memcpy(cv_sig_buf, sig, cv_sig_len);
      have_cv = 1;
    }
    /* 8=EncryptedExtensions ignored */
    free(msgbody);
    msgbody = NULL;
  }

  /* ---- certificate chain validation ---- */
  if (verify_cert) {
    nc_x509_cert leaf;
    nc_buf signed_content;
    static const char kCtx[] = "TLS 1.3, server CertificateVerify";
    uint8_t pad[64];
    int i;
    const uint8_t* der_chain[MAX_CERTS];
    nc_verify_result vr;

    if (!have_cert || !have_cv) {
      set_err(err, errcap, "server did not present a certificate");
      goto done;
    }
    /* 1) CertificateVerify signature. */
    nc_buf_init(&signed_content);
    memset(pad, 0x20, 64);
    nc_buf_put(&signed_content, pad, 64);
    nc_buf_put(&signed_content, (const uint8_t*)kCtx, sizeof(kCtx) - 1);
    nc_buf_putc(&signed_content, 0x00);
    nc_buf_put(&signed_content, th_through_cert, 32);

    if (nc_x509_parse(&leaf, certs.der[0], certs.der_len[0]) != 0 ||
        !nc_x509_verify_tls13(&leaf, cv_scheme, signed_content.data,
                              signed_content.len, cv_sig_buf, cv_sig_len)) {
      nc_buf_free(&signed_content);
      set_err(err, errcap, "CertificateVerify signature is invalid");
      goto done;
    }
    nc_buf_free(&signed_content);

    /* 2) Chain to a system trust anchor + validity + hostname. */
    if (!g_store_tried) {
      g_store_tried = 1;
      if (nc_trust_store_load(&g_store, NULL) != 0) g_store.loaded = 0;
    }
    if (!g_store.loaded) {
      set_err(err, errcap, "no system trust store found");
      goto done;
    }
    for (i = 0; i < certs.count; ++i) der_chain[i] = certs.der[i];
    if (nc_x509_verify_chain(&vr, der_chain, certs.der_len, certs.count,
                             &g_store, host, (int64_t)time(NULL)) != 0 ||
        !vr.ok) {
      char msg[192 + 40];
      snprintf(msg, sizeof(msg), "certificate chain validation failed: %s",
               vr.error);
      set_err(err, errcap, msg);
      goto done;
    }
  }

  /* ---- application traffic secrets (transcript = CH..server Finished) ---- */
  nc_sha256(c.transcript.data, c.transcript.len, th_sfin);
  derive_secret(master, "c ap traffic", th_sfin, 32, c_app);
  derive_secret(master, "s ap traffic", th_sfin, 32, s_app);
  key_iv_from(c_app, c.c_app_key, c.c_app_iv);
  key_iv_from(s_app, c.s_app_key, c.s_app_iv);

  /* ---- client Finished (encrypted with client handshake keys) ---- */
  {
    uint8_t fkey[32], vd[32];
    nc_buf fin;
    int ok;
    nc_hkdf_expand_label(NC_PRF_SHA256, c_hs, 32, "finished", NULL, 0, fkey, 32);
    nc_hmac(NC_PRF_SHA256, fkey, 32, th_sfin, 32, vd);
    nc_buf_init(&fin);
    nc_buf_putc(&fin, 20);
    put24(&fin, 32);
    nc_buf_put(&fin, vd, 32);
    ok = send_encrypted(&c, 22, fin.data, fin.len, c.c_hs_key, c.c_hs_iv,
                        &c.c_seq);
    nc_buf_free(&fin);
    if (!ok) {
      set_err(err, errcap, "send Finished failed");
      goto done;
    }
  }
  c.c_seq = 0; /* switch to application keys */
  c.s_seq = 0;

  /* ---- send the HTTP request as application data ---- */
  nc_buf_init(&reqbuf);
  {
    char hdr[2048];
    int n;
    if (accept && accept[0]) {
      n = snprintf(hdr, sizeof(hdr),
                   "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: %s\r\n"
                   "Accept: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                   path, host, content_type, accept, body_len);
    } else {
      n = snprintf(hdr, sizeof(hdr),
                   "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: %s\r\n"
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                   path, host, content_type, body_len);
    }
    if (n < 0 || (size_t)n >= sizeof(hdr)) {
      nc_buf_free(&reqbuf);
      set_err(err, errcap, "request header too large");
      goto done;
    }
    nc_buf_put(&reqbuf, (const uint8_t*)hdr, (size_t)n);
    if (body && body_len) nc_buf_put(&reqbuf, body, body_len);
  }
  if (!send_encrypted(&c, 23, reqbuf.data, reqbuf.len, c.c_app_key, c.c_app_iv,
                      &c.c_seq)) {
    nc_buf_free(&reqbuf);
    set_err(err, errcap, "send request failed");
    goto done;
  }
  nc_buf_free(&reqbuf);

  /* ---- read response application data until close ---- */
  nc_buf_init(&resp);
  for (;;) {
    uint8_t rtype;
    uint8_t* payload = NULL;
    size_t plen = 0;
    uint8_t* inner = NULL;
    size_t inner_len = 0;
    uint8_t ct = 0;
    if (!read_record(c.fd, &rtype, &payload, &plen)) break;
    if (rtype != 23) {
      free(payload);
      continue;
    }
    if (!decrypt_record(c.s_app_key, c.s_app_iv, c.s_seq, payload, plen, &inner,
                        &inner_len, &ct)) {
      free(payload);
      break;
    }
    free(payload);
    c.s_seq++;
    if (ct == 21) {
      free(inner);
      break;
    }
    if (ct == 23) nc_buf_put(&resp, inner, inner_len);
    free(inner);
  }

  /* ---- parse HTTP ---- */
  {
    const char* rd = (const char*)resp.data;
    size_t rlen = resp.len;
    size_t he = (size_t)-1, i;
    size_t hlen, bstart, blen;
    int is200 = 0, chunked = 0;
    for (i = 0; i + 3 < rlen; ++i) {
      if (rd[i] == '\r' && rd[i + 1] == '\n' && rd[i + 2] == '\r' &&
          rd[i + 3] == '\n') {
        he = i;
        break;
      }
    }
    if (he == (size_t)-1) {
      nc_buf_free(&resp);
      set_err(err, errcap, "no HTTP header");
      goto done;
    }
    hlen = he;
    bstart = he + 4;
    blen = rlen - bstart;

    /* status 200? look for " 200" in headers */
    for (i = 0; i + 4 <= hlen; ++i) {
      if (rd[i] == ' ' && rd[i + 1] == '2' && rd[i + 2] == '0' &&
          rd[i + 3] == '0') {
        is200 = 1;
        break;
      }
    }
    /* chunked? case-insensitive search for "transfer-encoding: chunked" */
    {
      const char* needle = "transfer-encoding: chunked";
      size_t nl = strlen(needle);
      for (i = 0; i + nl <= hlen; ++i) {
        size_t j;
        for (j = 0; j < nl; ++j) {
          if (tolower((unsigned char)rd[i + j]) != needle[j]) break;
        }
        if (j == nl) {
          chunked = 1;
          break;
        }
      }
    }
    if (!is200) {
      nc_buf_free(&resp);
      set_err(err, errcap, "HTTP status not 200");
      goto done;
    }

    {
      uint8_t* out;
      size_t outlen;
      if (chunked) {
        const char* bp = rd + bstart;
        size_t i2 = 0;
        nc_buf dec;
        nc_buf_init(&dec);
        while (i2 < blen) {
          size_t eol = (size_t)-1, k, sz = 0;
          for (k = i2; k + 1 < blen; ++k) {
            if (bp[k] == '\r' && bp[k + 1] == '\n') {
              eol = k;
              break;
            }
          }
          if (eol == (size_t)-1) break;
          for (k = i2; k < eol; ++k) {
            char ch2 = bp[k];
            int v;
            if (ch2 >= '0' && ch2 <= '9')
              v = ch2 - '0';
            else if ((ch2 | 32) >= 'a' && (ch2 | 32) <= 'f')
              v = (ch2 | 32) - 'a' + 10;
            else
              break;
            sz = sz * 16 + (size_t)v;
          }
          i2 = eol + 2;
          if (sz == 0 || i2 + sz > blen) break;
          nc_buf_put(&dec, (const uint8_t*)bp + i2, sz);
          i2 += sz + 2;
        }
        outlen = dec.len;
        out = (uint8_t*)malloc(outlen ? outlen : 1);
        if (out && outlen) memcpy(out, dec.data, outlen);
        nc_buf_free(&dec);
      } else {
        outlen = blen;
        out = (uint8_t*)malloc(outlen ? outlen : 1);
        if (out && outlen) memcpy(out, rd + bstart, outlen);
      }
      if (!out) {
        nc_buf_free(&resp);
        set_err(err, errcap, "out of memory");
        goto done;
      }
      if (outlen == 0) {
        free(out);
        nc_buf_free(&resp);
        set_err(err, errcap, "empty response body");
        goto done;
      }
      if (resp_out) *resp_out = out;
      else free(out);
      if (resp_len) *resp_len = outlen;
      rc = 0;
    }
  }
  nc_buf_free(&resp);

done:
  cert_list_free(&certs);
  nc_buf_free(&c.transcript);
  nc_buf_free(&c.hs_buf);
  if (c.fd >= 0) close(c.fd);
  return rc;
}
