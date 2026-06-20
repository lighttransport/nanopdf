// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "pdf_sign.hh"

// OpenSSL is optional: it is only needed for https TSAs (TLS) and the legacy
// signing path. Without it, the pure-C++ stack still handles PEM/PKCS#12
// signing, http timestamps, and verification.
#ifndef PDFVIEW_HAVE_OPENSSL
#define PDFVIEW_HAVE_OPENSSL 0
#endif

#if PDFVIEW_HAVE_OPENSSL
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/http.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/pkcs7.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/ts.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>

#include "nanopdf.hh"
#include "pdf-writer.hh"
#include "asn1-der.hh"
#include "cms.hh"
#include "crypto.hh"
#include "crypto-pk.hh"
#include "pkcs12.hh"
#include "rfc3161.hh"
#include "tls_client.hh"

namespace pdfview {

namespace {

#if PDFVIEW_HAVE_OPENSSL
std::string ossl_err() {
  unsigned long e = ERR_get_error();
  if (!e) return "unknown OpenSSL error";
  char buf[256];
  ERR_error_string_n(e, buf, sizeof(buf));
  return buf;
}

// --- credential loading -----------------------------------------------------

struct Credentials {
  X509* cert = nullptr;
  EVP_PKEY* key = nullptr;
  STACK_OF(X509)* chain = nullptr;  // additional certs (issuer chain)
  ~Credentials() {
    if (cert) X509_free(cert);
    if (key) EVP_PKEY_free(key);
    if (chain) sk_X509_pop_free(chain, X509_free);
  }
};

bool load_credentials(const SignOptions& opt, Credentials* out,
                      std::string* err) {
  if (!opt.p12_path.empty()) {
    BIO* bio = BIO_new_file(opt.p12_path.c_str(), "rb");
    if (!bio) { *err = "cannot open PKCS#12: " + opt.p12_path; return false; }
    PKCS12* p12 = d2i_PKCS12_bio(bio, nullptr);
    BIO_free(bio);
    if (!p12) { *err = "invalid PKCS#12 file: " + ossl_err(); return false; }
    int ok = PKCS12_parse(p12, opt.p12_password.c_str(), &out->key, &out->cert,
                          &out->chain);
    PKCS12_free(p12);
    if (!ok || !out->cert || !out->key) {
      *err = "PKCS#12 parse failed (wrong password?): " + ossl_err();
      return false;
    }
    return true;
  }
  if (opt.cert_pem_path.empty() || opt.key_pem_path.empty()) {
    *err = "no credentials: provide --p12 or both --cert and --key";
    return false;
  }
  BIO* cbio = BIO_new_file(opt.cert_pem_path.c_str(), "rb");
  if (!cbio) { *err = "cannot open cert: " + opt.cert_pem_path; return false; }
  out->cert = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr);
  // Any remaining certs in the file form the chain.
  out->chain = sk_X509_new_null();
  X509* extra;
  while ((extra = PEM_read_bio_X509(cbio, nullptr, nullptr, nullptr)) != nullptr)
    sk_X509_push(out->chain, extra);
  BIO_free(cbio);
  if (!out->cert) { *err = "invalid PEM certificate: " + ossl_err(); return false; }

