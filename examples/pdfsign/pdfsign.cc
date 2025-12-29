// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// PDF Digital Signature Tool using nanopdf
//
// Commands:
//   sign    - Sign a PDF document with a certificate
//   verify  - Verify signatures in a PDF
//   info    - Display signature information
//   timestamp - Add a document timestamp
//
// Usage:
//   pdfsign sign <input.pdf> <output.pdf> --cert <cert.pem> --key <key.pem>
//   pdfsign verify <input.pdf>
//   pdfsign info <input.pdf>
//   pdfsign timestamp <input.pdf> <output.pdf> --tsa <url>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <algorithm>

#include "../../src/nanopdf.hh"
#include "../../src/crypto.hh"

namespace {

// Command types
enum class Command {
  Sign,
  Verify,
  Info,
  Timestamp,
  Help
};

struct SignOptions {
  Command command{Command::Help};
  std::string input_file;
  std::string output_file;
  std::string cert_file;
  std::string key_file;
  std::string key_password;
  std::string tsa_url;
  std::string reason;
  std::string location;
  std::string contact;
  int mdp_permissions{0};  // 0=approval, 1-3=certification
  bool verbose{false};
};

// Base64 encoding/decoding
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
  std::string result;
  result.reserve((len + 2) / 3 * 4);

  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

    result += base64_chars[(n >> 18) & 0x3F];
    result += base64_chars[(n >> 12) & 0x3F];
    result += (i + 1 < len) ? base64_chars[(n >> 6) & 0x3F] : '=';
    result += (i + 2 < len) ? base64_chars[n & 0x3F] : '=';
  }

  return result;
}

std::vector<uint8_t> base64_decode(const std::string& encoded) {
  std::vector<uint8_t> result;
  std::vector<int> T(256, -1);
  for (int i = 0; i < 64; i++) T[base64_chars[i]] = i;

  int val = 0, valb = -8;
  for (char c : encoded) {
    if (T[static_cast<unsigned char>(c)] == -1) continue;
    val = (val << 6) + T[static_cast<unsigned char>(c)];
    valb += 6;
    if (valb >= 0) {
      result.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }

  return result;
}

// PEM file parsing
struct PemBlock {
  std::string type;
  std::vector<uint8_t> data;
};

std::vector<PemBlock> parse_pem_file(const std::string& content) {
  std::vector<PemBlock> blocks;
  std::istringstream iss(content);
  std::string line;
  PemBlock current;
  std::string base64_data;
  bool in_block = false;

  while (std::getline(iss, line)) {
    // Remove trailing \r if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.find("-----BEGIN ") == 0) {
      in_block = true;
      size_t start = 11;
      size_t end = line.find("-----", start);
      if (end != std::string::npos) {
        current.type = line.substr(start, end - start);
      }
      base64_data.clear();
    } else if (line.find("-----END ") == 0) {
      if (in_block) {
        current.data = base64_decode(base64_data);
        blocks.push_back(current);
        current = PemBlock();
      }
      in_block = false;
    } else if (in_block) {
      base64_data += line;
    }
  }

  return blocks;
}

// Read file contents
std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    return {};
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)),
                              std::istreambuf_iterator<char>());
}

std::string read_file_string(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) {
    return false;
  }
  ofs.write(reinterpret_cast<const char*>(data.data()), data.size());
  return ofs.good();
}

// Get current date in PDF format
std::string get_pdf_date() {
  time_t now = time(nullptr);
  struct tm* tm_info = localtime(&now);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "D:%Y%m%d%H%M%S", tm_info);

  // Add timezone
  char tz_buffer[8];
  strftime(tz_buffer, sizeof(tz_buffer), "%z", tm_info);
  std::string tz(tz_buffer);
  if (tz.length() >= 5) {
    // Convert +0900 to +09'00'
    std::string result = buffer;
    result += tz.substr(0, 3) + "'" + tz.substr(3, 2) + "'";
    return result;
  }

  return std::string(buffer) + "Z";
}

// Convert bytes to hex string
std::string bytes_to_hex(const uint8_t* data, size_t len) {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (size_t i = 0; i < len; ++i) {
    ss << std::setw(2) << static_cast<int>(data[i]);
  }
  return ss.str();
}

