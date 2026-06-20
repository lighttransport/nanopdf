// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "x509.hh"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "cms.hh"     // pem_to_certs (reused for the trust bundle)
#include "crypto.hh"  // SHA-1/256/384/512

namespace nanopdf {
namespace x509 {

namespace {

// ---- minimal DER reader ----------------------------------------------------
struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
};

// Read tag+length; on success [*vbeg,*vend) is the value, r.p advances past it.
uint8_t read_tl(Reader& r, const uint8_t** vbeg, const uint8_t** vend) {
  if (!r.ok || r.p + 2 > r.end) { r.ok = false; return 0; }
  uint8_t tag = *r.p++;
  size_t len = *r.p++;
  if (len & 0x80) {
    size_t nb = len & 0x7F;
    if (nb == 0 || nb > 4 || r.p + nb > r.end) { r.ok = false; return 0; }
    len = 0;
    for (size_t i = 0; i < nb; ++i) len = (len << 8) | *r.p++;
  }
  if (r.p + len > r.end) { r.ok = false; return 0; }
  *vbeg = r.p;
  *vend = r.p + len;
  r.p += len;
  return tag;
}

Reader enter(const uint8_t* b, const uint8_t* e) { return Reader{b, e, true}; }

bool oid_eq(const uint8_t* b, const uint8_t* e, const uint8_t* oid,
            size_t n) {
  return (size_t)(e - b) == n && std::memcmp(b, oid, n) == 0;
}

// OID value bytes.
const uint8_t kRsaEncryption[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01,
                                  0x01, 0x01};
const uint8_t kRsaSha1[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01,
                            0x05};
const uint8_t kRsaSha256[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01,
                              0x0B};
const uint8_t kRsaSha384[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01,
                              0x0C};
const uint8_t kRsaSha512[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01,
                              0x0D};
const uint8_t kRsaPss[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01,
                           0x0A};
const uint8_t kEcPublicKey[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
const uint8_t kCurveP256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};
const uint8_t kCurveP384[] = {0x2B, 0x81, 0x04, 0x00, 0x22};
const uint8_t kEcdsaSha1[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x01};
const uint8_t kEcdsaSha256[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02};
const uint8_t kEcdsaSha384[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03};
const uint8_t kEcdsaSha512[] = {0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04};
const uint8_t kHashSha256[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                               0x01};
const uint8_t kHashSha384[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                               0x02};
const uint8_t kHashSha512[] = {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02,
                               0x03};
const uint8_t kOidSan[] = {0x55, 0x1D, 0x11};
const uint8_t kOidBasicConstraints[] = {0x55, 0x1D, 0x13};

#define OIDEQ(b, e, arr) oid_eq((b), (e), (arr), sizeof(arr))

// Parse the AlgorithmIdentifier at [b,e) (a SEQUENCE value) into a SigAlg.
// For RsaPss, also recover the message-hash length from the params.
SigAlg parse_sig_alg(const uint8_t* b, const uint8_t* e, size_t* pss_hlen) {
  Reader r = enter(b, e);
  const uint8_t *ob, *oe;
  if (read_tl(r, &ob, &oe) != 0x06) return SigAlg::Unknown;  // OID
  if (OIDEQ(ob, oe, kRsaSha256)) return SigAlg::RsaPkcs1Sha256;
  if (OIDEQ(ob, oe, kRsaSha384)) return SigAlg::RsaPkcs1Sha384;
  if (OIDEQ(ob, oe, kRsaSha512)) return SigAlg::RsaPkcs1Sha512;
  if (OIDEQ(ob, oe, kRsaSha1)) return SigAlg::RsaPkcs1Sha1;
  if (OIDEQ(ob, oe, kEcdsaSha256)) return SigAlg::EcdsaSha256;
  if (OIDEQ(ob, oe, kEcdsaSha384)) return SigAlg::EcdsaSha384;
  if (OIDEQ(ob, oe, kEcdsaSha512)) return SigAlg::EcdsaSha512;
  if (OIDEQ(ob, oe, kEcdsaSha1)) return SigAlg::EcdsaSha1;
  if (OIDEQ(ob, oe, kRsaPss)) {
    *pss_hlen = 32;  // default SHA-256 if params absent
    // params: SEQUENCE { [0] hashAlgorithm AlgorithmIdentifier, ... }
    const uint8_t *pb, *pe;
    if (read_tl(r, &pb, &pe) == 0x30) {
      Reader pr = enter(pb, pe);
      const uint8_t *cb, *ce;
      if (read_tl(pr, &cb, &ce) == 0xA0) {  // [0] hashAlgorithm
        Reader hr = enter(cb, ce);
        const uint8_t *hb, *he;
        if (read_tl(hr, &hb, &he) == 0x30) {
          Reader ar = enter(hb, he);
          const uint8_t *hob, *hoe;
          if (read_tl(ar, &hob, &hoe) == 0x06) {
            if (OIDEQ(hob, hoe, kHashSha384)) *pss_hlen = 48;
            else if (OIDEQ(hob, hoe, kHashSha512)) *pss_hlen = 64;
            else *pss_hlen = 32;
          }
        }
      }
    }
    return SigAlg::RsaPss;
  }
  return SigAlg::Unknown;
}

// ASN.1 time (UTCTime / GeneralizedTime) -> epoch seconds. 0 on failure.
int64_t parse_time(uint8_t tag, const uint8_t* b, const uint8_t* e) {
  std::string s(reinterpret_cast<const char*>(b), e - b);
  struct tm tmv;
  std::memset(&tmv, 0, sizeof(tmv));
  size_t i = 0;
  auto two = [&](void) -> int {
    if (i + 1 >= s.size()) return -1;
    int v = (s[i] - '0') * 10 + (s[i + 1] - '0');
    i += 2;
    return v;
  };
  int year;
  if (tag == 0x17) {  // UTCTime YY...
    int yy = two();
    if (yy < 0) return 0;
    year = (yy < 50) ? 2000 + yy : 1900 + yy;
  } else if (tag == 0x18) {  // GeneralizedTime YYYY...
    int hi = two(), lo = two();
    if (hi < 0 || lo < 0) return 0;
    year = hi * 100 + lo;
  } else {
    return 0;
  }
  int mon = two(), day = two(), hour = two(), min = two();
  int sec = 0;
  if (i + 1 < s.size() && s[i] >= '0' && s[i] <= '9') sec = two();
  if (mon < 1 || day < 0 || hour < 0 || min < 0) return 0;
  tmv.tm_year = year - 1900;
  tmv.tm_mon = mon - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = min;
  tmv.tm_sec = sec;
  return (int64_t)timegm(&tmv);
}

// Pull RSA (n,e) or EC point from a SubjectPublicKeyInfo value [b,e).
void parse_spki(const uint8_t* b, const uint8_t* e, Certificate* c) {
  Reader spki = enter(b, e);
  const uint8_t *ab, *ae;
  if (read_tl(spki, &ab, &ae) != 0x30) return;  // algorithm AlgorithmIdentifier
  Reader alg = enter(ab, ae);
  const uint8_t *ob, *oe;
  if (read_tl(alg, &ob, &oe) != 0x06) return;  // OID
  bool is_rsa = OIDEQ(ob, oe, kRsaEncryption);
  bool is_ec = OIDEQ(ob, oe, kEcPublicKey);
  const crypto::EcCurve* curve = nullptr;
  if (is_ec) {
    const uint8_t *cb, *ce;
    if (read_tl(alg, &cb, &ce) == 0x06) {  // named curve OID
      if (OIDEQ(cb, ce, kCurveP256)) curve = &crypto::curve_p256();
      else if (OIDEQ(cb, ce, kCurveP384)) curve = &crypto::curve_p384();
    }
  }
  const uint8_t *kb, *ke;
  if (read_tl(spki, &kb, &ke) != 0x03) return;  // subjectPublicKey BIT STRING
  if (kb >= ke || *kb != 0x00) return;          // unused-bits byte must be 0
  const uint8_t* key = kb + 1;
  size_t key_len = ke - (kb + 1);
  if (is_rsa) {
    Reader rsaw = enter(key, ke);
    const uint8_t *sb, *se;
    if (read_tl(rsaw, &sb, &se) != 0x30) return;  // RSAPublicKey SEQUENCE
    Reader rsa = enter(sb, se);
    const uint8_t *nb, *ne, *eb, *ee;
    if (read_tl(rsa, &nb, &ne) != 0x02) return;
    if (read_tl(rsa, &eb, &ee) != 0x02) return;
    c->rsa_pub.n = crypto::BigInt::from_bytes(nb, ne - nb);
    c->rsa_pub.e = crypto::BigInt::from_bytes(eb, ee - eb);
    c->rsa_pub.modulus_bytes = (c->rsa_pub.n.bit_length() + 7) / 8;
    c->rsa_pub.valid = !c->rsa_pub.n.is_zero();
    c->key_type = KeyType::Rsa;
  } else if (is_ec && curve) {
    c->key_type = KeyType::Ec;
    c->ec_curve = curve;
    c->ec_point.assign(key, key + key_len);
  }
}

// Walk the extensions SEQUENCE value for SAN dNSNames and basicConstraints CA.
void parse_extensions(const uint8_t* b, const uint8_t* e, Certificate* c) {
  Reader exts = enter(b, e);
  const uint8_t *xb, *xe;
  while (exts.ok && exts.p < exts.end) {
    if (read_tl(exts, &xb, &xe) != 0x30) break;  // Extension SEQUENCE
    Reader ext = enter(xb, xe);
    const uint8_t *ob, *oe;
    if (read_tl(ext, &ob, &oe) != 0x06) continue;  // extnID
    // optional critical BOOLEAN, then OCTET STRING extnValue
    const uint8_t *vb, *ve;
    uint8_t t = read_tl(ext, &vb, &ve);
    if (t == 0x01) t = read_tl(ext, &vb, &ve);  // skip critical
    if (t != 0x04) continue;                     // extnValue OCTET STRING
    if (OIDEQ(ob, oe, kOidSan)) {
      Reader san = enter(vb, ve);
      const uint8_t *gb, *ge;
      if (read_tl(san, &gb, &ge) != 0x30) continue;  // GeneralNames SEQUENCE
      Reader names = enter(gb, ge);
      const uint8_t *nb, *ne;
      while (names.ok && names.p < names.end) {
        uint8_t gt = read_tl(names, &nb, &ne);
        if (!names.ok) break;
        if (gt == 0x82)  // [2] dNSName (IA5String, implicit)
          c->san_dns.emplace_back(reinterpret_cast<const char*>(nb), ne - nb);
      }
    } else if (OIDEQ(ob, oe, kOidBasicConstraints)) {
      Reader bc = enter(vb, ve);
      const uint8_t *sb, *se;
      if (read_tl(bc, &sb, &se) != 0x30) continue;  // SEQUENCE
      Reader seq = enter(sb, se);
      const uint8_t *cb, *ce;
      if (read_tl(seq, &cb, &ce) == 0x01)  // cA BOOLEAN
        c->is_ca = (ce > cb && cb[0] != 0x00);
    }
  }
}

// DER ECDSA-Sig-Value SEQUENCE { INTEGER r, INTEGER s } -> r,s magnitudes.
bool parse_ecdsa_sig(const uint8_t* sig, size_t len, Bytes* r, Bytes* s) {
  Reader top = enter(sig, sig + len);
  const uint8_t *sb, *se;
  if (read_tl(top, &sb, &se) != 0x30) return false;
  Reader seq = enter(sb, se);
  const uint8_t *rb, *re, *ssb, *sse;
  if (read_tl(seq, &rb, &re) != 0x02) return false;
  if (read_tl(seq, &ssb, &sse) != 0x02) return false;
  while (rb < re && *rb == 0x00) ++rb;   // strip sign byte
  while (ssb < sse && *ssb == 0x00) ++ssb;
  r->assign(rb, re);
  s->assign(ssb, sse);
  return true;
}

// Hash @data with the algorithm implied by digest length (20/32/48/64).
Bytes hash_len(const uint8_t* data, size_t n, size_t hlen) {
  Bytes out(hlen);
  if (hlen == 20) crypto::SHA1::hash(data, n, out.data());
  else if (hlen == 32) crypto::SHA256::hash(data, n, out.data());
  else if (hlen == 48) crypto::SHA384::hash(data, n, out.data());
  else if (hlen == 64) crypto::SHA512::hash(data, n, out.data());
  else out.clear();
  return out;
}

// DigestInfo prefixes for RSA PKCS#1 v1.5.
const uint8_t kDiSha1[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E, 0x03,
                           0x02, 0x1A, 0x05, 0x00, 0x04, 0x14};
const uint8_t kDiSha256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
                             0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
                             0x00, 0x04, 0x20};
const uint8_t kDiSha384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
                             0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05,
                             0x00, 0x04, 0x30};
