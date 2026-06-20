// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "pdf_sign.hh"

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
#include "rfc3161.hh"

namespace pdfview {

namespace {

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

// --- native (OpenSSL-free) signing path -------------------------------------

// The pure-C++ stack (crypto-pk + asn1-der + cms) can sign with unencrypted PEM
// credentials and no timestamp. p12, encrypted keys, and RFC 3161 still use
// OpenSSL until those pieces are ported.
bool native_eligible(const SignOptions& o) {
  if (!o.p12_path.empty() || o.cert_pem_path.empty() ||
      o.key_pem_path.empty() || !o.key_password.empty())
    return false;
  // Native timestamping works over http TSAs (no TLS yet); https falls back to
  // the OpenSSL path.
  if (o.tsa_url.empty()) return true;
  return o.tsa_url.rfind("http://", 0) == 0;
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
  if (url.rfind("http://", 0) != 0) { *err = "not an http:// URL: " + url; return out; }
  std::string rest = url.substr(7);
  std::string hostport = rest, path = "/";
  size_t slash = rest.find('/');
  if (slash != std::string::npos) { hostport = rest.substr(0, slash); path = rest.substr(slash); }
  std::string host = hostport, port = "80";
  size_t colon = hostport.find(':');
  if (colon != std::string::npos) { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }

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
  // OpenSSL credentials (p12 / encrypted key / timestamp path).
  Credentials cred;

  if (native) {
    nkey = nanopdf::crypto::rsa_parse_private_key_pem(
        read_text_file(opt.key_pem_path));
    if (!nkey.valid) {
      sr.error =
          "could not parse PEM private key (native path supports unencrypted "
          "RSA keys only)";
      return sr;
    }
    std::vector<std::vector<uint8_t>> certs =
        nanopdf::cms::pem_to_certs(read_text_file(opt.cert_pem_path));
    if (certs.empty()) {
      sr.error = "no certificate found in " + opt.cert_pem_path;
      return sr;
    }
    nsigner = certs[0];
    nchain.assign(certs.begin() + 1, certs.end());
    sr.signer_name = cert_cn(nsigner);
    ntime = utc_now();
    std::fprintf(stderr, "pdf_sign: signing with native (OpenSSL-free) CMS\n");
  } else {
    std::string err;
    if (!load_credentials(opt, &cred, &err)) {
      sr.error = err;
      return sr;
    }
    X509_NAME* nm = X509_get_subject_name(cred.cert);
    char cn[256] = {0};
    if (X509_NAME_get_text_by_NID(nm, NID_commonName, cn, sizeof(cn)) > 0)
      sr.signer_name = cn;
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
    return build_cms(data, cred, opt, &sr);
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

namespace {

// Minimal HTTP POST of a binary body via OSSL_HTTP (http + https). Returns the
// raw response bytes, or empty on failure.
std::vector<uint8_t> http_post_binary(const std::string& url,
                                      const std::string& content_type,
                                      const std::string& accept,
                                      const std::vector<uint8_t>& body,
                                      std::string* err) {
  std::vector<uint8_t> out;
  char *host = nullptr, *port = nullptr, *path = nullptr;
  int use_ssl = 0;
  if (!OSSL_HTTP_parse_url(url.c_str(), &use_ssl, nullptr, &host, &port, nullptr,
                           &path, nullptr, nullptr)) {
    *err = "bad url: " + url;
    return out;
  }
  SSL_CTX* tls = nullptr;
  if (use_ssl) {
    tls = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(tls, SSL_VERIFY_NONE, nullptr);
  }
  BIO* reqbio = BIO_new_mem_buf(body.data(), (int)body.size());
  OSSL_HTTP_bio_cb_t tls_cb = use_ssl ? http_tls_cb : nullptr;
  OSSL_HTTP_REQ_CTX* rctx = OSSL_HTTP_open(host, port, nullptr, nullptr, use_ssl,
                                           nullptr, nullptr, tls_cb, tls, 0,
                                           30000);
  if (rctx) {
    STACK_OF(CONF_VALUE)* hdrs = nullptr;
    if (!accept.empty()) X509V3_add_value("Accept", accept.c_str(), &hdrs);
    if (OSSL_HTTP_set1_request(rctx, path, hdrs, content_type.c_str(), reqbio,
                               nullptr, /*expect_asn1=*/0, /*max_resp_len=*/0,
                               30000, 0)) {
      BIO* rbio = OSSL_HTTP_exchange(rctx, nullptr);
      if (rbio) {
        unsigned char buf[4096];
        int n;
        while ((n = BIO_read(rbio, buf, sizeof(buf))) > 0)
          out.insert(out.end(), buf, buf + n);
      }
    }
    if (hdrs) sk_CONF_VALUE_pop_free(hdrs, X509V3_conf_free);
    OSSL_HTTP_close(rctx, 1);
  }
  BIO_free(reqbio);
  OPENSSL_free(host);
  OPENSSL_free(port);
  OPENSSL_free(path);
  if (tls) SSL_CTX_free(tls);
  if (out.empty()) *err = "no response from " + url;
  return out;
}

}  // namespace

std::vector<uint8_t> opentimestamps_stamp(const std::vector<uint8_t>& data,
                                          std::string* errp) {
  std::string err;
  std::vector<uint8_t> ots;
  // 1) SHA-256 of the document.
  unsigned char digest[32];
  unsigned int dlen = 0;
  EVP_Digest(data.data(), data.size(), digest, &dlen, EVP_sha256(), nullptr);

  // 2) Submit the digest to a public calendar; take the first that answers.
  static const char* kCalendars[] = {
      "https://alice.btc.calendar.opentimestamps.org/digest",
      "https://bob.btc.calendar.opentimestamps.org/digest",
      "https://finney.calendar.eternitywall.com/digest",
      nullptr};
  std::vector<uint8_t> body(digest, digest + dlen);
  std::vector<uint8_t> cal;
  for (int i = 0; kCalendars[i]; ++i) {
    std::string e;
    cal = http_post_binary(kCalendars[i], "application/x-www-form-urlencoded",
                           "application/vnd.opentimestamps.v1", body, &e);
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
  ots.insert(ots.end(), digest, digest + dlen);
  ots.insert(ots.end(), cal.begin(), cal.end());
  return ots;
}

// --- verification -----------------------------------------------------------

namespace {

std::string asn1_time_str(const ASN1_TIME* t) {
  if (!t) return "";
  BIO* b = BIO_new(BIO_s_mem());
  ASN1_TIME_print(b, t);
  char buf[64] = {0};
  int n = BIO_read(b, buf, sizeof(buf) - 1);
  BIO_free(b);
  return n > 0 ? std::string(buf, (size_t)n) : "";
}

// Parse the id-aa-timeStampToken unsigned attribute, returning genTime + TSA.
void parse_timestamp(PKCS7* p7, VerifyResult* vr) {
  STACK_OF(PKCS7_SIGNER_INFO)* sis = PKCS7_get_signer_info(p7);
  if (!sis || sk_PKCS7_SIGNER_INFO_num(sis) < 1) return;
  PKCS7_SIGNER_INFO* si = sk_PKCS7_SIGNER_INFO_value(sis, 0);
  if (!si->unauth_attr) return;
  ASN1_OBJECT* oid = OBJ_txt2obj("1.2.840.113549.1.9.16.2.14", 1);
  for (int i = 0; i < sk_X509_ATTRIBUTE_num(si->unauth_attr); ++i) {
    X509_ATTRIBUTE* at = sk_X509_ATTRIBUTE_value(si->unauth_attr, i);
    if (OBJ_cmp(X509_ATTRIBUTE_get0_object(at), oid) != 0) continue;
    ASN1_TYPE* tv = X509_ATTRIBUTE_get0_type(at, 0);
    if (!tv || tv->type != V_ASN1_SEQUENCE) break;
    const unsigned char* p = tv->value.sequence->data;
    PKCS7* token = d2i_PKCS7(nullptr, &p, tv->value.sequence->length);
    if (token) {
      TS_TST_INFO* tst = PKCS7_to_TS_TST_INFO(token);
      if (tst) {
        vr->has_timestamp = true;
        vr->timestamp_time = asn1_time_str(TS_TST_INFO_get_time(tst));
        const GENERAL_NAME* tsa = TS_TST_INFO_get_tsa(tst);
        if (tsa && tsa->type == GEN_DIRNAME) {
          char nm[256] = {0};
          X509_NAME_get_text_by_NID(tsa->d.directoryName, NID_commonName, nm,
                                    sizeof(nm));
          vr->timestamp_authority = nm;
        }
        TS_TST_INFO_free(tst);
      }
      PKCS7_free(token);
    }
    break;
  }
  ASN1_OBJECT_free(oid);
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
  const unsigned char* p = cms.data();
  PKCS7* p7 = d2i_PKCS7(nullptr, &p, (long)cms.size());
  if (!p7) { vr.error = "cannot parse CMS: " + ossl_err(); return vr; }
  vr.checked = true;

  // Reconstruct the signed bytes from the ByteRange.
  std::vector<uint8_t> data;
  bool ranges_ok = true;
  for (size_t i = 0; i + 1 < br.size(); i += 2) {
    uint64_t off = br[i], len = br[i + 1];
    if (off + len > pdf.size()) { ranges_ok = false; break; }
    data.insert(data.end(), pdf.begin() + (ptrdiff_t)off,
                pdf.begin() + (ptrdiff_t)(off + len));
  }
  // "covers document": the ByteRange leaves only the /Contents hex gap.
  if (br.size() >= 4) {
    uint64_t end = br[2] + br[3];
    vr.covers_document = ranges_ok && (end >= pdf.size() - 2);
  }

  if (ranges_ok) {
    BIO* indata = BIO_new_mem_buf(data.data(), (int)data.size());
    // PKCS7_NOVERIFY: don't require a trusted CA chain (self-signed ok); the
    // signature math + ByteRange digest are still verified.
    int rc = PKCS7_verify(p7, nullptr, nullptr, indata, nullptr,
                          PKCS7_NOVERIFY);
    vr.signature_valid = (rc == 1);
    if (rc != 1) vr.error = "signature invalid: " + ossl_err();
    BIO_free(indata);
  } else {
    vr.error = "ByteRange out of bounds";
  }

  // Signer identity + digest algorithm + signing time.
  STACK_OF(X509)* signers = PKCS7_get0_signers(p7, nullptr, PKCS7_NOVERIFY);
  if (signers && sk_X509_num(signers) > 0) {
    X509* sc = sk_X509_value(signers, 0);
    X509_NAME* nm = X509_get_subject_name(sc);
    char cn[256] = {0};
    if (X509_NAME_get_text_by_NID(nm, NID_commonName, cn, sizeof(cn)) > 0)
      vr.signer = cn;
    char* dn = X509_NAME_oneline(nm, nullptr, 0);
    if (dn) { vr.signer_dn = dn; OPENSSL_free(dn); }
  }
  if (signers) sk_X509_free(signers);

  STACK_OF(PKCS7_SIGNER_INFO)* sis = PKCS7_get_signer_info(p7);
  if (sis && sk_PKCS7_SIGNER_INFO_num(sis) > 0) {
    PKCS7_SIGNER_INFO* si = sk_PKCS7_SIGNER_INFO_value(sis, 0);
    if (si->digest_alg)
      vr.digest_algorithm = OBJ_nid2sn(OBJ_obj2nid(si->digest_alg->algorithm));
    ASN1_TYPE* st = PKCS7_get_signed_attribute(si, NID_pkcs9_signingTime);
    if (st && st->type == V_ASN1_UTCTIME)
      vr.signing_time = asn1_time_str(st->value.utctime);
    else if (st && st->type == V_ASN1_GENERALIZEDTIME)
      vr.signing_time = asn1_time_str((ASN1_TIME*)st->value.generalizedtime);
  }

  parse_timestamp(p7, &vr);
  PKCS7_free(p7);
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
