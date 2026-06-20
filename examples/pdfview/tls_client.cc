// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Minimal TLS 1.3 client (TLS_AES_128_GCM_SHA256, X25519). The server
// certificate chain IS validated by default (CertificateVerify signature +
// chain to a system trust anchor + validity + hostname); see tls_client.hh.

#include "tls_client.hh"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <ctime>
#include <random>

#include "crypto.hh"
#include "tls-crypto.hh"
#include "x509.hh"

namespace pdfview {

namespace {

using nanopdf::tlscrypto::Bytes;
using nanopdf::tlscrypto::hkdf_expand_label;
using nanopdf::tlscrypto::hkdf_extract;

Bytes sha256(const Bytes& d) {
  uint8_t h[32];
  nanopdf::crypto::SHA256::hash(d.data(), d.size(), h);
  return Bytes(h, h + 32);
}
Bytes derive_secret(const Bytes& secret, const std::string& label,
                    const Bytes& thash) {
  return hkdf_expand_label(secret, label, thash, 32);
}

void put16(Bytes& b, uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xff); }
void put24(Bytes& b, uint32_t v) {
  b.push_back((v >> 16) & 0xff); b.push_back((v >> 8) & 0xff); b.push_back(v & 0xff);
}

bool send_all(int fd, const uint8_t* p, size_t n) {
  size_t s = 0;
  while (s < n) {
    ssize_t k = send(fd, p + s, n - s, 0);
    if (k <= 0) return false;
    s += (size_t)k;
  }
  return true;
}
bool recv_n(int fd, uint8_t* p, size_t n) {
  size_t r = 0;
  while (r < n) {
    ssize_t k = recv(fd, p + r, n - r, 0);
    if (k <= 0) return false;
    r += (size_t)k;
  }
  return true;
}

struct Conn {
  int fd = -1;
  Bytes transcript;
  // Traffic keys (16-byte key + 12-byte iv) and per-key sequence numbers.
  Bytes s_hs_key, s_hs_iv, c_hs_key, c_hs_iv;
  Bytes s_app_key, s_app_iv, c_app_key, c_app_iv;
  uint64_t s_seq = 0, c_seq = 0;
  enum Phase { HS, APP } recv_phase = HS;

  // Buffered decrypted handshake bytes awaiting message framing.
  Bytes hs_buf;

  void key_iv_from(const Bytes& ts, Bytes* key, Bytes* iv) {
    *key = hkdf_expand_label(ts, "key", {}, 16);
    *iv = hkdf_expand_label(ts, "iv", {}, 12);
  }

  Bytes nonce(const Bytes& iv, uint64_t seq) {
    Bytes n = iv;
    for (int i = 0; i < 8; ++i) n[11 - i] ^= (seq >> (8 * i)) & 0xff;
    return n;
  }

  // Send one record. @type is the (inner) content type; before keys are set
  // (handshake==true && no key) it is sent as plaintext.
  bool send_plaintext_handshake(const Bytes& msg) {
    Bytes rec;
    rec.push_back(22);
    put16(rec, 0x0303);
    put16(rec, (uint16_t)msg.size());
    rec.insert(rec.end(), msg.begin(), msg.end());
    return send_all(fd, rec.data(), rec.size());
  }

  bool send_encrypted(uint8_t inner_type, const Bytes& content,
                      const Bytes& key, const Bytes& iv, uint64_t* seq) {
    Bytes inner = content;
    inner.push_back(inner_type);
    Bytes aad;
    aad.push_back(23);
    put16(aad, 0x0303);
    put16(aad, (uint16_t)(inner.size() + 16));
    Bytes ct = nanopdf::tlscrypto::aes128_gcm_seal(key.data(),
                                                   nonce(iv, *seq).data(), aad,
                                                   inner);
    (*seq)++;
    Bytes rec = aad;
    rec.insert(rec.end(), ct.begin(), ct.end());
    return send_all(fd, rec.data(), rec.size());
  }

  // Read one raw record. Returns false on EOF. Fills outer type + payload.
  bool read_record(uint8_t* type, Bytes* payload) {
    uint8_t hdr[5];
    if (!recv_n(fd, hdr, 5)) return false;
    *type = hdr[0];
    size_t len = (hdr[3] << 8) | hdr[4];
    payload->assign(len, 0);
    if (len && !recv_n(fd, payload->data(), len)) return false;
    return true;
  }