const uint8_t kDiSha512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
                             0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05,
                             0x00, 0x04, 0x40};

bool rsa_pkcs1_verify(const crypto::RsaPublicKey& key, const Bytes& sig,
                      const Bytes& hash, const uint8_t* di_prefix,
                      size_t di_len) {
  Bytes di(di_prefix, di_prefix + di_len);
  di.insert(di.end(), hash.begin(), hash.end());
  return crypto::rsa_verify_pkcs1v15(key, sig.data(), sig.size(), di.data(),
                                     di.size());
}

bool to_lower_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
      return false;
  return true;
}

// Match @host against a SAN pattern (case-insensitive; leftmost-label wildcard).
bool host_match(const std::string& host, const std::string& pat) {
  if (pat.empty()) return false;
  if (pat[0] == '*' && pat.size() > 2 && pat[1] == '.') {
    // Wildcard matches exactly one leftmost label.
    size_t dot = host.find('.');
    if (dot == std::string::npos) return false;
    return to_lower_eq(host.substr(dot), pat.substr(1));
  }
  return to_lower_eq(host, pat);
}

const char* kBundlePaths[] = {
    "/etc/ssl/certs/ca-certificates.crt",      // Debian/Ubuntu
    "/etc/pki/tls/certs/ca-bundle.crt",        // RHEL/Fedora
    "/etc/ssl/ca-bundle.pem",                  // openSUSE
    "/etc/ssl/cert.pem",                       // Alpine/macOS/BSD
    nullptr};

}  // namespace