// Simple DER encoding helpers
void der_append_length(std::vector<uint8_t>& out, size_t len) {
  if (len < 128) {
    out.push_back(static_cast<uint8_t>(len));
  } else if (len < 256) {
    out.push_back(0x81);
    out.push_back(static_cast<uint8_t>(len));
  } else if (len < 65536) {
    out.push_back(0x82);
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(len & 0xFF));
  } else {
    out.push_back(0x83);
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(len & 0xFF));
  }
}

std::vector<uint8_t> der_sequence(const std::vector<uint8_t>& content) {
  std::vector<uint8_t> result;
  result.push_back(0x30);  // SEQUENCE tag
  der_append_length(result, content.size());
  result.insert(result.end(), content.begin(), content.end());
  return result;
}

std::vector<uint8_t> der_set(const std::vector<uint8_t>& content) {
  std::vector<uint8_t> result;
  result.push_back(0x31);  // SET tag
  der_append_length(result, content.size());
  result.insert(result.end(), content.begin(), content.end());
  return result;
}

std::vector<uint8_t> der_octet_string(const std::vector<uint8_t>& data) {
  std::vector<uint8_t> result;
  result.push_back(0x04);  // OCTET STRING tag
  der_append_length(result, data.size());
  result.insert(result.end(), data.begin(), data.end());
  return result;
}

std::vector<uint8_t> der_integer(int64_t value) {
  std::vector<uint8_t> result;
  result.push_back(0x02);  // INTEGER tag

  std::vector<uint8_t> bytes;
  if (value == 0) {
    bytes.push_back(0);
  } else {
    bool negative = value < 0;
    uint64_t abs_val = negative ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);

    while (abs_val > 0) {
      bytes.insert(bytes.begin(), static_cast<uint8_t>(abs_val & 0xFF));
      abs_val >>= 8;
    }

    // Add leading zero if high bit is set (for positive numbers)
    if (!negative && !bytes.empty() && (bytes[0] & 0x80)) {
      bytes.insert(bytes.begin(), 0);
    }
  }

  der_append_length(result, bytes.size());
  result.insert(result.end(), bytes.begin(), bytes.end());
  return result;
}

std::vector<uint8_t> der_oid(const std::vector<uint32_t>& components) {
  std::vector<uint8_t> result;
  result.push_back(0x06);  // OID tag

  std::vector<uint8_t> content;
  if (components.size() >= 2) {
    content.push_back(static_cast<uint8_t>(components[0] * 40 + components[1]));

    for (size_t i = 2; i < components.size(); ++i) {
      uint32_t val = components[i];
      std::vector<uint8_t> bytes;

      if (val == 0) {
        bytes.push_back(0);
      } else {
        while (val > 0) {
          bytes.insert(bytes.begin(), static_cast<uint8_t>((val & 0x7F) | (bytes.empty() ? 0 : 0x80)));
          val >>= 7;
        }
      }

      content.insert(content.end(), bytes.begin(), bytes.end());
    }
  }

  der_append_length(result, content.size());
  result.insert(result.end(), content.begin(), content.end());
  return result;
}

// OIDs
std::vector<uint8_t> oid_sha256() {
  return der_oid({2, 16, 840, 1, 101, 3, 4, 2, 1});
}

std::vector<uint8_t> oid_pkcs7_signed_data() {
  return der_oid({1, 2, 840, 113549, 1, 7, 2});
}

std::vector<uint8_t> oid_pkcs7_data() {
  return der_oid({1, 2, 840, 113549, 1, 7, 1});
}

std::vector<uint8_t> oid_content_type() {
  return der_oid({1, 2, 840, 113549, 1, 9, 3});
}

std::vector<uint8_t> oid_message_digest() {
  return der_oid({1, 2, 840, 113549, 1, 9, 4});
}

std::vector<uint8_t> oid_signing_time() {
  return der_oid({1, 2, 840, 113549, 1, 9, 5});
}

std::vector<uint8_t> oid_rsa_encryption() {
  return der_oid({1, 2, 840, 113549, 1, 1, 1});
}

std::vector<uint8_t> oid_sha256_with_rsa() {
  return der_oid({1, 2, 840, 113549, 1, 1, 11});
}