  BIO* kbio = BIO_new_file(opt.key_pem_path.c_str(), "rb");
  if (!kbio) { *err = "cannot open key: " + opt.key_pem_path; return false; }
  void* pw = opt.key_password.empty()
                 ? nullptr
                 : const_cast<char*>(opt.key_password.c_str());
  out->key = PEM_read_bio_PrivateKey(kbio, nullptr, nullptr, pw);
  BIO_free(kbio);
  if (!out->key) { *err = "invalid PEM private key: " + ossl_err(); return false; }
  return true;
}

// --- RFC 3161 timestamp -----------------------------------------------------

const EVP_MD* md_by_name(const std::string& n) {
  if (n == "sha384") return EVP_sha384();
  if (n == "sha512") return EVP_sha512();
  return EVP_sha256();
}

// TLS BIO callback for OSSL_HTTP (https TSAs like FreeTSA). Wraps @bio in TLS
// on connect; tears it down on disconnect.
BIO* http_tls_cb(BIO* bio, void* arg, int connect, int detail) {
  SSL_CTX* ctx = static_cast<SSL_CTX*>(arg);
  if (connect && detail) {  // set up TLS
    BIO* sbio = BIO_new_ssl(ctx, /*client=*/1);
    if (!sbio) return nullptr;
    bio = BIO_push(sbio, bio);
  } else if (!connect) {  // tear down
    BIO_ssl_shutdown(bio);
  }
  return bio;
}

// Request an RFC 3161 timestamp token over @url for the message imprint
// hash(@data). Returns the TimeStampToken as DER bytes, or empty on failure.
std::vector<uint8_t> fetch_timestamp_token(const std::string& url,
                                           const std::string& user,
                                           const std::string& pass,
                                           const unsigned char* data, size_t len,
                                           const std::string& digest,
                                           std::string* err) {
  std::vector<uint8_t> result;
  const EVP_MD* md = md_by_name(digest);
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hlen = 0;
  EVP_Digest(data, len, hash, &hlen, md, nullptr);

  TS_REQ* req = TS_REQ_new();
  TS_MSG_IMPRINT* imp = TS_MSG_IMPRINT_new();
  X509_ALGOR* alg = X509_ALGOR_new();
  X509_ALGOR_set0(alg, OBJ_nid2obj(EVP_MD_type(md)), V_ASN1_NULL, nullptr);
  TS_REQ_set_version(req, 1);
  TS_MSG_IMPRINT_set_algo(imp, alg);
  TS_MSG_IMPRINT_set_msg(imp, hash, hlen);
  TS_REQ_set_msg_imprint(req, imp);
  TS_REQ_set_cert_req(req, 1);
  // Random 64-bit nonce.
  {
    ASN1_INTEGER* nonce = ASN1_INTEGER_new();
    unsigned char nb[8];
    RAND_bytes(nb, sizeof(nb));
    BIGNUM* bn = BN_bin2bn(nb, sizeof(nb), nullptr);
    BN_to_ASN1_INTEGER(bn, nonce);
    TS_REQ_set_nonce(req, nonce);
    ASN1_INTEGER_free(nonce);
    BN_free(bn);
  }
  X509_ALGOR_free(alg);
  TS_MSG_IMPRINT_free(imp);

  unsigned char* reqder = nullptr;
  int reqlen = i2d_TS_REQ(req, &reqder);
  TS_REQ_free(req);
  if (reqlen <= 0) { *err = "failed to encode TS request"; return result; }

  // Parse URL and POST via OSSL_HTTP (handles http + https).
  char *host = nullptr, *port = nullptr, *path = nullptr;
  int use_ssl = 0;
  if (!OSSL_HTTP_parse_url(url.c_str(), &use_ssl, nullptr, &host, &port, nullptr,
                           &path, nullptr, nullptr)) {
    OPENSSL_free(reqder);
    *err = "bad TSA url: " + url;
    return result;
  }
  SSL_CTX* tls = nullptr;
  if (use_ssl) {
    tls = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(tls, SSL_VERIFY_NONE, nullptr);  // token is self-verifying
  }
  BIO* reqbio = BIO_new_mem_buf(reqder, reqlen);
  OSSL_HTTP_bio_cb_t tls_cb = use_ssl ? http_tls_cb : nullptr;
  OSSL_HTTP_REQ_CTX* rctx =
      OSSL_HTTP_open(host, port, nullptr, nullptr, use_ssl, nullptr, nullptr,
                     tls_cb, tls, 0, 30000);
  TS_RESP* resp = nullptr;
  if (rctx) {
    STACK_OF(CONF_VALUE)* hdrs = nullptr;
    if (!user.empty()) {
      std::string cred = user + ":" + pass;
      BIO* b64 = BIO_new(BIO_f_base64());
      BIO* mem = BIO_new(BIO_s_mem());
      BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
      b64 = BIO_push(b64, mem);
      BIO_write(b64, cred.data(), (int)cred.size());
      BIO_flush(b64);
      char* enc = nullptr;
      long n = BIO_get_mem_data(mem, &enc);
      std::string auth = "Basic " + std::string(enc, (size_t)n);
      X509V3_add_value("Authorization", auth.c_str(), &hdrs);
      BIO_free_all(b64);
    }
    if (OSSL_HTTP_set1_request(rctx, path, hdrs, "application/timestamp-query",
                               reqbio, "application/timestamp-reply",
                               /*expect_asn1=*/1, /*max_resp_len=*/0,
                               /*timeout=*/30000, /*keep_alive=*/0)) {
      BIO* rbio = OSSL_HTTP_exchange(rctx, nullptr);
      if (rbio) resp = d2i_TS_RESP_bio(rbio, nullptr);
    }
    if (hdrs) sk_CONF_VALUE_pop_free(hdrs, X509V3_conf_free);
    OSSL_HTTP_close(rctx, 1);
  }
  BIO_free(reqbio);
  OPENSSL_free(reqder);
  OPENSSL_free(host);
  OPENSSL_free(port);
  OPENSSL_free(path);
  if (tls) SSL_CTX_free(tls);

  if (!resp) { *err = "no/invalid TSA response from " + url; return result; }
  PKCS7* token = TS_RESP_get_token(resp);
  if (token) {
    unsigned char* td = nullptr;
    int tl = i2d_PKCS7(token, &td);
    if (tl > 0) {
      result.assign(td, td + tl);
      OPENSSL_free(td);
    }
  }
  TS_RESP_free(resp);
  if (result.empty()) *err = "TSA returned no token (rejected request?)";
  return result;
}

// Attach a TimeStampToken as the id-aa-timeStampToken unsigned attribute of the
// signer info. Returns true on success.
bool embed_timestamp(PKCS7* p7, const std::vector<uint8_t>& token_der,
                     std::string* err) {
  STACK_OF(PKCS7_SIGNER_INFO)* sis = PKCS7_get_signer_info(p7);
  if (!sis || sk_PKCS7_SIGNER_INFO_num(sis) < 1) {
    *err = "no signer info to timestamp";
    return false;
  }
  PKCS7_SIGNER_INFO* si = sk_PKCS7_SIGNER_INFO_value(sis, 0);
  ASN1_OBJECT* oid = OBJ_txt2obj("1.2.840.113549.1.9.16.2.14", 1);
  X509_ATTRIBUTE* attr = X509_ATTRIBUTE_new();
  X509_ATTRIBUTE_set1_object(attr, oid);
  X509_ATTRIBUTE_set1_data(attr, V_ASN1_SEQUENCE, token_der.data(),
                           (int)token_der.size());
  ASN1_OBJECT_free(oid);
  if (!si->unauth_attr) si->unauth_attr = sk_X509_ATTRIBUTE_new_null();
  sk_X509_ATTRIBUTE_push(si->unauth_attr, attr);
  return true;
}

// Build the detached CMS/PKCS#7 signature over @data, optionally embedding an
// RFC 3161 timestamp. Returns DER bytes (empty on failure).
std::vector<uint8_t> build_cms(const std::vector<uint8_t>& data,
                               const Credentials& cred, const SignOptions& opt,
                               SignResult* sr) {
  std::vector<uint8_t> der;
  int flags = PKCS7_DETACHED | PKCS7_BINARY | PKCS7_NOSMIMECAP;
  BIO* dbio = BIO_new_mem_buf(data.data(), (int)data.size());
  PKCS7* p7 = PKCS7_sign(cred.cert, cred.key, cred.chain, dbio, flags);
  BIO_free(dbio);
  if (!p7) { sr->error = "PKCS7_sign failed: " + ossl_err(); return der; }

  if (!opt.tsa_url.empty()) {
    STACK_OF(PKCS7_SIGNER_INFO)* sis = PKCS7_get_signer_info(p7);
    PKCS7_SIGNER_INFO* si = sk_PKCS7_SIGNER_INFO_value(sis, 0);
    std::string terr;
    std::vector<uint8_t> token = fetch_timestamp_token(
        opt.tsa_url, opt.tsa_username, opt.tsa_password, si->enc_digest->data,
        (size_t)si->enc_digest->length, opt.tsa_digest, &terr);
    if (!token.empty() && embed_timestamp(p7, token, &terr)) {
      sr->timestamped = true;
      sr->timestamp_authority = opt.tsa_url;
    } else {
      // Non-fatal: keep the (untimestamped) signature, report the reason.
      std::fprintf(stderr, "pdf_sign: timestamp skipped: %s\n", terr.c_str());
    }
  }

  unsigned char* out = nullptr;
  int len = i2d_PKCS7(p7, &out);
  PKCS7_free(p7);
  if (len > 0) {
    der.assign(out, out + len);
    OPENSSL_free(out);
  } else {
    sr->error = "failed to encode CMS: " + ossl_err();
  }
  return der;
}
#endif  // PDFVIEW_HAVE_OPENSSL

// --- native (OpenSSL-free) signing path -------------------------------------

// The pure-C++ stack (crypto-pk + asn1-der + cms) can sign with unencrypted PEM
// credentials and no timestamp. p12, encrypted keys, and RFC 3161 still use
// OpenSSL until those pieces are ported.
bool native_eligible(const SignOptions& o) {
  // Need credentials: a PKCS#12 bundle, or a PEM cert + key.
  bool have_creds = !o.p12_path.empty() ||
                    (!o.cert_pem_path.empty() && !o.key_pem_path.empty());
  if (!have_creds) return false;
  // Native timestamping works over both http and https TSAs (the latter via the
  // hand-rolled TLS 1.3 client in tls_client.cc -- no OpenSSL needed).
  if (!o.tsa_url.empty() && o.tsa_url.rfind("http://", 0) != 0 &&
      o.tsa_url.rfind("https://", 0) != 0)
    return false;
  return true;
}

std::string read_text_file(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

// Current UTC time as an ASN.1 UTCTime string "YYMMDDhhmmssZ".
std::string utc_now() {
  std::time_t t = std::time(nullptr);
  std::tm tmv;
  gmtime_r(&t, &tmv);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%y%m%d%H%M%SZ", &tmv);
  return buf;
}

// Extract the first commonName string from a DER certificate (for reporting).
std::string cert_cn(const std::vector<uint8_t>& der) {
  const uint8_t oid[] = {0x06, 0x03, 0x55, 0x04, 0x03};  // id-at-commonName
  for (size_t i = 0; i + 7 < der.size(); ++i) {
    if (std::memcmp(&der[i], oid, sizeof(oid)) == 0) {
      size_t j = i + sizeof(oid);
      uint8_t tag = der[j];
      size_t len = der[j + 1];
      if ((tag == 0x0c || tag == 0x13 || tag == 0x16) &&
          j + 2 + len <= der.size())
        return std::string(reinterpret_cast<const char*>(&der[j + 2]), len);
    }
  }
  return "";
}

std::string base64(const std::string& in) {
  static const char* T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  int val = 0, bits = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    bits += 8;
    while (bits >= 0) { o += T[(val >> bits) & 0x3F]; bits -= 6; }
  }
  if (bits > -6) o += T[((val << 8) >> (bits + 8)) & 0x3F];
  while (o.size() % 4) o += '=';
  return o;
}

// Minimal HTTP/1.1 POST over a raw TCP socket (http:// only — no TLS). Returns
// the response body bytes, or empty (with @err set) on failure.
std::vector<uint8_t> http_post_raw(const std::string& url,
                                   const std::string& content_type,
                                   const std::string& accept,
                                   const std::string& user,
                                   const std::string& pass,
                                   const std::vector<uint8_t>& body,
                                   std::string* err) {
  std::vector<uint8_t> out;
  bool https = url.rfind("https://", 0) == 0;
  if (!https && url.rfind("http://", 0) != 0) {
    *err = "not an http(s):// URL: " + url;
    return out;
  }
  std::string rest = url.substr(https ? 8 : 7);
  std::string hostport = rest, path = "/";
  size_t slash = rest.find('/');
  if (slash != std::string::npos) { hostport = rest.substr(0, slash); path = rest.substr(slash); }
  std::string host = hostport, port = https ? "443" : "80";
  size_t colon = hostport.find(':');
  if (colon != std::string::npos) { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }

  // https -> hand-rolled TLS 1.3 (no OpenSSL).
  if (https)
    return tls_https_post(host, port, path, content_type, accept, body, err);

  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    *err = "DNS resolution failed for " + host;
    return out;
  }
  int fd = -1;
  for (addrinfo* p = res; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) { *err = "connect failed to " + host + ":" + port; return out; }