std::string Certificate::subject_cn() const {
  // Scan the subject Name DER for the commonName attribute (OID 2.5.4.3).
  const uint8_t kCn[] = {0x06, 0x03, 0x55, 0x04, 0x03};
  const uint8_t* b = subject_der.data();
  const uint8_t* e = b + subject_der.size();
  for (const uint8_t* p = b; p + 7 < e; ++p) {
    if (std::memcmp(p, kCn, sizeof(kCn)) == 0) {
      const uint8_t* q = p + sizeof(kCn);
      uint8_t tag = q[0];
      size_t len = q[1];
      if ((tag == 0x0c || tag == 0x13 || tag == 0x16) && q + 2 + len <= e)
        return std::string(reinterpret_cast<const char*>(q + 2), len);
    }
  }
  return "";
}

Certificate parse(const uint8_t* der, size_t len) {
  Certificate c;
  Reader top{der, der + len, true};
  const uint8_t *vb, *ve;
  if (read_tl(top, &vb, &ve) != 0x30) return c;  // Certificate SEQUENCE
  Reader cert = enter(vb, ve);

  // tbsCertificate — capture full DER (tag..value) for signature verification.
  const uint8_t* tbs_start = cert.p;
  const uint8_t *tb, *te;
  if (read_tl(cert, &tb, &te) != 0x30) return c;
  c.tbs.assign(tbs_start, te);
  Reader tbs = enter(tb, te);

  const uint8_t *xb, *xe;
  const uint8_t* save = tbs.p;
  uint8_t t = read_tl(tbs, &xb, &xe);
  if (t == 0xA0) {              // [0] version
    save = tbs.p;
    t = read_tl(tbs, &xb, &xe);
  }
  if (t != 0x02) { tbs.p = save; return c; }  // serialNumber INTEGER
  read_tl(tbs, &xb, &xe);                      // signature AlgorithmIdentifier
  // issuer Name
  const uint8_t* issuer_start = tbs.p;
  if (read_tl(tbs, &xb, &xe) != 0x30) return c;
  c.issuer_der.assign(issuer_start, xe);
  // validity
  const uint8_t *valb, *vale;
  if (read_tl(tbs, &valb, &vale) != 0x30) return c;
  {
    Reader v = enter(valb, vale);
    const uint8_t *nb, *ne;
    uint8_t nt = read_tl(v, &nb, &ne);
    c.not_before = parse_time(nt, nb, ne);
    nt = read_tl(v, &nb, &ne);
    c.not_after = parse_time(nt, nb, ne);
  }
  // subject Name
  const uint8_t* subject_start = tbs.p;
  if (read_tl(tbs, &xb, &xe) != 0x30) return c;
  c.subject_der.assign(subject_start, xe);
  // subjectPublicKeyInfo
  const uint8_t *spkib, *spkie;
  if (read_tl(tbs, &spkib, &spkie) != 0x30) return c;
  parse_spki(spkib, spkie, &c);
  // optional [1] issuerUID, [2] subjectUID, [3] extensions
  while (tbs.ok && tbs.p < tbs.end) {
    const uint8_t *eb, *ee;
    uint8_t et = read_tl(tbs, &eb, &ee);
    if (et == 0xA3) {  // [3] extensions EXPLICIT
      Reader ex = enter(eb, ee);
      const uint8_t *sb, *se;
      if (read_tl(ex, &sb, &se) == 0x30) parse_extensions(sb, se, &c);
      break;
    }
  }

  // signatureAlgorithm + signatureValue (siblings of tbsCertificate)
  const uint8_t *sab, *sae;
  if (read_tl(cert, &sab, &sae) != 0x30) return c;
  c.sig_alg = parse_sig_alg(sab, sae, &c.pss_hash_len);
  const uint8_t *sigb, *sige;
  if (read_tl(cert, &sigb, &sige) != 0x03) return c;  // BIT STRING
  if (sigb < sige && *sigb == 0x00)
    c.signature.assign(sigb + 1, sige);
  else
    c.signature.assign(sigb, sige);

  c.parsed = true;
  return c;
}

