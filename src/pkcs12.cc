// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "pkcs12.hh"

#include <cstring>
#include <utility>

#include "cms.hh"

namespace nanopdf {
namespace pkcs12 {

namespace {

struct Cur {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
};

uint8_t read_tl(Cur& r, const uint8_t** vb, const uint8_t** ve) {
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
  *vb = r.p;
  *ve = r.p + len;
  r.p += len;
  return tag;
}

Cur sub(const uint8_t* b, const uint8_t* e) { return Cur{b, e, true}; }

bool oid_is(const uint8_t* b, const uint8_t* e, const uint8_t* want, size_t n) {
  return (size_t)(e - b) == n && std::memcmp(b, want, n) == 0;
}

const uint8_t OID_DATA[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x01};
const uint8_t OID_ENCRYPTED[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x06};
const uint8_t OID_KEYBAG[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x01};
const uint8_t OID_SHROUDED[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x02};
const uint8_t OID_CERTBAG[] = {0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x03};

// Parse a SafeContents (SEQUENCE OF SafeBag), collecting certs/key into @out.
void parse_safe_contents(const uint8_t* b, const uint8_t* e,
                         const std::string& pw, Bundle* out) {
  Cur sc{b, e, true};
  const uint8_t *vb, *ve;
  if (read_tl(sc, &vb, &ve) != 0x30) return;  // SafeContents SEQUENCE
  Cur bags = sub(vb, ve);
  while (bags.p < bags.end && bags.ok) {
    const uint8_t *bb, *be;
    if (read_tl(bags, &bb, &be) != 0x30) break;  // SafeBag SEQUENCE
    Cur bag = sub(bb, be);
    const uint8_t *ob, *oe;
    if (read_tl(bag, &ob, &oe) != 0x06) continue;  // bagId OID
    const uint8_t *cb, *ce;
    if (read_tl(bag, &cb, &ce) != 0xA0) continue;  // bagValue [0] EXPLICIT
    Cur val = sub(cb, ce);

    if (oid_is(ob, oe, OID_CERTBAG, sizeof(OID_CERTBAG))) {
      // CertBag ::= SEQUENCE { certId OID, certValue [0] EXPLICIT OCTET STRING }
      const uint8_t *xb, *xe;
      if (read_tl(val, &xb, &xe) != 0x30) continue;
      Cur cbag = sub(xb, xe);
      read_tl(cbag, &xb, &xe);                       // certId OID
      if (read_tl(cbag, &xb, &xe) != 0xA0) continue;  // [0]
      Cur cv = sub(xb, xe);
      if (read_tl(cv, &xb, &xe) != 0x04) continue;    // OCTET STRING (cert DER)
      out->certs.emplace_back(xb, xe);
    } else if (oid_is(ob, oe, OID_KEYBAG, sizeof(OID_KEYBAG))) {
      // bagValue = PrivateKeyInfo (PKCS#8, plaintext)
      crypto::RsaPrivateKey k = crypto::rsa_parse_private_key_der(cb, ce - cb);
      if (k.valid) out->key = k;
    } else if (oid_is(ob, oe, OID_SHROUDED, sizeof(OID_SHROUDED))) {
      // bagValue = EncryptedPrivateKeyInfo (PBES2)
      std::vector<uint8_t> pkcs8 =
          crypto::decrypt_pkcs8_pbes2(cb, ce - cb, pw);
      if (!pkcs8.empty()) {
        crypto::RsaPrivateKey k =
            crypto::rsa_parse_private_key_der(pkcs8.data(), pkcs8.size());
        if (k.valid) out->key = k;
      }
    }
  }
}

// Parse one AuthenticatedSafe ContentInfo (data or encryptedData).
void parse_content_info(const uint8_t* b, const uint8_t* e,
                        const std::string& pw, Bundle* out) {
  Cur ci{b, e, true};
  const uint8_t *vb, *ve;
  if (read_tl(ci, &vb, &ve) != 0x30) return;  // ContentInfo SEQUENCE
  Cur c = sub(vb, ve);
  const uint8_t *ob, *oe;
  if (read_tl(c, &ob, &oe) != 0x06) return;  // contentType OID

  if (oid_is(ob, oe, OID_DATA, sizeof(OID_DATA))) {
    if (read_tl(c, &vb, &ve) != 0xA0) return;  // [0] EXPLICIT
    Cur c0 = sub(vb, ve);
    if (read_tl(c0, &vb, &ve) != 0x04) return;  // OCTET STRING SafeContents
    parse_safe_contents(vb, ve, pw, out);
  } else if (oid_is(ob, oe, OID_ENCRYPTED, sizeof(OID_ENCRYPTED))) {
    if (read_tl(c, &vb, &ve) != 0xA0) return;  // [0] EncryptedData
    Cur c0 = sub(vb, ve);
    if (read_tl(c0, &vb, &ve) != 0x30) return;  // EncryptedData SEQUENCE
    Cur ed = sub(vb, ve);
    read_tl(ed, &vb, &ve);  // version
    if (read_tl(ed, &vb, &ve) != 0x30) return;  // EncryptedContentInfo
    Cur eci = sub(vb, ve);
    const uint8_t *xb, *xe;
    read_tl(eci, &xb, &xe);                       // contentType OID (data)
    const uint8_t *ab, *ae;
    if (read_tl(eci, &ab, &ae) != 0x30) return;   // contentEncryptionAlgorithm
    if (read_tl(eci, &xb, &xe) != 0x80) return;   // [0] IMPLICIT encryptedContent
    std::vector<uint8_t> plain =
        crypto::pbes2_decrypt(ab, ae - ab, xb, xe - xb, pw);
    if (!plain.empty())
      parse_safe_contents(plain.data(), plain.data() + plain.size(), pw, out);
  }
}

}  // namespace

Bundle parse(const uint8_t* data, size_t len, const std::string& pw) {
  Bundle out;
  Cur top{data, data + len, true};
  const uint8_t *vb, *ve;
  if (read_tl(top, &vb, &ve) != 0x30) { out.error = "not a PFX SEQUENCE"; return out; }
  Cur pfx = sub(vb, ve);
  read_tl(pfx, &vb, &ve);  // version INTEGER
  // authSafe ContentInfo: SEQUENCE { OID(data), [0] OCTET STRING(AuthSafe) }
  if (read_tl(pfx, &vb, &ve) != 0x30) { out.error = "no authSafe"; return out; }
  Cur as = sub(vb, ve);
  const uint8_t *ob, *oe;
  if (read_tl(as, &ob, &oe) != 0x06) { out.error = "bad authSafe"; return out; }
  if (read_tl(as, &vb, &ve) != 0xA0) { out.error = "no authSafe content"; return out; }
  Cur a0 = sub(vb, ve);
  if (read_tl(a0, &vb, &ve) != 0x04) { out.error = "authSafe not OCTET STRING"; return out; }

  // AuthenticatedSafe ::= SEQUENCE OF ContentInfo
  Cur authsafe = sub(vb, ve);
  if (read_tl(authsafe, &vb, &ve) != 0x30) { out.error = "bad AuthenticatedSafe"; return out; }
  Cur cis = sub(vb, ve);
  while (cis.p < cis.end && cis.ok) {
    const uint8_t *cb, *ce;
    const uint8_t* start = cis.p;
    if (read_tl(cis, &cb, &ce) != 0x30) break;  // ContentInfo SEQUENCE
    parse_content_info(start, cis.p, pw, &out);
  }

  if (!out.key.valid) {
    out.error = out.error.empty() ? "no decryptable private key (wrong "
                                    "password or unsupported PBE)" : out.error;
    return out;
  }
  // Put the signer cert (the one whose public key matches the private key)
  // first, so callers can treat certs[0] as the signer and the rest as chain.
  for (size_t i = 0; i < out.certs.size(); ++i) {
    crypto::RsaPublicKey pk = cms::extract_rsa_public_key(out.certs[i]);
    if (pk.valid && crypto::BigInt::cmp(pk.n, out.key.n) == 0) {
      if (i != 0) std::swap(out.certs[0], out.certs[i]);
      break;
    }
  }
  out.valid = true;
  return out;
}

}  // namespace pkcs12
}  // namespace nanopdf