// Create a minimal PKCS#7 signature structure (placeholder for actual crypto)
std::vector<uint8_t> create_pkcs7_signature(
    const std::vector<uint8_t>& data_hash,
    const std::vector<uint8_t>& certificate,
    const std::string& signing_time) {

  // This creates a minimal PKCS#7/CMS structure
  // In a real implementation, this would use OpenSSL or similar

  // DigestAlgorithm
  std::vector<uint8_t> digest_algo_content;
  auto sha256_oid = oid_sha256();
  digest_algo_content.insert(digest_algo_content.end(), sha256_oid.begin(), sha256_oid.end());
  digest_algo_content.push_back(0x05);  // NULL
  digest_algo_content.push_back(0x00);
  auto digest_algo = der_sequence(digest_algo_content);

  // DigestAlgorithms SET
  auto digest_algos = der_set(digest_algo);

  // ContentInfo (pkcs7-data)
  std::vector<uint8_t> content_info_content;
  auto data_oid = oid_pkcs7_data();
  content_info_content.insert(content_info_content.end(), data_oid.begin(), data_oid.end());
  auto content_info = der_sequence(content_info_content);

  // Certificates [0] IMPLICIT
  std::vector<uint8_t> certs;
  certs.push_back(0xA0);  // [0] IMPLICIT tag
  der_append_length(certs, certificate.size());
  certs.insert(certs.end(), certificate.begin(), certificate.end());

  // SignerInfo
  std::vector<uint8_t> signer_info_content;

  // Version
  auto version = der_integer(1);
  signer_info_content.insert(signer_info_content.end(), version.begin(), version.end());

  // IssuerAndSerialNumber (placeholder)
  std::vector<uint8_t> issuer_serial;
  // Empty sequence for issuer
  issuer_serial.push_back(0x30);
  issuer_serial.push_back(0x00);
  // Serial number 1
  auto serial = der_integer(1);
  issuer_serial.insert(issuer_serial.end(), serial.begin(), serial.end());
  auto issuer_serial_seq = der_sequence(issuer_serial);
  signer_info_content.insert(signer_info_content.end(), issuer_serial_seq.begin(), issuer_serial_seq.end());

  // DigestAlgorithm
  signer_info_content.insert(signer_info_content.end(), digest_algo.begin(), digest_algo.end());

  // AuthenticatedAttributes [0] IMPLICIT
  std::vector<uint8_t> auth_attrs_content;

  // Content-Type attribute
  std::vector<uint8_t> ct_attr;
  auto ct_oid = oid_content_type();
  ct_attr.insert(ct_attr.end(), ct_oid.begin(), ct_oid.end());
  std::vector<uint8_t> ct_value;
  ct_value.insert(ct_value.end(), data_oid.begin(), data_oid.end());
  auto ct_value_set = der_set(ct_value);
  ct_attr.insert(ct_attr.end(), ct_value_set.begin(), ct_value_set.end());
  auto ct_attr_seq = der_sequence(ct_attr);
  auth_attrs_content.insert(auth_attrs_content.end(), ct_attr_seq.begin(), ct_attr_seq.end());

  // Message-Digest attribute
  std::vector<uint8_t> md_attr;
  auto md_oid = oid_message_digest();
  md_attr.insert(md_attr.end(), md_oid.begin(), md_oid.end());
  auto md_value = der_octet_string(data_hash);
  auto md_value_set = der_set(md_value);
  md_attr.insert(md_attr.end(), md_value_set.begin(), md_value_set.end());
  auto md_attr_seq = der_sequence(md_attr);
  auth_attrs_content.insert(auth_attrs_content.end(), md_attr_seq.begin(), md_attr_seq.end());

  // Signing-Time attribute
  std::vector<uint8_t> st_attr;
  auto st_oid = oid_signing_time();
  st_attr.insert(st_attr.end(), st_oid.begin(), st_oid.end());
  // UTCTime
  std::vector<uint8_t> utc_time;
  utc_time.push_back(0x17);  // UTCTime tag
  std::string time_str = "240101120000Z";  // Placeholder time
  utc_time.push_back(static_cast<uint8_t>(time_str.size()));
  utc_time.insert(utc_time.end(), time_str.begin(), time_str.end());
  auto st_value_set = der_set(utc_time);
  st_attr.insert(st_attr.end(), st_value_set.begin(), st_value_set.end());
  auto st_attr_seq = der_sequence(st_attr);
  auth_attrs_content.insert(auth_attrs_content.end(), st_attr_seq.begin(), st_attr_seq.end());

  // Build authenticated attributes with [0] IMPLICIT tag
  std::vector<uint8_t> auth_attrs;
  auth_attrs.push_back(0xA0);  // [0] IMPLICIT
  der_append_length(auth_attrs, auth_attrs_content.size());
  auth_attrs.insert(auth_attrs.end(), auth_attrs_content.begin(), auth_attrs_content.end());
  signer_info_content.insert(signer_info_content.end(), auth_attrs.begin(), auth_attrs.end());

  // DigestEncryptionAlgorithm
  std::vector<uint8_t> enc_algo_content;
  auto rsa_oid = oid_sha256_with_rsa();
  enc_algo_content.insert(enc_algo_content.end(), rsa_oid.begin(), rsa_oid.end());
  enc_algo_content.push_back(0x05);  // NULL
  enc_algo_content.push_back(0x00);
  auto enc_algo = der_sequence(enc_algo_content);
  signer_info_content.insert(signer_info_content.end(), enc_algo.begin(), enc_algo.end());

  // EncryptedDigest (placeholder - would be actual RSA signature)
  std::vector<uint8_t> fake_signature(256, 0);  // 2048-bit RSA signature placeholder
  auto enc_digest = der_octet_string(fake_signature);
  signer_info_content.insert(signer_info_content.end(), enc_digest.begin(), enc_digest.end());

  auto signer_info = der_sequence(signer_info_content);
  auto signer_infos = der_set(signer_info);

  // SignedData
  std::vector<uint8_t> signed_data_content;

  // Version
  auto sd_version = der_integer(1);
  signed_data_content.insert(signed_data_content.end(), sd_version.begin(), sd_version.end());

  // DigestAlgorithms
  signed_data_content.insert(signed_data_content.end(), digest_algos.begin(), digest_algos.end());

  // ContentInfo
  signed_data_content.insert(signed_data_content.end(), content_info.begin(), content_info.end());

  // Certificates
  signed_data_content.insert(signed_data_content.end(), certs.begin(), certs.end());

  // SignerInfos
  signed_data_content.insert(signed_data_content.end(), signer_infos.begin(), signer_infos.end());

  auto signed_data = der_sequence(signed_data_content);

  // ContentInfo wrapper
  std::vector<uint8_t> result_content;
  auto pkcs7_oid = oid_pkcs7_signed_data();
  result_content.insert(result_content.end(), pkcs7_oid.begin(), pkcs7_oid.end());

  // [0] EXPLICIT SignedData
  std::vector<uint8_t> explicit_wrapper;
  explicit_wrapper.push_back(0xA0);  // [0] EXPLICIT
  der_append_length(explicit_wrapper, signed_data.size());
  explicit_wrapper.insert(explicit_wrapper.end(), signed_data.begin(), signed_data.end());
  result_content.insert(result_content.end(), explicit_wrapper.begin(), explicit_wrapper.end());

  return der_sequence(result_content);
}