  // Pull the next handshake message (type+body), decrypting records as needed.
  // @encrypted selects whether records are AEAD-protected (post-ServerHello).
  bool next_handshake(uint8_t* htype, Bytes* body, std::string* err) {
    for (;;) {
      // Frame a complete message from the buffer if possible.
      if (hs_buf.size() >= 4) {
        size_t mlen = (hs_buf[1] << 16) | (hs_buf[2] << 8) | hs_buf[3];
        if (hs_buf.size() >= 4 + mlen) {
          *htype = hs_buf[0];
          body->assign(hs_buf.begin() + 4, hs_buf.begin() + 4 + mlen);
          // Record this message into the transcript.
          transcript.insert(transcript.end(), hs_buf.begin(),
                            hs_buf.begin() + 4 + mlen);
          hs_buf.erase(hs_buf.begin(), hs_buf.begin() + 4 + mlen);
          return true;
        }
      }
      uint8_t rtype;
      Bytes payload;
      if (!read_record(&rtype, &payload)) { *err = "connection closed"; return false; }
      if (rtype == 20) continue;  // ChangeCipherSpec (compat) — ignore
      if (rtype == 22) {          // plaintext handshake (ServerHello)
        hs_buf.insert(hs_buf.end(), payload.begin(), payload.end());
        continue;
      }
      if (rtype == 23) {  // encrypted record
        Bytes inner;
        if (!nanopdf::tlscrypto::aes128_gcm_open(
                s_hs_key.data(), nonce(s_hs_iv, s_seq).data(),
                rec_aad(payload.size()), payload, &inner)) {
          *err = "handshake record decrypt failed";
          return false;
        }
        s_seq++;
        while (!inner.empty() && inner.back() == 0) inner.pop_back();
        if (inner.empty()) { *err = "empty inner record"; return false; }
        uint8_t ct = inner.back();
        inner.pop_back();
        if (ct == 21) { *err = "TLS alert during handshake"; return false; }
        if (ct == 22)
          hs_buf.insert(hs_buf.end(), inner.begin(), inner.end());
        continue;
      }
      *err = "unexpected record type";
      return false;
    }
  }

