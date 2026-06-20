// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// pdf_sign — digital-signature engine for pdfview, built on nanopdf's signing
// framework (PdfWriter incremental update + apply_signature) and OpenSSL's
// PKCS#7/CMS + RFC 3161 timestamp APIs.
//
// The signature is an adbe.pkcs7.detached CMS over the PDF /ByteRange. When a
// timestamp authority (TSA) is configured, an RFC 3161 signature-timestamp is
// embedded as an unsigned attribute (id-aa-timeStampToken), yielding a
// PAdES-T / CAdES-T style signature.

#ifndef PDFVIEW_PDF_SIGN_HH_
#define PDFVIEW_PDF_SIGN_HH_

#include <cstdint>
#include <string>
#include <vector>

namespace pdfview {

// A named RFC 3161 timestamp-authority preset.
struct TsaPreset {
  const char* key;   // short id used on the CLI / MCP (e.g. "digicert")
  const char* name;  // human-readable
  const char* url;   // RFC 3161 endpoint
};

// Built-in TSA presets (DigiCert, GlobalSign, Sectigo, FreeTSA). Returns a
// pointer to a static, NULL-terminated array.
const TsaPreset* tsa_presets();
// Resolve a preset key to its URL, or "" if unknown. "opentimestamps" is
// recognized but handled via a different (non-RFC-3161) path.
std::string tsa_url_for(const std::string& key);

struct SignOptions {
  // Credentials: either a PKCS#12 bundle, or a PEM cert + PEM key.
  std::string p12_path;       // .p12 / .pfx (cert + key + optional chain)
  std::string p12_password;
  std::string cert_pem_path;  // alternative: PEM certificate
  std::string key_pem_path;   // alternative: PEM private key
  std::string key_password;   // password for an encrypted PEM key

  // Signature metadata.
  std::string field_name = "Signature1";
  std::string reason;
  std::string location;
  std::string contact_info;

  // Visible appearance (ignored when visible == false).
  bool visible = false;
  int page = 0;            // 0-based
  double x = 36, y = 36;   // bottom-left, PDF points
  double width = 200, height = 60;

  // RFC 3161 timestamp. Empty url => no timestamp. Use a preset key via
  // tsa_url_for(), or a full URL.
  std::string tsa_url;
  std::string tsa_username;  // optional HTTP basic auth
  std::string tsa_password;
  // Digest used for the timestamp message imprint ("sha256" | "sha384" |
  // "sha512"). The CMS itself always uses SHA-256.
  std::string tsa_digest = "sha256";
};

struct SignResult {
  bool ok = false;
  std::string error;
  bool timestamped = false;       // a TSA token was embedded
  std::string signer_name;        // certificate common name
  std::string timestamp_authority;
};

// Sign @in (an existing PDF) and write the signed PDF to @out. Performs an
// incremental update so existing revisions are preserved.
SignResult sign_pdf(const std::vector<uint8_t>& in, const SignOptions& opt,
                    std::vector<uint8_t>* out);

// Convenience file-based wrapper.
SignResult sign_pdf_file(const std::string& in_path, const std::string& out_path,
                         const SignOptions& opt);

// --- verification -----------------------------------------------------------

struct VerifyResult {
  bool checked = false;       // verification was attempted (CMS parsed)
  bool signature_valid = false;  // CMS signature is cryptographically valid
  bool covers_document = false;  // ByteRange spans the whole file
  std::string signer;            // signer certificate common name
  std::string signer_dn;         // signer full distinguished name
  std::string digest_algorithm;  // e.g. "SHA-256"
  std::string signing_time;      // from signed attributes (UTC)
  bool has_timestamp = false;
  std::string timestamp_time;    // RFC 3161 token genTime (UTC)
  std::string timestamp_authority;
  std::string error;
};

// Cryptographically verify a signature field against the full PDF bytes. Does
// NOT require a trusted CA store (self-signed certs verify); it confirms the
// signature math and ByteRange integrity, and parses any embedded RFC 3161
// timestamp. @br is the field's ByteRange [o1,l1,o2,l2]; @cms is the DER
// /Contents; @file_size is the total PDF size.
VerifyResult verify_signature(const std::vector<uint8_t>& pdf,
                              const std::vector<uint64_t>& byte_range,
                              const std::vector<uint8_t>& cms);

}  // namespace pdfview

#endif  // PDFVIEW_PDF_SIGN_HH_