bool verify_signature(const Certificate& child, const Certificate& issuer) {
  if (!child.parsed || !issuer.parsed) return false;
  switch (child.sig_alg) {
    case SigAlg::RsaPkcs1Sha1:
    case SigAlg::RsaPkcs1Sha256:
    case SigAlg::RsaPkcs1Sha384:
    case SigAlg::RsaPkcs1Sha512: {
      if (issuer.key_type != KeyType::Rsa || !issuer.rsa_pub.valid)
        return false;
      size_t hlen = child.sig_alg == SigAlg::RsaPkcs1Sha1   ? 20
                    : child.sig_alg == SigAlg::RsaPkcs1Sha256 ? 32
                    : child.sig_alg == SigAlg::RsaPkcs1Sha384 ? 48
                                                              : 64;
      Bytes h = hash_len(child.tbs.data(), child.tbs.size(), hlen);
      const uint8_t* pfx = hlen == 20   ? kDiSha1
                           : hlen == 32 ? kDiSha256
                           : hlen == 48 ? kDiSha384
                                        : kDiSha512;
      size_t pfx_len = hlen == 20   ? sizeof(kDiSha1)
                       : hlen == 32 ? sizeof(kDiSha256)
                       : hlen == 48 ? sizeof(kDiSha384)
                                    : sizeof(kDiSha512);
      return rsa_pkcs1_verify(issuer.rsa_pub, child.signature, h, pfx, pfx_len);
    }
    case SigAlg::RsaPss: {
      if (issuer.key_type != KeyType::Rsa || !issuer.rsa_pub.valid)
        return false;
      Bytes h = hash_len(child.tbs.data(), child.tbs.size(),
                         child.pss_hash_len);
      return crypto::rsa_verify_pss(issuer.rsa_pub, child.signature.data(),
                                    child.signature.size(), h.data(), h.size());
    }
    case SigAlg::EcdsaSha1:
    case SigAlg::EcdsaSha256:
    case SigAlg::EcdsaSha384:
    case SigAlg::EcdsaSha512: {
      if (issuer.key_type != KeyType::Ec || !issuer.ec_curve) return false;
      size_t hlen = child.sig_alg == SigAlg::EcdsaSha1     ? 20
                    : child.sig_alg == SigAlg::EcdsaSha256 ? 32
                    : child.sig_alg == SigAlg::EcdsaSha384 ? 48
                                                           : 64;
      Bytes h = hash_len(child.tbs.data(), child.tbs.size(), hlen);
      Bytes r, s;
      if (!parse_ecdsa_sig(child.signature.data(), child.signature.size(), &r,
                           &s))
        return false;
      return crypto::ecdsa_verify(*issuer.ec_curve, issuer.ec_point.data(),
                                  issuer.ec_point.size(), h.data(), h.size(),
                                  r.data(), r.size(), s.data(), s.size());
    }
    default:
      return false;
  }
}

