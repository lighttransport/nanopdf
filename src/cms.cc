// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "cms.hh"

#include <cstring>

#include "asn1-der.hh"
#include "crypto.hh"

namespace nanopdf {
namespace cms {

using namespace nanopdf::asn1;

namespace {

// OIDs.
const char* kOidSignedData = "1.2.840.113549.1.7.2";
const char* kOidData = "1.2.840.113549.1.7.1";
const char* kOidSha256 = "2.16.840.1.101.3.4.2.1";
const char* kOidRsaEncryption = "1.2.840.113549.1.1.1";
const char* kOidContentType = "1.2.840.113549.1.9.3";
const char* kOidMessageDigest = "1.2.840.113549.1.9.4";
const char* kOidSigningTime = "1.2.840.113549.1.9.5";

// ---- minimal DER reader (for the X.509 fields we need) ---------------------

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
};

// Read tag + length, returning the value range [vbeg,vend) and the tag. On
// success @r.p is left just past the value (so siblings can be read).
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

// Enter a constructed value (return a Reader over its contents).
Reader enter(const uint8_t* vbeg, const uint8_t* vend) {
  return Reader{vbeg, vend, true};
}

}  // namespace

CertInfo parse_certificate(const Bytes& cert) {
  CertInfo info;
  Reader top{cert.data(), cert.data() + cert.size(), true};
  const uint8_t *vb, *ve;
  if (read_tl(top, &vb, &ve) != TAG_SEQUENCE) return info;  // Certificate
  Reader c = enter(vb, ve);
  // tbsCertificate
  if (read_tl(c, &vb, &ve) != TAG_SEQUENCE) return info;
  Reader tbs = enter(vb, ve);
  // optional [0] version
  const uint8_t* save = tbs.p;
  const uint8_t *tvb, *tve;
  uint8_t t = read_tl(tbs, &tvb, &tve);
  if (t == 0xA0) {
    // version present; next is serialNumber
    t = read_tl(tbs, &tvb, &tve);
  }
  if (!tbs.ok || t != TAG_INTEGER) { tbs = enter(save, ve); return info; }
  info.serial_be.assign(tvb, tve);  // serialNumber INTEGER body
  // signature AlgorithmIdentifier (SEQUENCE) — skip
  if (read_tl(tbs, &tvb, &tve) != TAG_SEQUENCE) return info;
  // issuer Name (SEQUENCE) — capture full DER (tag..value)
  const uint8_t* issuer_start = tbs.p;
  if (read_tl(tbs, &tvb, &tve) != TAG_SEQUENCE) return info;
  info.issuer_der.assign(issuer_start, tve);
  info.valid = tbs.ok;
  return info;
}

std::vector<Bytes> pem_to_certs(const std::string& pem) {
  static const char* kBegin = "-----BEGIN CERTIFICATE-----";
  static const char* kEnd = "-----END CERTIFICATE-----";
  int dtab[256];
  for (int i = 0; i < 256; ++i) dtab[i] = -1;
  const char* B64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (int i = 0; i < 64; ++i) dtab[(unsigned char)B64[i]] = i;

  std::vector<Bytes> out;
  size_t pos = 0;
  while ((pos = pem.find(kBegin, pos)) != std::string::npos) {
    size_t bstart = pos + std::strlen(kBegin);
    size_t bend = pem.find(kEnd, bstart);
    if (bend == std::string::npos) break;
    Bytes der;
    int val = 0, bits = 0;
    for (size_t i = bstart; i < bend; ++i) {
      int dv = dtab[(unsigned char)pem[i]];
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

// ---- X.509 public key + subject CN -----------------------------------------

crypto::RsaPublicKey extract_rsa_public_key(const Bytes& cert) {
  crypto::RsaPublicKey pk;
  Reader top{cert.data(), cert.data() + cert.size(), true};
  const uint8_t *vb, *ve;
  if (read_tl(top, &vb, &ve) != TAG_SEQUENCE) return pk;  // Certificate
  Reader c = enter(vb, ve);
  if (read_tl(c, &vb, &ve) != TAG_SEQUENCE) return pk;  // TBSCertificate
  Reader tbs = enter(vb, ve);
  const uint8_t *tvb, *tve;
  const uint8_t* save = tbs.p;
  if (read_tl(tbs, &tvb, &tve) != 0xA0) tbs.p = save;  // optional [0] version
  read_tl(tbs, &tvb, &tve);  // serialNumber
  read_tl(tbs, &tvb, &tve);  // signature AlgId
  read_tl(tbs, &tvb, &tve);  // issuer
  read_tl(tbs, &tvb, &tve);  // validity
  read_tl(tbs, &tvb, &tve);  // subject
  if (read_tl(tbs, &tvb, &tve) != TAG_SEQUENCE) return pk;  // SPKI
  Reader spki = enter(tvb, tve);
  read_tl(spki, &tvb, &tve);                            // algorithm
  if (read_tl(spki, &tvb, &tve) != 0x03) return pk;     // subjectPublicKey BIT STRING
  if (tvb >= tve || *tvb != 0x00) return pk;            // unused-bits byte
  Reader rsaw{tvb + 1, tve, true};
  if (read_tl(rsaw, &tvb, &tve) != TAG_SEQUENCE) return pk;  // RSAPublicKey
  Reader rsa = enter(tvb, tve);
  const uint8_t *nb, *ne2, *eb, *ee;
  if (read_tl(rsa, &nb, &ne2) != TAG_INTEGER) return pk;  // modulus
  if (read_tl(rsa, &eb, &ee) != TAG_INTEGER) return pk;   // exponent
  pk.n = crypto::BigInt::from_bytes(nb, ne2 - nb);
  pk.e = crypto::BigInt::from_bytes(eb, ee - eb);
  pk.modulus_bytes = (pk.n.bit_length() + 7) / 8;
  pk.valid = !pk.n.is_zero();
  return pk;
}

namespace {
// commonName OID value bytes (2.5.4.3).
const uint8_t kCnOid[] = {0x55, 0x04, 0x03};

std::string scan_cn(const uint8_t* b, const uint8_t* e) {
  for (const uint8_t* p = b; p + 7 < e; ++p) {
    if (p[0] == 0x06 && p[1] == 0x03 && std::memcmp(p + 2, kCnOid, 3) == 0) {
      const uint8_t* q = p + 5;
      uint8_t tag = q[0];
      size_t len = q[1];
      if ((tag == 0x0c || tag == 0x13 || tag == 0x16) && q + 2 + len <= e)
        return std::string(reinterpret_cast<const char*>(q + 2), len);
    }
  }
  return "";
}
}  // namespace

std::string subject_common_name(const Bytes& cert) {
  // Navigate to the subject Name and scan it for commonName.
  Reader top{cert.data(), cert.data() + cert.size(), true};
  const uint8_t *vb, *ve;
  if (read_tl(top, &vb, &ve) != TAG_SEQUENCE) return "";
  Reader c = enter(vb, ve);
  if (read_tl(c, &vb, &ve) != TAG_SEQUENCE) return "";
  Reader tbs = enter(vb, ve);
  const uint8_t *tvb, *tve;
  const uint8_t* save = tbs.p;
  if (read_tl(tbs, &tvb, &tve) != 0xA0) tbs.p = save;  // [0] version
  read_tl(tbs, &tvb, &tve);  // serial
  read_tl(tbs, &tvb, &tve);  // sigAlg
  read_tl(tbs, &tvb, &tve);  // issuer
  read_tl(tbs, &tvb, &tve);  // validity
  if (read_tl(tbs, &tvb, &tve) != TAG_SEQUENCE) return "";  // subject
  return scan_cn(tvb, tve);
}

// IssuerAndSerialNumber ::= SEQUENCE { issuer Name, serialNumber INTEGER }
static Bytes issuer_and_serial(const CertInfo& ci) {
  return der_sequence({der_raw(ci.issuer_der), der_integer(ci.serial_be)});
}

// Attribute ::= SEQUENCE { attrType OID, attrValues SET OF value }
static Bytes attribute(const char* oid, const Bytes& value) {
  return der_sequence({der_oid(oid), der_set_raw({value})});
}

Bytes build_signed_data(const Bytes& content, const Bytes& signer_cert,
                        const std::vector<Bytes>& chain,
                        const crypto::RsaPrivateKey& key,
                        const std::string& signing_time_utc,
                        const TsaCallback& tsa) {
  CertInfo ci = parse_certificate(signer_cert);
  if (!ci.valid || !key.valid) return {};

  // messageDigest = SHA-256(content)
  uint8_t md[32];
  crypto::SHA256::hash(content.data(), content.size(), md);
  Bytes md_bytes(md, md + 32);

  // Signed attributes (DER SET OF Attribute). Order in the SignerInfo uses an
  // IMPLICIT [0] tag; for the signature it is re-tagged as a SET (0x31). DER
  // requires the SET OF members sorted by encoding.
  std::vector<Bytes> attrs = {
      attribute(kOidContentType, der_oid(kOidData)),
      attribute(kOidSigningTime, der_utctime(signing_time_utc)),
      attribute(kOidMessageDigest, der_octet_string(md_bytes)),
  };
  Bytes signed_attrs_set = der_set(attrs);  // 0x31 ... (sorted) — to be signed

  // Sign SHA-256(signed_attrs_set) with RSA PKCS#1 v1.5.
  uint8_t sa_hash[32];
  crypto::SHA256::hash(signed_attrs_set.data(), signed_attrs_set.size(),
                       sa_hash);
  Bytes signature = crypto::rsa_sign_sha256(key, sa_hash);
  if (signature.empty()) return {};

  // The same attributes, IMPLICIT [0] tagged for the SignerInfo. Re-tag the
  // sorted SET body with 0xA0.
  Bytes signed_attrs_context;
  {
    // signed_attrs_set is TLV with tag 0x31; replace tag with 0xA0, keep L+V.
    signed_attrs_context = signed_attrs_set;
    signed_attrs_context[0] = 0xA0;
  }

  // Optional RFC 3161 signature timestamp -> id-aa-timeStampToken unsigned attr.
  std::vector<Bytes> signer_items = {
      der_integer_u32(1),                     // version
      issuer_and_serial(ci),                  // sid
      der_algorithm_id(kOidSha256),           // digestAlgorithm
      signed_attrs_context,                   // [0] signedAttrs
      der_algorithm_id(kOidRsaEncryption),    // signatureAlgorithm
      der_octet_string(signature),            // signature
  };
  if (tsa) {
    Bytes token = tsa(signature);
    if (!token.empty()) {
      // id-aa-timeStampToken = 1.2.840.113549.1.9.16.2.14
      Bytes attr = der_sequence(
          {der_oid("1.2.840.113549.1.9.16.2.14"), der_set_raw({token})});
      signer_items.push_back(der_context(1, true, attr));  // [1] unsignedAttrs
    }
  }
  Bytes signer_info = der_sequence(signer_items);

  // certificates [0] IMPLICIT (signer first, then chain).
  Bytes certs_blob = signer_cert;
  for (const auto& cder : chain)
    certs_blob.insert(certs_blob.end(), cder.begin(), cder.end());
  Bytes certificates = der_context(0, true, certs_blob);

  // encapContentInfo: detached -> just the content type, no eContent.
  Bytes encap = der_sequence({der_oid(kOidData)});

  Bytes signed_data = der_sequence({
      der_integer_u32(1),                              // version
      der_set({der_algorithm_id(kOidSha256)}),         // digestAlgorithms
      encap,                                           // encapContentInfo
      certificates,                                    // [0] certificates
      der_set_raw({signer_info}),                      // signerInfos
  });

  // ContentInfo: SEQUENCE { signedData OID, [0] EXPLICIT SignedData }
  return der_sequence({der_oid(kOidSignedData),
                       der_context(0, true, signed_data)});
}

// ---- verification ----------------------------------------------------------

namespace {

// OID value bytes (the content of der_oid, minus tag+length).
Bytes oid_val(const char* dotted) {
  Bytes full = der_oid(dotted);
  return Bytes(full.begin() + 2, full.end());  // OIDs here have 1-byte length
}

// Within a SET-of-Attribute content range [b,e), find the Attribute whose type
// OID equals @oid and return the FULL TLV of its first value.
bool find_attr_value(const uint8_t* b, const uint8_t* e, const Bytes& oid,
                     const uint8_t** vbeg, const uint8_t** vend) {
  Reader r{b, e, true};
  const uint8_t *ab, *ae;
  while (r.p < r.end && r.ok) {
    if (read_tl(r, &ab, &ae) != TAG_SEQUENCE) break;
    Reader a = enter(ab, ae);
    const uint8_t *ob, *oe;
    if (read_tl(a, &ob, &oe) != TAG_OID) continue;
    if ((size_t)(oe - ob) == oid.size() &&
        std::memcmp(ob, oid.data(), oid.size()) == 0) {
      const uint8_t *sb, *se;
      if (read_tl(a, &sb, &se) != TAG_SET) return false;
      Reader sv = enter(sb, se);
      const uint8_t* start = sv.p;
      const uint8_t *xb, *xe;
      if (read_tl(sv, &xb, &xe) == 0) return false;
      *vbeg = start;
      *vend = sv.p;
      return true;
    }
  }
  return false;
}

const uint8_t kSha256DiPrefix[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09,
                                   0x60, 0x86, 0x48, 0x01, 0x65, 0x03,
                                   0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

// Extract genTime + TSA commonName from an RFC 3161 TimeStampToken (a CMS
// ContentInfo whose eContent is a TSTInfo). Best-effort.
void parse_tst(const Bytes& token, std::string* gen_time, std::string* tsa) {
  Reader top{token.data(), token.data() + token.size(), true};
  const uint8_t *vb, *ve;
  if (read_tl(top, &vb, &ve) != TAG_SEQUENCE) return;  // ContentInfo
  Reader ci = enter(vb, ve);
  const uint8_t *ob, *oe;
  if (read_tl(ci, &ob, &oe) != TAG_OID) return;        // contentType
  if (read_tl(ci, &vb, &ve) != 0xA0) return;           // [0] SignedData
  Reader c0 = enter(vb, ve);
  if (read_tl(c0, &vb, &ve) != TAG_SEQUENCE) return;   // SignedData
  Reader sd = enter(vb, ve);
  read_tl(sd, &vb, &ve);  // version
  read_tl(sd, &vb, &ve);  // digestAlgorithms
  // encapContentInfo SEQ { eContentType OID, [0] EXPLICIT OCTET STRING TSTInfo }
  if (read_tl(sd, &vb, &ve) != TAG_SEQUENCE) return;
  Reader eci = enter(vb, ve);
  read_tl(eci, &ob, &oe);                  // eContentType
  if (read_tl(eci, &vb, &ve) != 0xA0) return;  // [0]
  Reader e0 = enter(vb, ve);
  if (read_tl(e0, &vb, &ve) != TAG_OCTET_STRING) return;  // TSTInfo bytes
  Reader ti{vb, ve, true};
  if (read_tl(ti, &vb, &ve) != TAG_SEQUENCE) return;  // TSTInfo
  Reader t = enter(vb, ve);
  read_tl(t, &vb, &ve);  // version
  read_tl(t, &vb, &ve);  // policy OID
  read_tl(t, &vb, &ve);  // messageImprint
  read_tl(t, &vb, &ve);  // serialNumber
  const uint8_t *gb, *ge;
  if (read_tl(t, &gb, &ge) == TAG_GENERALIZEDTIME)
    *gen_time = std::string(reinterpret_cast<const char*>(gb), ge - gb);
  // TSA name: first certificate's subject CN (scan the [0] certs in sd).
  while (sd.p < sd.end && sd.ok) {
    const uint8_t* save = sd.p;
    uint8_t tag = read_tl(sd, &vb, &ve);
    if (tag == 0xA0) {
      Reader cr = enter(vb, ve);
      const uint8_t* cstart = cr.p;
      const uint8_t *cb, *ce2;
      if (read_tl(cr, &cb, &ce2) == TAG_SEQUENCE)
        *tsa = subject_common_name(Bytes(cstart, cr.p));
      break;
    } else if (tag == 0x31 || tag == 0) {
      break;
    }
    (void)save;
  }
}

}  // namespace

std::vector<Bytes> extract_certificates(const Bytes& cms) {
  std::vector<Bytes> certs;
  Reader top{cms.data(), cms.data() + cms.size(), true};
  const uint8_t *vb, *ve, *ob, *oe;
  if (read_tl(top, &vb, &ve) != TAG_SEQUENCE) return certs;
  Reader ci = enter(vb, ve);
  if (read_tl(ci, &ob, &oe) != TAG_OID) return certs;
  if (read_tl(ci, &vb, &ve) != 0xA0) return certs;
  Reader c0 = enter(vb, ve);
  if (read_tl(c0, &vb, &ve) != TAG_SEQUENCE) return certs;
  Reader sd = enter(vb, ve);
  read_tl(sd, &vb, &ve);  // version
  read_tl(sd, &vb, &ve);  // digestAlgorithms
  read_tl(sd, &vb, &ve);  // encapContentInfo
  while (sd.p < sd.end && sd.ok) {
    uint8_t t = read_tl(sd, &vb, &ve);
    if (t == 0xA0) {  // certificates
      Reader cr{vb, ve, true};
      const uint8_t *cb, *ce2;
      while (cr.p < cr.end && cr.ok) {
        const uint8_t* cstart = cr.p;
        if (read_tl(cr, &cb, &ce2) != TAG_SEQUENCE) break;
        certs.emplace_back(cstart, cr.p);
      }
      break;
    } else if (t == TAG_SET || t == 0) {
      break;  // signerInfos (no certs) or end
    }
    // [1] crls and anything else: ignored.
  }
  return certs;
}

VerifyInfo verify_signed_data(const Bytes& cms, const Bytes& content) {
  VerifyInfo vi;
  Reader top{cms.data(), cms.data() + cms.size(), true};
  const uint8_t *vb, *ve;
  if (read_tl(top, &vb, &ve) != TAG_SEQUENCE) { vi.error = "not DER SEQUENCE"; return vi; }
  Reader ci = enter(vb, ve);
  const uint8_t *ob, *oe;
  if (read_tl(ci, &ob, &oe) != TAG_OID) { vi.error = "no contentType"; return vi; }
  if (read_tl(ci, &vb, &ve) != 0xA0) { vi.error = "no content [0]"; return vi; }
  Reader c0 = enter(vb, ve);
  if (read_tl(c0, &vb, &ve) != TAG_SEQUENCE) { vi.error = "no SignedData"; return vi; }
  Reader sd = enter(vb, ve);
  read_tl(sd, &vb, &ve);  // version
  read_tl(sd, &vb, &ve);  // digestAlgorithms
  read_tl(sd, &vb, &ve);  // encapContentInfo

  std::vector<Bytes> certs;
  const uint8_t *si_b = nullptr, *si_e = nullptr;
  while (sd.p < sd.end && sd.ok) {
    uint8_t t = read_tl(sd, &vb, &ve);
    if (t == 0xA0) {  // certificates
      Reader cr{vb, ve, true};
      const uint8_t *cb, *ce2;
      while (cr.p < cr.end && cr.ok) {
        const uint8_t* cstart = cr.p;
        if (read_tl(cr, &cb, &ce2) != TAG_SEQUENCE) break;
        certs.emplace_back(cstart, cr.p);
      }
    } else if (t == TAG_SET) {  // signerInfos
      si_b = vb; si_e = ve; break;
    } else if (t == 0) {
      break;
    }
    // [1] crls and anything else: ignored.
  }
  if (!si_b) { vi.error = "no signerInfos"; return vi; }
  vi.parsed = true;

  Reader sis{si_b, si_e, true};
  if (read_tl(sis, &vb, &ve) != TAG_SEQUENCE) { vi.error = "bad SignerInfo"; return vi; }
  Reader si = enter(vb, ve);
  read_tl(si, &vb, &ve);  // version
  const uint8_t *sid_b, *sid_e;
  read_tl(si, &sid_b, &sid_e);  // sid (IssuerAndSerialNumber)
  read_tl(si, &vb, &ve);        // digestAlgorithm
  vi.digest_algorithm = "SHA-256";

  const uint8_t* sa_start = si.p;
  const uint8_t *sa_b, *sa_e;
  if (read_tl(si, &sa_b, &sa_e) != 0xA0) { vi.error = "no signedAttrs"; return vi; }
  const uint8_t* sa_end = si.p;
  read_tl(si, &vb, &ve);  // signatureAlgorithm
  const uint8_t *sig_b, *sig_e;
  if (read_tl(si, &sig_b, &sig_e) != TAG_OCTET_STRING) { vi.error = "no signature"; return vi; }
  Bytes signature(sig_b, sig_e);
  const uint8_t *ua_b = nullptr, *ua_e = nullptr;
  if (si.p < si.end) {
    const uint8_t* save = si.p;
    if (read_tl(si, &vb, &ve) == 0xA1) { ua_b = vb; ua_e = ve; } else si.p = save;
  }

  // messageDigest attr == SHA-256(content)
  const uint8_t *mv_b, *mv_e;
  if (find_attr_value(sa_b, sa_e, oid_val("1.2.840.113549.1.9.4"), &mv_b, &mv_e)) {
    Reader mr{mv_b, mv_e, true};
    const uint8_t *xb, *xe;
    if (read_tl(mr, &xb, &xe) == TAG_OCTET_STRING && (xe - xb) == 32) {
      uint8_t h[32];
      crypto::SHA256::hash(content.data(), content.size(), h);
      vi.digest_valid = std::memcmp(h, xb, 32) == 0;
    }
  }

  // signingTime
  const uint8_t *tv_b, *tv_e;
  if (find_attr_value(sa_b, sa_e, oid_val("1.2.840.113549.1.9.5"), &tv_b, &tv_e)) {
    Reader sr{tv_b, tv_e, true};
    const uint8_t *xb, *xe;
    uint8_t tag = read_tl(sr, &xb, &xe);
    if (tag == TAG_UTCTIME || tag == TAG_GENERALIZEDTIME)
      vi.signing_time = std::string(reinterpret_cast<const char*>(xb), xe - xb);
  }

  // Pick the signer certificate (match by IssuerAndSerialNumber; else first).
  Bytes signer_cert;
  {
    CertInfo want;  // parse sid: SEQUENCE { issuer, serial }
    Reader sidr{sid_b, sid_e, true};
    const uint8_t *ib, *ie;
    const uint8_t* istart = sidr.p;
    if (read_tl(sidr, &ib, &ie) == TAG_SEQUENCE) {
      Reader s2 = enter(ib, ie);
      const uint8_t* iss_start = s2.p;
      const uint8_t *xb, *xe;
      if (read_tl(s2, &xb, &xe) == TAG_SEQUENCE) want.issuer_der.assign(iss_start, s2.p);
      if (read_tl(s2, &xb, &xe) == TAG_INTEGER) want.serial_be.assign(xb, xe);
    }
    (void)istart;
    for (const auto& c : certs) {
      CertInfo got = parse_certificate(c);
      if (got.valid && got.issuer_der == want.issuer_der &&
          got.serial_be == want.serial_be) { signer_cert = c; break; }
    }
    if (signer_cert.empty() && !certs.empty()) signer_cert = certs[0];
  }
  vi.signer_cn = subject_common_name(signer_cert);

  // RSA-verify the signature over the signedAttrs re-encoded as a SET.
  crypto::RsaPublicKey pub = extract_rsa_public_key(signer_cert);
  if (pub.valid) {
    Bytes sa(sa_start, sa_end);
    if (!sa.empty()) sa[0] = 0x31;  // [0] IMPLICIT -> SET, for the digest
    uint8_t h[32];
    crypto::SHA256::hash(sa.data(), sa.size(), h);
    Bytes di(std::begin(kSha256DiPrefix), std::end(kSha256DiPrefix));
    di.insert(di.end(), h, h + 32);
    vi.signature_valid = crypto::rsa_verify_pkcs1v15(
        pub, signature.data(), signature.size(), di.data(), di.size());
  }

  // RFC 3161 signature timestamp (unsigned attribute).
  if (ua_b) {
    const uint8_t *zb, *ze;
    if (find_attr_value(ua_b, ua_e, oid_val("1.2.840.113549.1.9.16.2.14"), &zb, &ze)) {
      vi.has_timestamp = true;
      parse_tst(Bytes(zb, ze), &vi.timestamp_time, &vi.timestamp_authority);
    }
  }
  return vi;
}

}  // namespace cms
}  // namespace nanopdf