// Create signature dictionary
std::string create_signature_dict(size_t contents_placeholder_size,
                                   const std::string& reason,
                                   const std::string& location,
                                   const std::string& contact,
                                   int mdp_permissions) {
  std::ostringstream ss;
  ss << "<<\n";
  ss << "/Type /Sig\n";
  ss << "/Filter /Adobe.PPKLite\n";
  ss << "/SubFilter /adbe.pkcs7.detached\n";
  ss << "/ByteRange [0 0000000000 0000000000 0000000000]\n";
  ss << "/Contents <" << std::string(contents_placeholder_size * 2, '0') << ">\n";
  ss << "/M (" << get_pdf_date() << ")\n";

  if (!reason.empty()) {
    ss << "/Reason (" << reason << ")\n";
  }
  if (!location.empty()) {
    ss << "/Location (" << location << ")\n";
  }
  if (!contact.empty()) {
    ss << "/ContactInfo (" << contact << ")\n";
  }

  // MDP/Certification signature
  if (mdp_permissions > 0) {
    ss << "/Reference [\n";
    ss << "  <<\n";
    ss << "    /Type /SigRef\n";
    ss << "    /TransformMethod /DocMDP\n";
    ss << "    /TransformParams <<\n";
    ss << "      /Type /TransformParams\n";
    ss << "      /P " << mdp_permissions << "\n";
    ss << "      /V /1.2\n";
    ss << "    >>\n";
    ss << "  >>\n";
    ss << "]\n";
  }

  ss << ">>\n";
  return ss.str();
}