bool verify_tls13_signature(const Certificate& leaf, uint16_t scheme,
                            const uint8_t* msg, size_t msg_len,
                            const uint8_t* sig, size_t sig_len) {
  if (!leaf.parsed) return false;
  switch (scheme) {
    case 0x0804:  // rsa_pss_rsae_sha256
    case 0x0809:  // rsa_pss_pss_sha256
    case 0x0805:  // rsa_pss_rsae_sha384
    case 0x080a:  // rsa_pss_pss_sha384
    case 0x0806:  // rsa_pss_rsae_sha512
    case 0x080b:  // rsa_pss_pss_sha512
    {
      if (leaf.key_type != KeyType::Rsa || !leaf.rsa_pub.valid) return false;
      size_t hlen = (scheme == 0x0804 || scheme == 0x0809)   ? 32
                    : (scheme == 0x0805 || scheme == 0x080a) ? 48
                                                             : 64;
      Bytes h = hash_len(msg, msg_len, hlen);
      return crypto::rsa_verify_pss(leaf.rsa_pub, sig, sig_len, h.data(),
                                    h.size());
    }
    case 0x0403:    // ecdsa_secp256r1_sha256
    case 0x0503: {  // ecdsa_secp384r1_sha384
      if (leaf.key_type != KeyType::Ec || !leaf.ec_curve) return false;
      size_t hlen = scheme == 0x0403 ? 32 : 48;
      const crypto::EcCurve& want =
          scheme == 0x0403 ? crypto::curve_p256() : crypto::curve_p384();
      if (leaf.ec_curve != &want) return false;  // cert curve must match scheme
      Bytes h = hash_len(msg, msg_len, hlen);
      Bytes r, s;
      if (!parse_ecdsa_sig(sig, sig_len, &r, &s)) return false;
      return crypto::ecdsa_verify(*leaf.ec_curve, leaf.ec_point.data(),
                                  leaf.ec_point.size(), h.data(), h.size(),
                                  r.data(), r.size(), s.data(), s.size());
    }
    default:
      return false;  // ed25519/ed448, secp521r1, legacy pkcs1 unsupported
  }
}