  std::string req = "POST " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "Content-Type: " + content_type + "\r\n";
  if (!accept.empty()) req += "Accept: " + accept + "\r\n";
  if (!user.empty())
    req += "Authorization: Basic " + base64(user + ":" + pass) + "\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "Connection: close\r\n\r\n";

  std::string wire = req;
  wire.append(reinterpret_cast<const char*>(body.data()), body.size());
  size_t sent = 0;
  while (sent < wire.size()) {
    ssize_t n = send(fd, wire.data() + sent, wire.size() - sent, 0);
    if (n <= 0) { close(fd); *err = "send failed"; return out; }
    sent += static_cast<size_t>(n);
  }

  std::string resp;
  char buf[8192];
  ssize_t n;
  while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) resp.append(buf, n);
  close(fd);

  size_t hdr_end = resp.find("\r\n\r\n");
  if (hdr_end == std::string::npos) { *err = "no HTTP header terminator"; return out; }
  std::string headers = resp.substr(0, hdr_end);
  std::string bodystr = resp.substr(hdr_end + 4);
  if (headers.find(" 200") == std::string::npos) {
    *err = "HTTP status not 200: " + headers.substr(0, headers.find("\r\n"));
    return out;
  }
  // De-chunk if needed (Connection: close usually gives a plain body).
  std::string lower = headers;
  for (char& c : lower) c = (char)std::tolower((unsigned char)c);
  if (lower.find("transfer-encoding: chunked") != std::string::npos) {
    std::string dec;
    size_t i = 0;
    while (i < bodystr.size()) {
      size_t eol = bodystr.find("\r\n", i);
      if (eol == std::string::npos) break;
      size_t sz = std::strtoul(bodystr.substr(i, eol - i).c_str(), nullptr, 16);
      i = eol + 2;
      if (sz == 0 || i + sz > bodystr.size()) break;
      dec.append(bodystr, i, sz);
      i += sz + 2;
    }
    bodystr.swap(dec);
  }
  out.assign(bodystr.begin(), bodystr.end());
  if (out.empty()) *err = "empty response body";
  return out;
}

