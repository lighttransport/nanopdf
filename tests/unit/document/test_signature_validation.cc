// Signature validation unit tests: digest-algorithm coverage and BER-encoded
// PKCS#7/CMS parsing. Fixtures are self-signed (throwaway cert) PDFs produced
// with OpenSSL; we only assert on integrity (ByteRange hash == messageDigest),
// not on trust-chain validity.
#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

#include <string>
#include <vector>

using namespace nanopdf;

namespace {

std::string fixture_path(const std::string& filename) {
  std::string current_file = __FILE__;
  size_t slash = current_file.find_last_of("/\\");
  std::string dir = slash == std::string::npos
      ? std::string(".")
      : current_file.substr(0, slash);
  return dir + "/../../fixtures/" + filename;
}

}  // namespace

TEST_SUITE("SignatureValidation") {

// A SHA-512-signed PDF must validate (regression: previously failed with
// "Unsupported digest algorithm: SHA-512").
TEST_CASE("Validates a SHA-512 detached signature") {
  std::vector<uint8_t> data;
  REQUIRE(test::read_file(fixture_path("signature_sha512.pdf"), data));
  Pdf pdf;
  REQUIRE(parse_from_memory(data.data(), data.size(), &pdf));
  REQUIRE(pdf.load_document_structure());
  REQUIRE(pdf.parse_signature_fields());
  REQUIRE_EQ(pdf.catalog.signature_fields.size(), size_t(1));

  SignatureValidationResult r =
      validate_signature(pdf, pdf.catalog.signature_fields[0]);
  CHECK_EQ(r.digest_algorithm, std::string("SHA-512"));
  CHECK(r.integrity_valid);
  CHECK(r.success);
  CHECK_EQ(r.error, std::string(""));
}

// A BER indefinite-length encoded CMS must parse and validate (regression:
// previously failed with "Failed to parse PKCS#7 ContentInfo outer SEQUENCE").
TEST_CASE("Parses a BER indefinite-length PKCS#7 signature") {
  std::vector<uint8_t> data;
  REQUIRE(test::read_file(fixture_path("signature_ber_indefinite.pdf"), data));
  Pdf pdf;
  REQUIRE(parse_from_memory(data.data(), data.size(), &pdf));
  REQUIRE(pdf.load_document_structure());
  REQUIRE(pdf.parse_signature_fields());
  REQUIRE_EQ(pdf.catalog.signature_fields.size(), size_t(1));

  SignatureValidationResult r =
      validate_signature(pdf, pdf.catalog.signature_fields[0]);
  CHECK_EQ(r.digest_algorithm, std::string("SHA-256"));
  CHECK(r.integrity_valid);
  CHECK(r.success);
  CHECK_FALSE(r.signer_name.empty());
  CHECK_EQ(r.error, std::string(""));
}

}  // namespace -> TEST_SUITE