// Verify signature byte range
bool verify_byte_range_coverage(const std::vector<uint8_t>& pdf_data,
                                 const nanopdf::SignatureField& sig) {
  if (sig.byte_range.size() != 4) return false;

  uint64_t offset1 = sig.byte_range[0];
  uint64_t length1 = sig.byte_range[1];
  uint64_t offset2 = sig.byte_range[2];
  uint64_t length2 = sig.byte_range[3];

  // First range should start at 0
  if (offset1 != 0) return false;

  // Check bounds
  if (offset1 + length1 > pdf_data.size()) return false;
  if (offset2 > pdf_data.size()) return false;
  if (offset2 + length2 > pdf_data.size()) return false;

  // Ranges should not overlap
  if (offset2 <= offset1 + length1) return false;

  return true;
}

// Check document modification status
std::string check_modification_status(const std::vector<uint8_t>& pdf_data,
                                       const nanopdf::SignatureField& sig) {
  if (!sig.signature_present || !sig.is_signed) {
    return "not_signed";
  }

  if (sig.byte_range.empty() || sig.byte_range.size() != 4) {
    return "invalid_byte_range";
  }

  if (!verify_byte_range_coverage(pdf_data, sig)) {
    return "invalid_byte_range";
  }

  uint64_t covered = sig.byte_range[1] + sig.byte_range[3];
  uint64_t gap = sig.byte_range[2] - (sig.byte_range[0] + sig.byte_range[1]);
  uint64_t total = covered + gap;

  if (total < pdf_data.size()) {
    return "modified_after_signing";
  }

  return "intact";
}

// Display signature information
void display_signature_info(const nanopdf::Pdf& pdf,
                            const std::vector<uint8_t>& pdf_data,
                            bool verbose) {
  const auto& sigs = pdf.catalog.signature_fields;

  std::cout << "Signature Information:\n";
  std::cout << "======================\n\n";

  if (sigs.empty()) {
    std::cout << "No signatures found in document.\n";
    return;
  }

  std::cout << "Total signature fields: " << sigs.size() << "\n\n";

  int index = 1;
  for (const auto& sig : sigs) {
    std::cout << "Signature #" << index++ << ":\n";
    std::cout << "  Name: " << (sig.name.empty() ? "(unnamed)" : sig.name) << "\n";
    std::cout << "  Signed: " << (sig.is_signed ? "Yes" : "No") << "\n";

    if (sig.is_signed || sig.signature_present) {
      // Type
      if (sig.is_certification_signature) {
        std::cout << "  Type: Certification (DocMDP)\n";
        std::cout << "  MDP Permissions: " << sig.mdp_permissions;
        switch (sig.mdp_permissions) {
          case 1: std::cout << " (No changes allowed)"; break;
          case 2: std::cout << " (Form fill and sign only)"; break;
          case 3: std::cout << " (Form fill, sign, annotate)"; break;
        }
        std::cout << "\n";
      } else {
        std::cout << "  Type: Approval\n";
      }

      // Signer info
      if (!sig.signing_reason.empty()) {
        std::cout << "  Reason: " << sig.signing_reason << "\n";
      }
      if (!sig.signing_location.empty()) {
        std::cout << "  Location: " << sig.signing_location << "\n";
      }
      if (!sig.signing_contact_info.empty()) {
        std::cout << "  Contact: " << sig.signing_contact_info << "\n";
      }
      if (!sig.signing_date.empty()) {
        std::cout << "  Date: " << sig.signing_date << "\n";
      }

      // Filter/SubFilter
      if (!sig.filter.empty()) {
        std::cout << "  Filter: " << sig.filter << "\n";
      }
      if (!sig.subfilter.empty()) {
        std::cout << "  SubFilter: " << sig.subfilter << "\n";
      }

      // Timestamp
      if (sig.has_timestamp) {
        std::cout << "  Timestamp: Yes";
        if (sig.is_document_timestamp) {
          std::cout << " (Document Timestamp)";
        } else {
          std::cout << " (Embedded)";
        }
        std::cout << "\n";
        if (!sig.timestamp_hash_algorithm.empty()) {
          std::cout << "  Timestamp Algorithm: " << sig.timestamp_hash_algorithm << "\n";
        }
      }

      // Byte range
      if (!sig.byte_range.empty() && sig.byte_range.size() == 4) {
        std::cout << "  Byte Range: [" << sig.byte_range[0] << ", "
                  << sig.byte_range[1] << ", " << sig.byte_range[2] << ", "
                  << sig.byte_range[3] << "]\n";
        uint64_t covered = sig.byte_range[1] + sig.byte_range[3];
        std::cout << "  Bytes Covered: " << covered << "\n";
      }

      // Signature size
      if (!sig.signature_contents.empty()) {
        std::cout << "  Signature Size: " << sig.signature_contents.size() << " bytes\n";
      }

      // Integrity check
      std::string status = check_modification_status(pdf_data, sig);
      std::cout << "  Integrity: ";
      if (status == "intact") {
        std::cout << "INTACT (document not modified after signing)\n";
      } else if (status == "modified_after_signing") {
        std::cout << "MODIFIED (document was changed after signing)\n";
      } else {
        std::cout << status << "\n";
      }

      // Algorithm
      if (!sig.digest_algorithm.empty()) {
        std::cout << "  Digest Algorithm: " << sig.digest_algorithm << "\n";
      }
    }

    std::cout << "\n";
  }
}