// Native (OpenSSL-free) RFC 3161 timestamp callback over http TSAs: hash the
// signature value, request a token, and return it for embedding.
nanopdf::cms::TsaCallback make_native_tsa(const SignOptions& opt,
                                          SignResult* sr) {
  return [opt, sr](const std::vector<uint8_t>& signature) -> std::vector<uint8_t> {
    uint8_t digest[64];
    size_t dlen = 32;
    if (opt.tsa_digest == "sha512") {
      nanopdf::crypto::SHA512::hash(signature.data(), signature.size(), digest);
      dlen = 64;
    } else if (opt.tsa_digest == "sha384") {
      nanopdf::crypto::SHA384::hash(signature.data(), signature.size(), digest);
      dlen = 48;
    } else {
      nanopdf::crypto::SHA256::hash(signature.data(), signature.size(), digest);
      dlen = 32;
    }
    std::random_device rd;
    uint64_t nonce = (uint64_t(rd()) << 32) ^ rd();
    std::vector<uint8_t> req = nanopdf::rfc3161::build_request(
        std::vector<uint8_t>(digest, digest + dlen),
        nanopdf::rfc3161::hash_oid(opt.tsa_digest), nonce, true);
    std::string e;
    std::vector<uint8_t> resp =
        http_post_raw(opt.tsa_url, "application/timestamp-query",
                      "application/timestamp-reply", opt.tsa_username,
                      opt.tsa_password, req, &e);
    if (resp.empty()) {
      std::fprintf(stderr, "pdf_sign: timestamp skipped: %s\n", e.c_str());
      return {};
    }
    int status = 0;
    std::vector<uint8_t> token = nanopdf::rfc3161::parse_response(resp, &status);
    if (token.empty()) {
      std::fprintf(stderr, "pdf_sign: TSA rejected request (status %d)\n",
                   status);
      return {};
    }
    sr->timestamped = true;
    sr->timestamp_authority = opt.tsa_url;
    return token;
  };
}

}  // namespace

