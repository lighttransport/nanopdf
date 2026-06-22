// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// asn1-der — minimal ASN.1 DER encoder (pure C++17), used to build PKCS#7/CMS
// signatures and RFC 3161 timestamp requests without OpenSSL.

#ifndef NANOPDF_ASN1_DER_HH_
#define NANOPDF_ASN1_DER_HH_

#include <cstdint>
#include <string>
#include <vector>

namespace nanopdf {
namespace asn1 {

using Bytes = std::vector<uint8_t>;

// Universal tags.
enum Tag : uint8_t {
  TAG_BOOLEAN = 0x01,
  TAG_INTEGER = 0x02,
  TAG_BIT_STRING = 0x03,
  TAG_OCTET_STRING = 0x04,
  TAG_NULL = 0x05,
  TAG_OID = 0x06,
  TAG_UTF8STRING = 0x0C,
  TAG_PRINTABLESTRING = 0x13,
  TAG_UTCTIME = 0x17,
  TAG_GENERALIZEDTIME = 0x18,
  TAG_SEQUENCE = 0x30,
  TAG_SET = 0x31,
};

// Encode a DER length.
Bytes encode_length(size_t len);

// Wrap @content in a tag+length+value triple.
Bytes tlv(uint8_t tag, const Bytes& content);

// Primitive encoders.
Bytes der_integer(const Bytes& be_unsigned);     // unsigned big-endian magnitude
Bytes der_integer_u32(uint32_t v);
Bytes der_oid(const std::string& dotted);        // "1.2.840.113549.1.1.11"
Bytes der_octet_string(const Bytes& data);
Bytes der_bit_string(const Bytes& data);         // 0 unused bits
Bytes der_null();
Bytes der_bool(bool v);
Bytes der_printable_string(const std::string& s);
Bytes der_utf8_string(const std::string& s);
Bytes der_utctime(const std::string& yymmddhhmmssZ);
Bytes der_generalizedtime(const std::string& yyyymmddhhmmssZ);

// Constructed encoders. @items are already-encoded TLVs, concatenated.
Bytes der_sequence(const std::vector<Bytes>& items);
Bytes der_set(const std::vector<Bytes>& items);  // items are sorted for DER SET OF
Bytes der_set_raw(const std::vector<Bytes>& items);  // no sorting (SET, not SET OF)

// Context-specific tag [n]. @constructed selects 0xA0|n vs 0x80|n.
Bytes der_context(int n, bool constructed, const Bytes& content);

// Pass an already-DER-encoded blob through unchanged (e.g. an embedded cert).
inline Bytes der_raw(const Bytes& b) { return b; }

// AlgorithmIdentifier ::= SEQUENCE { algorithm OID, parameters NULL }
Bytes der_algorithm_id(const std::string& oid, bool null_params = true);

}  // namespace asn1
}  // namespace nanopdf

#endif  // NANOPDF_ASN1_DER_HH_
