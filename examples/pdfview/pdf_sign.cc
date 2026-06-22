// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// pdf_sign — digital-signature engine for pdfview. The PDF structure work
// (incremental update + ByteRange + apply_signature) uses nanopdf's PdfWriter;
// ALL cryptography runs on the pure-C11 ncrypto library (RSA/CMS/RFC3161/PKCS12)
// and its hand-rolled TLS 1.3 client. No OpenSSL.

#include "pdf_sign.hh"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>

#include "nanopdf.hh"
#include "pdf-writer.hh"
#include "tls_client.hh"

#include "ncrypto/nc_asn1.h"
#include "ncrypto/nc_cms.h"
#include "ncrypto/nc_hash.h"
#include "ncrypto/nc_pkcs12.h"
#include "ncrypto/nc_rfc3161.h"
#include "ncrypto/nc_rsa.h"

namespace pdfview {

namespace {

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

// Decode every "-----BEGIN CERTIFICATE-----" block of a PEM string to DER.
std::vector<std::vector<uint8_t>> pem_to_certs(const std::string& pem) {
  static const char* kBegin = "-----BEGIN CERTIFICATE-----";
  static const char* kEnd = "-----END CERTIFICATE-----";
  int d[256];
  for (int i = 0; i < 256; ++i) d[i] = -1;
  const char* B64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (int i = 0; i < 64; ++i) d[(unsigned char)B64[i]] = i;

  std::vector<std::vector<uint8_t>> out;
  size_t pos = 0;
  while ((pos = pem.find(kBegin, pos)) != std::string::npos) {
    size_t bstart = pos + std::strlen(kBegin);
    size_t bend = pem.find(kEnd, bstart);
    if (bend == std::string::npos) break;
    std::vector<uint8_t> der;
    int val = 0, bits = 0;
    for (size_t i = bstart; i < bend; ++i) {
      int dv = d[(unsigned char)pem[i]];
      if (dv < 0) continue;
      val = (val << 6) | dv;
      bits += 6;
      if (bits >= 8) { bits -= 8; der.push_back((uint8_t)((val >> bits) & 0xFF)); }
    }
    if (!der.empty()) out.push_back(std::move(der));
    pos = bend + std::strlen(kEnd);
  }
  return out;
}

// Minimal HTTP/1.1 POST. http:// goes over a raw TCP socket; https:// is routed
// through the ncrypto TLS 1.3 client. Returns the response body, or empty (with
// @err set) on failure.
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

bool have_credentials(const SignOptions& o) {
  return !o.p12_path.empty() ||
         (!o.cert_pem_path.empty() && !o.key_pem_path.empty());
}

// Context handed to the C TSA callback during CMS construction.
struct TsaCtx {
  const SignOptions* opt;
  SignResult* sr;
};

// nc_tsa_cb: hash the signature value, fetch an RFC 3161 token over http/https,
// and append it for embedding as an unsigned attribute. Returns 0 on success.
int tsa_callback(const uint8_t* signature, size_t sig_len, nc_buf* token_out,
                 void* user) {
  TsaCtx* ctx = static_cast<TsaCtx*>(user);
  const SignOptions& opt = *ctx->opt;

  uint8_t digest[64];
  size_t dlen = 32;
  if (opt.tsa_digest == "sha512") {
    nc_sha512(signature, sig_len, digest);
    dlen = 64;
  } else if (opt.tsa_digest == "sha384") {
    nc_sha384(signature, sig_len, digest);
    dlen = 48;
  } else {
    nc_sha256(signature, sig_len, digest);
    dlen = 32;
  }

  std::random_device rd;
  uint64_t nonce = (uint64_t(rd()) << 32) ^ rd();
  const char* oid = nc_rfc3161_hash_oid(opt.tsa_digest.c_str());
  if (!oid) oid = nc_rfc3161_hash_oid("sha256");

  nc_buf req;
  nc_buf_init(&req);
  if (nc_rfc3161_build_request(&req, digest, dlen, oid, nonce, 1) != 0) {
    nc_buf_free(&req);
    return 1;
  }
  std::vector<uint8_t> reqv(req.data, req.data + req.len);
  nc_buf_free(&req);

  std::string e;
  std::vector<uint8_t> resp =
      http_post_raw(opt.tsa_url, "application/timestamp-query",
                    "application/timestamp-reply", opt.tsa_username,
                    opt.tsa_password, reqv, &e);
  if (resp.empty()) {
    std::fprintf(stderr, "pdf_sign: timestamp skipped: %s\n", e.c_str());
    return 1;
  }

  int status = 0;
  nc_buf token;
  nc_buf_init(&token);
  if (nc_rfc3161_parse_response(resp.data(), resp.size(), &status, &token) != 0 ||
      token.len == 0) {
    std::fprintf(stderr, "pdf_sign: TSA rejected request (status %d)\n", status);
    nc_buf_free(&token);
    return 1;
  }
  nc_buf_put(token_out, token.data, token.len);
  nc_buf_free(&token);
  ctx->sr->timestamped = true;
  ctx->sr->timestamp_authority = opt.tsa_url;
  return 0;
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
  if (!have_credentials(opt)) {
    sr.error = "no credentials: provide a PKCS#12 (--p12) or PEM cert+key";
    return sr;
  }

  // Load the signer key + certificate chain via ncrypto.
  nc_rsa_privkey nkey;
  std::memset(&nkey, 0, sizeof(nkey));
  std::vector<std::vector<uint8_t>> certs;
  nc_pkcs12_bundle p12;
  std::memset(&p12, 0, sizeof(p12));
  bool have_p12 = false;

  if (!opt.p12_path.empty()) {
    std::string raw = read_text_file(opt.p12_path);
    if (nc_pkcs12_parse(&p12, reinterpret_cast<const uint8_t*>(raw.data()),
                        raw.size(),
                        opt.p12_password.c_str()) != 0 || !p12.ok) {
      sr.error = std::string("PKCS#12 load failed: ") +
                 (p12.error[0] ? p12.error : "unknown");
      nc_pkcs12_bundle_free(&p12);
      return sr;
    }
    have_p12 = true;
    nkey = p12.key;
    for (int i = 0; i < p12.cert_count; ++i)
      certs.emplace_back(p12.certs[i], p12.certs[i] + p12.cert_lens[i]);
  } else {
    std::string keypem = read_text_file(opt.key_pem_path);
    if (nc_rsa_parse_privkey_pem(
            &nkey, keypem.c_str(),
            opt.key_password.empty() ? nullptr : opt.key_password.c_str()) != 0 ||
        !nkey.valid) {
      sr.error = "could not parse PEM private key (wrong password or "
                 "unsupported encryption?)";
      return sr;
    }
    certs = pem_to_certs(read_text_file(opt.cert_pem_path));
  }

  if (certs.empty()) {
    sr.error = "no signer certificate found";
    if (have_p12) nc_pkcs12_bundle_free(&p12);
    return sr;
  }
  std::vector<uint8_t> nsigner = certs[0];
  std::vector<std::vector<uint8_t>> nchain(certs.begin() + 1, certs.end());
  sr.signer_name = cert_cn(nsigner);
  std::string ntime = utc_now();
  std::fprintf(stderr, "pdf_sign: signing with ncrypto (pure C11) CMS\n");

  nanopdf::PdfWriter writer;
  std::string werr;
  if (!writer.load_existing(in, &werr)) {
    sr.error = "load_existing failed: " + werr;
    if (have_p12) nc_pkcs12_bundle_free(&p12);
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
    if (have_p12) nc_pkcs12_bundle_free(&p12);
    return sr;
  }

  std::vector<uint8_t> bytes;
  // Reserve generous space: cert chain + RFC 3161 token can be large.
  nanopdf::WriteResult wr = writer.write_incremental_for_signing(bytes, 32768);
  if (!wr.success) {
    sr.error = "write_incremental_for_signing failed: " + wr.error;
    if (have_p12) nc_pkcs12_bundle_free(&p12);
    return sr;
  }
  const auto& phs = writer.get_signature_placeholders();
  if (phs.empty()) {
    sr.error = "no signature placeholder produced";
    if (have_p12) nc_pkcs12_bundle_free(&p12);
    return sr;
  }

  const bool want_tsa = !opt.tsa_url.empty();
  TsaCtx tctx{&opt, &sr};

  // The signing callback builds a detached CMS SignedData over the ByteRange
  // content using ncrypto, optionally embedding an RFC 3161 timestamp token.
  nanopdf::SigningCallback cb =
      [&](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
    std::vector<const uint8_t*> chain_ptrs;
    std::vector<size_t> chain_lens;
    for (const auto& c : nchain) {
      chain_ptrs.push_back(c.data());
      chain_lens.push_back(c.size());
    }
    nc_buf cms;
    nc_buf_init(&cms);
    int rc = nc_cms_build_signed_data(
        &cms, data.data(), data.size(), nsigner.data(), nsigner.size(),
        chain_ptrs.empty() ? nullptr : chain_ptrs.data(),
        chain_lens.empty() ? nullptr : chain_lens.data(),
        (int)chain_ptrs.size(), &nkey, ntime.c_str(),
        want_tsa ? tsa_callback : nullptr, want_tsa ? &tctx : nullptr);
    std::vector<uint8_t> sig;
    if (rc == 0) sig.assign(cms.data, cms.data + cms.len);
    nc_buf_free(&cms);
    return sig;
  };

  nanopdf::WriteResult ar = nanopdf::apply_signature(bytes, phs[0], cb);
  if (have_p12) nc_pkcs12_bundle_free(&p12);
  if (!ar.success) {
    if (sr.error.empty()) sr.error = "apply_signature failed: " + ar.error;
    return sr;
  }
  *out = std::move(bytes);
  sr.ok = true;
  return sr;
}

// --- OpenTimestamps ---------------------------------------------------------

std::vector<uint8_t> opentimestamps_stamp(const std::vector<uint8_t>& data,
                                          std::string* errp) {
  std::string err;
  std::vector<uint8_t> ots;
  // 1) SHA-256 of the document.
  uint8_t digest[32];
  nc_sha256(data.data(), data.size(), digest);

  // 2) Submit the digest to a public calendar; take the first that answers.
  //    The https POST goes through the ncrypto TLS 1.3 client -- no OpenSSL.
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

// --- verification -----------------------------------------------------------

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

  // Pure-C11 CMS verification (RSA PKCS#1 v1.5 + SHA-256), no OpenSSL.
  nc_cms_verify_info info;
  std::memset(&info, 0, sizeof(info));
  nc_cms_verify_signed_data(&info, cms.data(), cms.size(), data.data(),
                            data.size());
  vr.checked = info.parsed;
  if (!info.parsed) {
    vr.error = info.error[0] ? info.error : "cannot parse CMS";
    return vr;
  }
  vr.signature_valid = info.signature_valid && info.digest_valid;
  vr.signer = info.signer_cn;
  vr.digest_algorithm = info.digest_algorithm;
  vr.signing_time = format_asn1_time(info.signing_time);
  vr.has_timestamp = info.has_timestamp != 0;
  vr.timestamp_time = format_asn1_time(info.timestamp_time);
  vr.timestamp_authority = info.timestamp_authority;
  if (!vr.signature_valid && vr.error.empty())
    vr.error = info.signature_valid ? "digest does not cover document"
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