  static Bytes rec_aad(size_t payload_len) {
    Bytes aad;
    aad.push_back(23);
    aad.push_back(0x03);
    aad.push_back(0x03);
    aad.push_back((payload_len >> 8) & 0xff);
    aad.push_back(payload_len & 0xff);
    return aad;
  }
};

Bytes build_client_hello(const std::string& host, const uint8_t client_random[32],
                         const Bytes& session_id, const uint8_t x25519_pub[32]) {
  Bytes ch;
  put16(ch, 0x0303);                          // legacy_version
  ch.insert(ch.end(), client_random, client_random + 32);
  ch.push_back((uint8_t)session_id.size());   // legacy_session_id
  ch.insert(ch.end(), session_id.begin(), session_id.end());
  put16(ch, 2);                               // cipher_suites length
  put16(ch, 0x1301);                          // TLS_AES_128_GCM_SHA256
  ch.push_back(1);                            // compression methods len
  ch.push_back(0);                            // null compression

  Bytes ext;
  // server_name (0)
  {
    Bytes sni;
    put16(sni, (uint16_t)(host.size() + 3));  // server_name_list len
    sni.push_back(0);                         // host_name type
    put16(sni, (uint16_t)host.size());
    sni.insert(sni.end(), host.begin(), host.end());
    put16(ext, 0); put16(ext, (uint16_t)sni.size());
    ext.insert(ext.end(), sni.begin(), sni.end());
  }
  // supported_versions (43): TLS 1.3
  { put16(ext, 43); put16(ext, 3); ext.push_back(2); put16(ext, 0x0304); }
  // supported_groups (10): x25519
  { put16(ext, 10); put16(ext, 4); put16(ext, 2); put16(ext, 0x001d); }
  // signature_algorithms (13)
  {
    // Only schemes we can verify: RSA-PSS (SHA-256/384/512) and ECDSA on
    // P-256/P-384. (No P-521 / EdDSA / legacy PKCS#1-for-CertVerify.)
    static const uint16_t algs[] = {0x0804, 0x0805, 0x0806, 0x0403, 0x0503};
    Bytes sa;
    put16(sa, (uint16_t)(sizeof(algs)));
    for (uint16_t a : algs) put16(sa, a);
    put16(ext, 13); put16(ext, (uint16_t)sa.size());
    ext.insert(ext.end(), sa.begin(), sa.end());
  }
  // key_share (51): x25519
  {
    Bytes ks;
    put16(ks, 36);            // client_shares length
    put16(ks, 0x001d);        // group
    put16(ks, 32);            // key_exchange length
    ks.insert(ks.end(), x25519_pub, x25519_pub + 32);
    put16(ext, 51); put16(ext, (uint16_t)ks.size());
    ext.insert(ext.end(), ks.begin(), ks.end());
  }
  put16(ch, (uint16_t)ext.size());
  ch.insert(ch.end(), ext.begin(), ext.end());

  Bytes msg;
  msg.push_back(1);  // handshake type: client_hello
  put24(msg, (uint32_t)ch.size());
  msg.insert(msg.end(), ch.begin(), ch.end());
  return msg;
}

// Parse ServerHello, extracting the server's x25519 key share. Returns false on
// HelloRetryRequest or missing key share.
bool parse_server_hello(const Bytes& sh, uint8_t server_pub[32],
                        std::string* err) {
  // sh is the ServerHello body (after the 4-byte handshake header).
  static const uint8_t kHRR[32] = {
      0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c,
      0x02, 0x1e, 0x65, 0xb8, 0x91, 0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb,
      0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};
  size_t p = 2 + 32;  // legacy_version + random
  if (sh.size() > 2 && std::memcmp(sh.data() + 2, kHRR, 32) == 0) {
    *err = "HelloRetryRequest not supported";
    return false;
  }
  if (p >= sh.size()) { *err = "short ServerHello"; return false; }
  uint8_t sid_len = sh[p++];
  p += sid_len;                 // legacy_session_id_echo
  p += 2;                       // cipher_suite
  p += 1;                       // legacy_compression_method
  if (p + 2 > sh.size()) { *err = "no extensions"; return false; }
  size_t ext_len = (sh[p] << 8) | sh[p + 1];
  p += 2;
  size_t ext_end = p + ext_len;
  while (p + 4 <= ext_end && ext_end <= sh.size()) {
    uint16_t etype = (sh[p] << 8) | sh[p + 1];
    uint16_t elen = (sh[p + 2] << 8) | sh[p + 3];
    p += 4;
    if (etype == 51 && elen >= 4) {  // key_share
      // group(2) + key_exchange length(2) + key
      uint16_t klen = (sh[p + 2] << 8) | sh[p + 3];
      if (klen == 32 && p + 4 + 32 <= sh.size()) {
        std::memcpy(server_pub, sh.data() + p + 4, 32);
        return true;
      }
    }
    p += elen;
  }
  *err = "no x25519 key_share in ServerHello";
  return false;
}

// Parse a TLS 1.3 Certificate message body into the DER certificate list.
// body = certificate_request_context<1> || certificate_list<3>, each entry =
// cert_data<3> || extensions<2>.
bool parse_certificate_msg(const Bytes& body,
                           std::vector<std::vector<uint8_t>>* certs) {
  size_t i = 0;
  if (i >= body.size()) return false;
  size_t ctx_len = body[i++];
  i += ctx_len;
  if (i + 3 > body.size()) return false;
  size_t list_len = (body[i] << 16) | (body[i + 1] << 8) | body[i + 2];
  i += 3;
  size_t list_end = i + list_len;
  if (list_end > body.size()) return false;
  while (i + 3 <= list_end) {
    size_t clen = (body[i] << 16) | (body[i + 1] << 8) | body[i + 2];
    i += 3;
    if (i + clen > list_end) return false;
    certs->emplace_back(body.begin() + i, body.begin() + i + clen);
    i += clen;
    if (i + 2 > list_end) return false;
    size_t elen = (body[i] << 8) | body[i + 1];
    i += 2 + elen;
  }
  return !certs->empty();
}

// Parse a CertificateVerify body: SignatureScheme(2) || signature<2>.
bool parse_certificate_verify(const Bytes& body, uint16_t* scheme,
                             Bytes* sig) {
  if (body.size() < 4) return false;
  *scheme = (body[0] << 8) | body[1];
  size_t slen = (body[2] << 8) | body[3];
  if (4 + slen > body.size()) return false;
  sig->assign(body.begin() + 4, body.begin() + 4 + slen);
  return true;
}

}  // namespace