TrustStore load_trust_store(const std::string& path) {
  TrustStore store;
  std::string chosen = path;
  if (chosen.empty()) {
    for (int i = 0; kBundlePaths[i]; ++i) {
      FILE* f = std::fopen(kBundlePaths[i], "rb");
      if (f) { std::fclose(f); chosen = kBundlePaths[i]; break; }
    }
  }
  if (chosen.empty()) return store;
  FILE* f = std::fopen(chosen.c_str(), "rb");
  if (!f) return store;
  std::string pem;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) pem.append(buf, n);
  std::fclose(f);
  for (const auto& der : cms::pem_to_certs(pem)) {
    Certificate c = parse(der.data(), der.size());
    if (c.parsed) store.roots.push_back(std::move(c));
  }
  store.loaded = !store.roots.empty();
  return store;
}

VerifyResult verify_chain(const std::vector<Bytes>& der_chain,
                          const TrustStore& store, const std::string& hostname,
                          int64_t now_epoch) {
  VerifyResult res;
  if (der_chain.empty()) { res.error = "empty certificate chain"; return res; }

  std::vector<Certificate> chain;
  for (const auto& d : der_chain) {
    Certificate c = parse(d.data(), d.size());
    if (!c.parsed) { res.error = "failed to parse a certificate"; return res; }
    chain.push_back(std::move(c));
  }
  const Certificate& leaf = chain[0];
  res.subject_cn = leaf.subject_cn();

  // Hostname (SAN, with CN fallback).
  if (!hostname.empty()) {
    bool ok = false;
    for (const auto& name : leaf.san_dns)
      if (host_match(hostname, name)) { ok = true; break; }
    if (!ok && leaf.san_dns.empty()) ok = host_match(hostname, leaf.subject_cn());
    if (!ok) { res.error = "hostname mismatch for " + hostname; return res; }
  }

  // Walk from the leaf to a trusted root, verifying each signature + validity.
  const Certificate* cur = &leaf;
  for (int depth = 0; depth < 16; ++depth) {
    // Validity. An unparseable date (not_before/not_after == 0) is treated as a
    // failure rather than "no constraint" (fail closed).
    if (now_epoch && (cur->not_before == 0 || cur->not_after == 0 ||
                      now_epoch < cur->not_before ||
                      now_epoch > cur->not_after)) {
      res.error = "certificate expired or not yet valid";
      return res;
    }
    // Trust anchor: a CA root whose subject is this cert's issuer.
    for (const auto& root : store.roots) {
      if (!root.is_ca) continue;  // issuer must be a CA (basicConstraints)
      if (root.subject_der == cur->issuer_der && verify_signature(*cur, root)) {
        if (now_epoch && root.not_after && now_epoch > root.not_after) continue;
        res.ok = true;
        return res;
      }
    }
    // Otherwise find a CA intermediate in the presented chain. An end-entity
    // (CA:FALSE) cert must never be accepted as an issuer (basicConstraints
    // bypass / certificate forgery).
    const Certificate* next = nullptr;
    for (const auto& cand : chain) {
      if (&cand == cur) continue;
      if (!cand.is_ca) continue;
      if (cand.subject_der == cur->issuer_der && verify_signature(*cur, cand)) {
        next = &cand;
        break;
      }
    }
    if (!next) {
      res.error = "unable to build chain to a trusted root";
      return res;
    }
    cur = next;
  }
  res.error = "certificate chain too long";
  return res;
}

}  // namespace x509
}  // namespace nanopdf