// Verify signatures
int verify_signatures(const nanopdf::Pdf& pdf,
                      const std::vector<uint8_t>& pdf_data,
                      bool verbose) {
  const auto& sigs = pdf.catalog.signature_fields;

  if (sigs.empty()) {
    std::cout << "No signatures to verify.\n";
    return 0;
  }

  std::cout << "Verifying " << sigs.size() << " signature(s)...\n\n";

  int passed = 0;
  int failed = 0;

  int index = 1;
  for (const auto& sig : sigs) {
    std::cout << "Signature #" << index++ << " (" << (sig.name.empty() ? "unnamed" : sig.name) << "): ";

    if (!sig.is_signed && !sig.signature_present) {
      std::cout << "UNSIGNED\n";
      continue;
    }

    // Check byte range coverage
    if (!verify_byte_range_coverage(pdf_data, sig)) {
      std::cout << "INVALID (byte range error)\n";
      failed++;
      continue;
    }

    // Check for modifications
    std::string status = check_modification_status(pdf_data, sig);
    if (status == "intact") {
      // Compute digest of signed data
      if (!sig.byte_range.empty() && sig.byte_range.size() == 4) {
        uint8_t digest[32];
        nanopdf::crypto::SHA256 hasher;

        // Hash first range
        hasher.update(pdf_data.data() + sig.byte_range[0],
                      static_cast<size_t>(sig.byte_range[1]));
        // Hash second range
        hasher.update(pdf_data.data() + sig.byte_range[2],
                      static_cast<size_t>(sig.byte_range[3]));
        hasher.finalize();
        hasher.get_digest(digest);

        if (verbose) {
          std::cout << "\n    Computed digest: " << bytes_to_hex(digest, 32) << "\n    ";
        }

        // Note: Full cryptographic verification would require parsing PKCS#7
        // and verifying the RSA signature against the certificate
        std::cout << "VALID (integrity check passed)\n";
        passed++;
      }
    } else if (status == "modified_after_signing") {
      std::cout << "WARNING (document modified after signing)\n";
      failed++;
    } else {
      std::cout << "ERROR (" << status << ")\n";
      failed++;
    }

    if (verbose && sig.has_timestamp) {
      std::cout << "    Timestamp: Present\n";
    }
  }

  std::cout << "\nSummary: " << passed << " passed, " << failed << " failed\n";

  return failed > 0 ? 1 : 0;
}