// --- TSA presets ------------------------------------------------------------

const TsaPreset* tsa_presets() {
  static const TsaPreset kPresets[] = {
      {"digicert", "DigiCert", "http://timestamp.digicert.com"},
      {"globalsign", "GlobalSign",
       "http://timestamp.globalsign.com/tsa/r6advanced1"},
      {"sectigo", "Sectigo", "http://timestamp.sectigo.com"},
      {"freetsa", "FreeTSA", "https://freetsa.org/tsr"},
      {"opentimestamps", "OpenTimestamps (not RFC 3161)", ""},
      {nullptr, nullptr, nullptr},
  };
  return kPresets;
}

std::string tsa_url_for(const std::string& key) {
  for (const TsaPreset* p = tsa_presets(); p->key; ++p)
    if (key == p->key) return p->url ? p->url : "";
  return "";
}

// --- orchestration ----------------------------------------------------------

SignResult sign_pdf(const std::vector<uint8_t>& in, const SignOptions& opt,
                    std::vector<uint8_t>* out) {
  SignResult sr;
  const bool native = native_eligible(opt);

  // Native (OpenSSL-free) credentials.
  nanopdf::crypto::RsaPrivateKey nkey;
  std::vector<uint8_t> nsigner;
  std::vector<std::vector<uint8_t>> nchain;
  std::string ntime;
#if PDFVIEW_HAVE_OPENSSL
  // OpenSSL credentials (legacy path: https timestamp).
  Credentials cred;
#endif

  if (native) {
    std::vector<std::vector<uint8_t>> certs;
    if (!opt.p12_path.empty()) {
      // PKCS#12 bundle (PBES2/AES; OpenSSL-free).
      std::string raw = read_text_file(opt.p12_path);
      nanopdf::pkcs12::Bundle b = nanopdf::pkcs12::parse(
          reinterpret_cast<const uint8_t*>(raw.data()), raw.size(),
          opt.p12_password);
      if (!b.valid) {
        sr.error = "PKCS#12 load failed: " + b.error;
        return sr;
      }
      nkey = b.key;
      certs = b.certs;
    } else {
      // PEM cert + key (key may be PBES2-encrypted with key_password).
      nkey = nanopdf::crypto::rsa_parse_private_key_pem(
          read_text_file(opt.key_pem_path), opt.key_password);
      if (!nkey.valid) {
        sr.error = "could not parse PEM private key (wrong password or "
                   "unsupported encryption?)";
        return sr;
      }
      certs = nanopdf::cms::pem_to_certs(read_text_file(opt.cert_pem_path));
    }
    if (certs.empty()) {
      sr.error = "no signer certificate found";
      return sr;
    }
    nsigner = certs[0];
    nchain.assign(certs.begin() + 1, certs.end());
    sr.signer_name = cert_cn(nsigner);
    ntime = utc_now();
    std::fprintf(stderr, "pdf_sign: signing with native (OpenSSL-free) CMS\n");
  } else {
#if PDFVIEW_HAVE_OPENSSL
    std::string err;
    if (!load_credentials(opt, &cred, &err)) {
      sr.error = err;
      return sr;
    }
    X509_NAME* nm = X509_get_subject_name(cred.cert);
    char cn[256] = {0};
    if (X509_NAME_get_text_by_NID(nm, NID_commonName, cn, sizeof(cn)) > 0)
      sr.signer_name = cn;
#else
    sr.error =
        "this build has no OpenSSL: https TSAs require an http TSA preset or a "
        "build with -DPDFVIEW_USE_OPENSSL=ON";
    return sr;
#endif
  }

  nanopdf::PdfWriter writer;
  std::string werr;
  if (!writer.load_existing(in, &werr)) {
    sr.error = "load_existing failed: " + werr;
    return sr;
  }
  nanopdf::SignatureFieldConfig cfg;
  cfg.name = opt.field_name;
  cfg.page = opt.page;
  cfg.x = opt.x;
  cfg.y = opt.y;
  cfg.width = opt.width;
  cfg.height = opt.height;
  cfg.visible = opt.visible;
  cfg.reason = opt.reason;
  cfg.location = opt.location;
  cfg.contact_info = opt.contact_info;
  if (writer.add_signature_field(cfg).empty()) {
    sr.error = "add_signature_field failed";
    return sr;
  }

  std::vector<uint8_t> bytes;
  // Reserve generous space: cert chain + RFC 3161 token can be large.
  nanopdf::WriteResult wr = writer.write_incremental_for_signing(bytes, 32768);
  if (!wr.success) {
    sr.error = "write_incremental_for_signing failed: " + wr.error;
    return sr;
  }
  const auto& phs = writer.get_signature_placeholders();
  if (phs.empty()) {
    sr.error = "no signature placeholder produced";
    return sr;
  }

  nanopdf::cms::TsaCallback native_tsa;
  if (native && !opt.tsa_url.empty()) native_tsa = make_native_tsa(opt, &sr);
  nanopdf::SigningCallback cb =
      [&](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
    if (native)
      return nanopdf::cms::build_signed_data(data, nsigner, nchain, nkey, ntime,
                                             native_tsa);
#if PDFVIEW_HAVE_OPENSSL
    return build_cms(data, cred, opt, &sr);
#else
    return {};
#endif
  };
  nanopdf::WriteResult ar = nanopdf::apply_signature(bytes, phs[0], cb);
  if (!ar.success) {
    if (sr.error.empty()) sr.error = "apply_signature failed: " + ar.error;
    return sr;
  }
  *out = std::move(bytes);
  sr.ok = true;
  return sr;
}

