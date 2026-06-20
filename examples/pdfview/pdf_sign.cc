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

#include <cstdio>
#include <cstring>
#include <memory>

#include "nanopdf.hh"
#include "pdf-writer.hh"

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
  Credentials cred;
  std::string err;
  if (!load_credentials(opt, &cred, &err)) {
    sr.error = err;
    return sr;
  }
  // Common name for reporting.
  {
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

  nanopdf::SigningCallback cb =
      [&](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
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