std::vector<uint8_t> tls_https_post(const std::string& host,
                                    const std::string& port,
                                    const std::string& path,
                                    const std::string& content_type,
                                    const std::string& accept,
                                    const std::vector<uint8_t>& body,
                                    std::string* err, bool verify_cert) {
  std::vector<uint8_t> result;

  // ---- TCP connect ----
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    *err = "DNS failed for " + host;
    return result;
  }
  Conn c;
  for (addrinfo* a = res; a; a = a->ai_next) {
    c.fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (c.fd < 0) continue;
    if (connect(c.fd, a->ai_addr, a->ai_addrlen) == 0) break;
    close(c.fd);
    c.fd = -1;
  }
  freeaddrinfo(res);
  if (c.fd < 0) { *err = "connect failed"; return result; }

  // ---- key material ----
  std::random_device rd;
  uint8_t priv[32], cpub[32], crand[32];
  for (int i = 0; i < 32; ++i) { priv[i] = rd() & 0xff; crand[i] = rd() & 0xff; }
  nanopdf::tlscrypto::x25519_base(cpub, priv);
  Bytes session_id(32);
  for (auto& b : session_id) b = rd() & 0xff;

  // ---- ClientHello ----
  Bytes ch = build_client_hello(host, crand, session_id, cpub);
  c.transcript.insert(c.transcript.end(), ch.begin(), ch.end());
  if (!c.send_plaintext_handshake(ch)) { *err = "send ClientHello failed"; close(c.fd); return result; }

  // ---- ServerHello ----
  uint8_t htype;
  Bytes sh;
  if (!c.next_handshake(&htype, &sh, err) || htype != 2) {
    if (err->empty()) *err = "expected ServerHello";
    close(c.fd);
    return result;
  }
  uint8_t spub[32];
  if (!parse_server_hello(sh, spub, err)) { close(c.fd); return result; }

  // ---- key schedule (handshake secrets) ----
  uint8_t shared[32];
  nanopdf::tlscrypto::x25519(shared, priv, spub);
  Bytes zero32(32, 0);
  Bytes early = hkdf_extract({}, zero32);
  Bytes derived = derive_secret(early, "derived", sha256({}));
  Bytes hs = hkdf_extract(derived, Bytes(shared, shared + 32));
  Bytes th_hello = sha256(c.transcript);  // CH..SH
  Bytes c_hs = derive_secret(hs, "c hs traffic", th_hello);
  Bytes s_hs = derive_secret(hs, "s hs traffic", th_hello);
  c.key_iv_from(s_hs, &c.s_hs_key, &c.s_hs_iv);
  c.key_iv_from(c_hs, &c.c_hs_key, &c.c_hs_iv);
  c.s_seq = 0;
  Bytes derived2 = derive_secret(hs, "derived", sha256({}));
  Bytes master = hkdf_extract(derived2, zero32);

  // ---- read encrypted handshake flight until server Finished ----
  // Capture Certificate + CertificateVerify for chain validation. The
  // CertificateVerify signs the transcript hash through the Certificate message.
  std::vector<std::vector<uint8_t>> cert_ders;
  Bytes th_through_cert, cv_sig;
  uint16_t cv_scheme = 0;
  bool have_cert = false, have_cv = false;
  for (;;) {
    Bytes body_msg;
    if (!c.next_handshake(&htype, &body_msg, err)) { close(c.fd); return result; }
    if (htype == 20) break;  // server Finished (transcript already updated)
    if (htype == 11) {       // Certificate
      if (!parse_certificate_msg(body_msg, &cert_ders)) {
        *err = "malformed Certificate message";
        close(c.fd);
        return result;
      }
      th_through_cert = sha256(c.transcript);  // CH..Certificate
      have_cert = true;
    } else if (htype == 15) {  // CertificateVerify
      if (!parse_certificate_verify(body_msg, &cv_scheme, &cv_sig)) {
        *err = "malformed CertificateVerify message";
        close(c.fd);
        return result;
      }
      have_cv = true;
    }
    // 8=EncryptedExtensions ignored.
  }

  // ---- certificate chain validation ----
  if (verify_cert) {
    if (!have_cert || !have_cv) {
      *err = "server did not present a certificate";
      close(c.fd);
      return result;
    }
    // 1) CertificateVerify: the server signs
    //    (0x20 * 64) || "TLS 1.3, server CertificateVerify" || 0x00 || THash.
    Bytes signed_content(64, 0x20);
    static const char kCtx[] = "TLS 1.3, server CertificateVerify";
    signed_content.insert(signed_content.end(), kCtx, kCtx + sizeof(kCtx) - 1);
    signed_content.push_back(0x00);
    signed_content.insert(signed_content.end(), th_through_cert.begin(),
                          th_through_cert.end());
    nanopdf::x509::Certificate leaf =
        nanopdf::x509::parse(cert_ders[0].data(), cert_ders[0].size());
    if (!leaf.parsed ||
        !nanopdf::x509::verify_tls13_signature(
            leaf, cv_scheme, signed_content.data(), signed_content.size(),
            cv_sig.data(), cv_sig.size())) {
      *err = "CertificateVerify signature is invalid";
      close(c.fd);
      return result;
    }
    // 2) Chain to a system trust anchor + validity + hostname.
    static nanopdf::x509::TrustStore g_store =
        nanopdf::x509::load_trust_store("");
    if (!g_store.loaded) {
      *err = "no system trust store found (set verify_cert=false to bypass)";
      close(c.fd);
      return result;
    }
    int64_t now = (int64_t)std::time(nullptr);
    nanopdf::x509::VerifyResult vr =
        nanopdf::x509::verify_chain(cert_ders, g_store, host, now);
    if (!vr.ok) {
      *err = "certificate chain validation failed: " + vr.error;
      close(c.fd);
      return result;
    }
  }

  // ---- application traffic secrets (transcript = CH..server Finished) ----
  Bytes th_sfin = sha256(c.transcript);
  Bytes c_app = derive_secret(master, "c ap traffic", th_sfin);
  Bytes s_app = derive_secret(master, "s ap traffic", th_sfin);
  c.key_iv_from(c_app, &c.c_app_key, &c.c_app_iv);
  c.key_iv_from(s_app, &c.s_app_key, &c.s_app_iv);

  // ---- client Finished (encrypted with client handshake keys) ----
  {
    Bytes fkey = hkdf_expand_label(c_hs, "finished", {}, 32);
    Bytes vd = nanopdf::crypto::hmac(nanopdf::crypto::Prf::Sha256, fkey.data(),
                                     fkey.size(), th_sfin.data(),
                                     th_sfin.size());
    Bytes fin;
    fin.push_back(20);
    put24(fin, (uint32_t)vd.size());
    fin.insert(fin.end(), vd.begin(), vd.end());
    if (!c.send_encrypted(22, fin, c.c_hs_key, c.c_hs_iv, &c.c_seq)) {
      *err = "send Finished failed";
      close(c.fd);
      return result;
    }
  }
  c.c_seq = 0;  // client switches to application keys (seq restarts)
  c.s_seq = 0;  // server switches to application keys after its Finished

  // ---- send the HTTP request as application data ----
  std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + host +
                    "\r\nContent-Type: " + content_type + "\r\n";
  if (!accept.empty()) req += "Accept: " + accept + "\r\n";
  req += "Content-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n";
  Bytes appdata(req.begin(), req.end());
  appdata.insert(appdata.end(), body.begin(), body.end());
  if (!c.send_encrypted(23, appdata, c.c_app_key, c.c_app_iv, &c.c_seq)) {
    *err = "send request failed";
    close(c.fd);
    return result;
  }

  // ---- read response application data until close ----
  std::string resp;
  for (;;) {
    uint8_t rtype;
    Bytes payload;
    if (!c.read_record(&rtype, &payload)) break;  // EOF / close
    if (rtype != 23) continue;
    Bytes inner;
    if (!nanopdf::tlscrypto::aes128_gcm_open(
            c.s_app_key.data(), c.nonce(c.s_app_iv, c.s_seq).data(),
            Conn::rec_aad(payload.size()), payload, &inner))
      break;  // could be a post-handshake msg under hs key; we stop on failure
    c.s_seq++;
    while (!inner.empty() && inner.back() == 0) inner.pop_back();
    if (inner.empty()) continue;
    uint8_t ct = inner.back();
    inner.pop_back();
    if (ct == 21) break;                 // close_notify / alert
    if (ct == 23) resp.append(inner.begin(), inner.end());
    // ct==22 (NewSessionTicket) -> ignore
  }
  close(c.fd);

  // ---- parse HTTP ----
  size_t he = resp.find("\r\n\r\n");
  if (he == std::string::npos) { *err = "no HTTP header"; return result; }
  std::string headers = resp.substr(0, he);
  std::string bodystr = resp.substr(he + 4);
  if (headers.find(" 200") == std::string::npos) {
    *err = "HTTP status not 200";
    return result;
  }
  std::string low = headers;
  for (char& ch2 : low) ch2 = (char)tolower((unsigned char)ch2);
  if (low.find("transfer-encoding: chunked") != std::string::npos) {
    std::string dec;
    size_t i = 0;
    while (i < bodystr.size()) {
      size_t eol = bodystr.find("\r\n", i);
      if (eol == std::string::npos) break;
      size_t sz = strtoul(bodystr.substr(i, eol - i).c_str(), nullptr, 16);
      i = eol + 2;
      if (sz == 0 || i + sz > bodystr.size()) break;
      dec.append(bodystr, i, sz);
      i += sz + 2;
    }
    bodystr.swap(dec);
  }
  result.assign(bodystr.begin(), bodystr.end());
  if (result.empty()) *err = "empty response body";
  return result;
}

}  // namespace pdfview