// --- OpenTimestamps ---------------------------------------------------------
// OTS calendar servers are https-only, so this path needs TLS (OpenSSL).

std::vector<uint8_t> opentimestamps_stamp(const std::vector<uint8_t>& data,
                                          std::string* errp) {
  std::string err;
  std::vector<uint8_t> ots;
  // 1) SHA-256 of the document (pure C++).
  uint8_t digest[32];
  nanopdf::crypto::SHA256::hash(data.data(), data.size(), digest);

  // 2) Submit the digest to a public calendar; take the first that answers.
  //    The https POST goes through the hand-rolled TLS 1.3 client -- no OpenSSL.
  static const char* kCalendars[] = {
      "https://alice.btc.calendar.opentimestamps.org/digest",
      "https://bob.btc.calendar.opentimestamps.org/digest",
      "https://finney.calendar.eternitywall.com/digest",
      nullptr};
  std::vector<uint8_t> body(digest, digest + 32);
  std::vector<uint8_t> cal;
  for (int i = 0; kCalendars[i]; ++i) {
    std::string e;
    cal = http_post_raw(kCalendars[i], "application/x-www-form-urlencoded",
                        "application/vnd.opentimestamps.v1", "", "", body, &e);
    if (!cal.empty()) break;
    err = e;
  }
  if (cal.empty()) {
    if (errp) *errp = "no calendar responded: " + err;
    return ots;
  }

  // 3) Assemble the detached .ots proof:
  //    HEADER_MAGIC | version(1) | file-hash-op(SHA256=0x08) | digest | ts
  static const unsigned char kMagic[] = {
      0x00, 'O',  'p',  'e',  'n',  'T',  'i',  'm',  'e',  's',  't',
      'a',  'm',  'p',  's',  0x00, 0x00, 'P',  'r',  'o',  'o',  'f',
      0x00, 0xbf, 0x89, 0xe2, 0xe8, 0x84, 0xe8, 0x92, 0x94};
  ots.insert(ots.end(), kMagic, kMagic + sizeof(kMagic));
  ots.push_back(0x01);  // major version (varuint)
  ots.push_back(0x08);  // OpSHA256 tag (the op applied to the file)
  ots.insert(ots.end(), digest, digest + 32);
  ots.insert(ots.end(), cal.begin(), cal.end());
  return ots;
}