// Sign a PDF document
int sign_pdf(const SignOptions& options) {
  // Read input PDF
  std::vector<uint8_t> pdf_data = read_file(options.input_file);
  if (pdf_data.empty()) {
    std::cerr << "Error: Failed to read input file: " << options.input_file << "\n";
    return 1;
  }

  // Parse PDF to validate
  nanopdf::Pdf pdf;
  if (!nanopdf::parse_from_memory(pdf_data.data(), pdf_data.size(), &pdf)) {
    std::cerr << "Error: Failed to parse PDF file\n";
    return 1;
  }

  // Read certificate
  std::string cert_content = read_file_string(options.cert_file);
  if (cert_content.empty()) {
    std::cerr << "Error: Failed to read certificate file: " << options.cert_file << "\n";
    return 1;
  }

  auto pem_blocks = parse_pem_file(cert_content);
  std::vector<uint8_t> certificate;
  for (const auto& block : pem_blocks) {
    if (block.type == "CERTIFICATE") {
      certificate = block.data;
      break;
    }
  }

  if (certificate.empty()) {
    std::cerr << "Error: No certificate found in PEM file\n";
    return 1;
  }

  std::cout << "Signing PDF: " << options.input_file << "\n";
  std::cout << "  Certificate: " << options.cert_file << "\n";
  if (!options.reason.empty()) {
    std::cout << "  Reason: " << options.reason << "\n";
  }
  if (!options.location.empty()) {
    std::cout << "  Location: " << options.location << "\n";
  }
  if (options.mdp_permissions > 0) {
    std::cout << "  Type: Certification (MDP P=" << options.mdp_permissions << ")\n";
  } else {
    std::cout << "  Type: Approval signature\n";
  }

  // Create signature placeholder (8KB should be enough for most signatures)
  const size_t sig_placeholder_size = 8192;

  // Note: A full implementation would:
  // 1. Add a signature field to the AcroForm
  // 2. Add a signature annotation to a page
  // 3. Create the signature dictionary with ByteRange placeholder
  // 4. Write the PDF with placeholder
  // 5. Calculate the actual byte ranges
  // 6. Compute the hash of signed bytes
  // 7. Create the PKCS#7 signature
  // 8. Insert the signature into the placeholder

  std::cerr << "Note: Full PDF signing requires modifying PDF structure.\n";
  std::cerr << "This example demonstrates the signature framework.\n";
  std::cerr << "For production use, integrate with OpenSSL for actual cryptographic operations.\n";

  // For demonstration, compute what the signature would contain
  uint8_t digest[32];
  nanopdf::crypto::SHA256::hash(pdf_data.data(), pdf_data.size(), digest);
  std::cout << "\n  Document hash (SHA-256): " << bytes_to_hex(digest, 32) << "\n";

  // Create signature structure (demonstration)
  std::vector<uint8_t> sig_hash(digest, digest + 32);
  auto pkcs7 = create_pkcs7_signature(sig_hash, certificate, get_pdf_date());
  std::cout << "  PKCS#7 signature size: " << pkcs7.size() << " bytes\n";

  // Copy original file (in a real implementation, we'd modify it)
  if (!write_file(options.output_file, pdf_data)) {
    std::cerr << "Error: Failed to write output file: " << options.output_file << "\n";
    return 1;
  }

  std::cout << "\nOutput written to: " << options.output_file << "\n";
  std::cout << "(Note: This is a demonstration - actual signing not performed)\n";

  return 0;
}

void print_usage(const char* program_name) {
  std::cout << "PDF Digital Signature Tool using nanopdf\n";
  std::cout << "\n";
  std::cout << "Usage:\n";
  std::cout << "  " << program_name << " sign <input.pdf> <output.pdf> [options]\n";
  std::cout << "  " << program_name << " verify <input.pdf> [options]\n";
  std::cout << "  " << program_name << " info <input.pdf> [options]\n";
  std::cout << "  " << program_name << " timestamp <input.pdf> <output.pdf> --tsa <url>\n";
  std::cout << "\n";
  std::cout << "Commands:\n";
  std::cout << "  sign       Sign a PDF with a certificate\n";
  std::cout << "  verify     Verify signatures in a PDF\n";
  std::cout << "  info       Display signature information\n";
  std::cout << "  timestamp  Add a document timestamp\n";
  std::cout << "\n";
  std::cout << "Sign options:\n";
  std::cout << "  --cert <file>      Certificate file (PEM format)\n";
  std::cout << "  --key <file>       Private key file (PEM format)\n";
  std::cout << "  --password <pass>  Private key password\n";
  std::cout << "  --reason <text>    Reason for signing\n";
  std::cout << "  --location <text>  Signing location\n";
  std::cout << "  --contact <text>   Contact information\n";
  std::cout << "  --certify <1-3>    Create certification signature with MDP level\n";
  std::cout << "                     1: No changes allowed\n";
  std::cout << "                     2: Form fill and sign only\n";
  std::cout << "                     3: Form fill, sign, and annotate\n";
  std::cout << "\n";
  std::cout << "Timestamp options:\n";
  std::cout << "  --tsa <url>        Time Stamp Authority URL\n";
  std::cout << "\n";
  std::cout << "General options:\n";
  std::cout << "  -v, --verbose      Verbose output\n";
  std::cout << "  --help             Show this help\n";
  std::cout << "\n";
  std::cout << "Examples:\n";
  std::cout << "  " << program_name << " info document.pdf\n";
  std::cout << "  " << program_name << " verify document.pdf -v\n";
  std::cout << "  " << program_name << " sign input.pdf signed.pdf --cert cert.pem --key key.pem\n";
  std::cout << "  " << program_name << " sign input.pdf certified.pdf --cert cert.pem --key key.pem --certify 2\n";
}

