// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "rfc3161.hh"

#include "asn1-der.hh"

namespace nanopdf {
namespace rfc3161 {

using namespace nanopdf::asn1;

std::string hash_oid(const std::string& name) {
  if (name == "sha384") return "2.16.840.1.101.3.4.2.2";
  if (name == "sha512") return "2.16.840.1.101.3.4.2.3";
  return "2.16.840.1.101.3.4.2.1";  // sha256
}

Bytes build_request(const Bytes& imprint, const std::string& oid,
                    uint64_t nonce, bool cert_req) {
  // MessageImprint ::= SEQUENCE { hashAlgorithm AlgorithmIdentifier,
  //                               hashedMessage OCTET STRING }
  Bytes message_imprint =
      der_sequence({der_algorithm_id(oid), der_octet_string(imprint)});

  // nonce INTEGER (big-endian, drop leading zeros via der_integer).
  Bytes nonce_be;
  for (int s = 56; s >= 0; s -= 8) {
    uint8_t b = static_cast<uint8_t>((nonce >> s) & 0xFF);
    if (!nonce_be.empty() || b) nonce_be.push_back(b);
  }
  if (nonce_be.empty()) nonce_be.push_back(0);

  // TimeStampReq ::= SEQUENCE { version INTEGER(1), messageImprint,
  //                             nonce INTEGER OPTIONAL, certReq BOOLEAN }
  std::vector<Bytes> items{der_integer_u32(1), message_imprint,
                           der_integer(nonce_be)};
  if (cert_req) items.push_back(der_bool(true));
  return der_sequence(items);
}

namespace {
struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
};
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
}  // namespace

Bytes parse_response(const Bytes& tsr, int* status_out) {
  // TimeStampResp ::= SEQUENCE { status PKIStatusInfo,
  //                              timeStampToken ContentInfo OPTIONAL }
  Reader top{tsr.data(), tsr.data() + tsr.size(), true};
  const uint8_t *vb, *ve;
  if (read_tl(top, &vb, &ve) != TAG_SEQUENCE) return {};
  Reader resp{vb, ve, true};

  // PKIStatusInfo ::= SEQUENCE { status INTEGER, ... }
  const uint8_t *sb, *se;
  if (read_tl(resp, &sb, &se) != TAG_SEQUENCE) return {};
  Reader st{sb, se, true};
  const uint8_t *ib, *ie;
  if (read_tl(st, &ib, &ie) != TAG_INTEGER) return {};
  int status = 0;
  for (const uint8_t* p = ib; p < ie; ++p) status = (status << 8) | *p;
  if (status_out) *status_out = status;
  if (status != 0 && status != 1) return {};  // not granted

  // timeStampToken: the next element is a ContentInfo (SEQUENCE). Return its
  // full DER (tag through value).
  const uint8_t* tok_start = resp.p;
  const uint8_t *tb, *te;
  if (read_tl(resp, &tb, &te) != TAG_SEQUENCE) return {};
  return Bytes(tok_start, te);
}

}  // namespace rfc3161
}  // namespace nanopdf