// --- verification (pure C++, no OpenSSL) ------------------------------------

namespace {

// Format an ASN.1 UTCTime ("YYMMDDhhmmssZ") or GeneralizedTime
// ("YYYYMMDDhhmmssZ") into "YYYY-MM-DD HH:MM:SS UTC". Falls back to the raw
// string if it doesn't match.
std::string format_asn1_time(const std::string& s) {
  std::string d = s;
  if (!d.empty() && d.back() == 'Z') d.pop_back();
  std::string yyyy, mm, dd, hh, mi, ss;
  if (d.size() >= 12 && d.size() < 14) {  // UTCTime YYMMDDhhmm[ss]
    int yy = (d[0] - '0') * 10 + (d[1] - '0');
    yyyy = std::to_string(yy < 50 ? 2000 + yy : 1900 + yy);
    mm = d.substr(2, 2); dd = d.substr(4, 2); hh = d.substr(6, 2);
    mi = d.substr(8, 2); ss = d.size() >= 12 ? d.substr(10, 2) : "00";
  } else if (d.size() >= 14) {  // GeneralizedTime YYYYMMDDhhmmss
    yyyy = d.substr(0, 4); mm = d.substr(4, 2); dd = d.substr(6, 2);
    hh = d.substr(8, 2); mi = d.substr(10, 2); ss = d.substr(12, 2);
  } else {
    return s;
  }
  return yyyy + "-" + mm + "-" + dd + " " + hh + ":" + mi + ":" + ss + " UTC";
}

}  // namespace