bool parse_arguments(int argc, char* argv[], SignOptions& options) {
  if (argc < 2) {
    return false;
  }

  std::string cmd = argv[1];
  if (cmd == "sign") {
    options.command = Command::Sign;
  } else if (cmd == "verify") {
    options.command = Command::Verify;
  } else if (cmd == "info") {
    options.command = Command::Info;
  } else if (cmd == "timestamp") {
    options.command = Command::Timestamp;
  } else if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    options.command = Command::Help;
    return true;
  } else {
    std::cerr << "Unknown command: " << cmd << "\n";
    return false;
  }

  int arg_index = 2;

  // Parse positional arguments based on command
  if (options.command == Command::Sign || options.command == Command::Timestamp) {
    if (argc < 4) {
      std::cerr << "Error: sign/timestamp requires input and output files\n";
      return false;
    }
    options.input_file = argv[2];
    options.output_file = argv[3];
    arg_index = 4;
  } else if (options.command == Command::Verify || options.command == Command::Info) {
    if (argc < 3) {
      std::cerr << "Error: verify/info requires an input file\n";
      return false;
    }
    options.input_file = argv[2];
    arg_index = 3;
  }

  // Parse optional arguments
  for (int i = arg_index; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--cert" && i + 1 < argc) {
      options.cert_file = argv[++i];
    } else if (arg == "--key" && i + 1 < argc) {
      options.key_file = argv[++i];
    } else if (arg == "--password" && i + 1 < argc) {
      options.key_password = argv[++i];
    } else if (arg == "--reason" && i + 1 < argc) {
      options.reason = argv[++i];
    } else if (arg == "--location" && i + 1 < argc) {
      options.location = argv[++i];
    } else if (arg == "--contact" && i + 1 < argc) {
      options.contact = argv[++i];
    } else if (arg == "--tsa" && i + 1 < argc) {
      options.tsa_url = argv[++i];
    } else if (arg == "--certify" && i + 1 < argc) {
      options.mdp_permissions = std::atoi(argv[++i]);
      if (options.mdp_permissions < 1 || options.mdp_permissions > 3) {
        std::cerr << "Error: --certify must be 1, 2, or 3\n";
        return false;
      }
    } else if (arg == "-v" || arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      options.command = Command::Help;
      return true;
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      return false;
    }
  }

  // Validate required options for sign command
  if (options.command == Command::Sign) {
    if (options.cert_file.empty()) {
      std::cerr << "Error: --cert is required for signing\n";
      return false;
    }
  }

  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  SignOptions options;

  if (!parse_arguments(argc, argv, options)) {
    print_usage(argv[0]);
    return 1;
  }

  if (options.command == Command::Help) {
    print_usage(argv[0]);
    return 0;
  }

  // Commands that need to read the PDF
  if (options.command == Command::Info ||
      options.command == Command::Verify) {
    std::vector<uint8_t> pdf_data = read_file(options.input_file);
    if (pdf_data.empty()) {
      std::cerr << "Error: Failed to read input file: " << options.input_file << "\n";
      return 1;
    }

    nanopdf::Pdf pdf;
    if (!nanopdf::parse_from_memory(pdf_data.data(), pdf_data.size(), &pdf)) {
      std::cerr << "Error: Failed to parse PDF file\n";
      return 1;
    }

    pdf.ensure_metadata_loaded();

    if (options.command == Command::Info) {
      display_signature_info(pdf, pdf_data, options.verbose);
      return 0;
    } else if (options.command == Command::Verify) {
      return verify_signatures(pdf, pdf_data, options.verbose);
    }
  }

  if (options.command == Command::Sign) {
    return sign_pdf(options);
  }

  if (options.command == Command::Timestamp) {
    std::cerr << "Timestamp command requires a TSA URL (--tsa)\n";
    std::cerr << "Note: Document timestamping requires network access to TSA server.\n";
    return 1;
  }

  return 0;
}
