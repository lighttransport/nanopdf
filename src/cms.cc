// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "cms.hh"

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
                        const std::string& signing_time_utc) {
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

  // SignerInfo
  Bytes signer_info = der_sequence({
      der_integer_u32(1),                     // version
      issuer_and_serial(ci),                  // sid
      der_algorithm_id(kOidSha256),           // digestAlgorithm
      signed_attrs_context,                   // [0] signedAttrs
      der_algorithm_id(kOidRsaEncryption),    // signatureAlgorithm
      der_octet_string(signature),            // signature
  });

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

}  // namespace cms
}  // namespace nanopdf