VerifyResult verify_signature(const std::vector<uint8_t>& pdf,
                              const std::vector<uint64_t>& br,
                              const std::vector<uint8_t>& cms) {
  VerifyResult vr;
  if (cms.empty() || br.size() < 4) {
    vr.error = "missing CMS or ByteRange";
    return vr;
  }
  // Reconstruct the signed bytes from the ByteRange.
  std::vector<uint8_t> data;
  bool ranges_ok = true;
  for (size_t i = 0; i + 1 < br.size(); i += 2) {
    uint64_t off = br[i], len = br[i + 1];
    if (off + len > pdf.size()) { ranges_ok = false; break; }
    data.insert(data.end(), pdf.begin() + (ptrdiff_t)off,
                pdf.begin() + (ptrdiff_t)(off + len));
  }
  uint64_t end = br[2] + br[3];
  vr.covers_document = ranges_ok && (end >= pdf.size() - 2);
  if (!ranges_ok) { vr.error = "ByteRange out of bounds"; return vr; }

  // Pure-C++ CMS verification (RSA PKCS#1 v1.5 + SHA-256), no OpenSSL.
  nanopdf::cms::VerifyInfo vi = nanopdf::cms::verify_signed_data(cms, data);
  vr.checked = vi.parsed;
  if (!vi.parsed) {
    vr.error = vi.error.empty() ? "cannot parse CMS" : vi.error;
    return vr;
  }
  vr.signature_valid = vi.signature_valid && vi.digest_valid;
  vr.signer = vi.signer_cn;
  vr.digest_algorithm = vi.digest_algorithm;
  vr.signing_time = format_asn1_time(vi.signing_time);
  vr.has_timestamp = vi.has_timestamp;
  vr.timestamp_time = format_asn1_time(vi.timestamp_time);
  vr.timestamp_authority = vi.timestamp_authority;
  if (!vr.signature_valid && vr.error.empty())
    vr.error = vi.signature_valid ? "digest does not cover document"
                                  : "signature verification failed";
  return vr;
}

SignResult sign_pdf_file(const std::string& in_path, const std::string& out_path,
                         const SignOptions& opt) {
  SignResult sr;
  FILE* f = std::fopen(in_path.c_str(), "rb");
  if (!f) { sr.error = "cannot open input: " + in_path; return sr; }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> in((size_t)(n > 0 ? n : 0));
  if (n > 0 && std::fread(in.data(), 1, (size_t)n, f) != (size_t)n) {
    std::fclose(f);
    sr.error = "read error: " + in_path;
    return sr;
  }
  std::fclose(f);

  std::vector<uint8_t> out;
  sr = sign_pdf(in, opt, &out);
  if (!sr.ok) return sr;
  FILE* of = std::fopen(out_path.c_str(), "wb");
  if (!of) { sr.ok = false; sr.error = "cannot open output: " + out_path; return sr; }
  std::fwrite(out.data(), 1, out.size(), of);
  std::fclose(of);
  return sr;
}

}  // namespace pdfview
