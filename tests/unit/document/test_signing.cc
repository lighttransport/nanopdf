// In-browser-style PKCS#12 signing round-trip: load a .p12, place a signature
// field, write_incremental_for_signing, build a detached CMS (RSA+SHA-256) and
// apply_signature, then re-parse and validate. This mirrors the WASM
// nanopdf_sign_pdf bridge's exact call sequence so the bridge stays covered by
// a native test (the .p12 uses the OpenSSL-3 default PBES2/PBKDF2/AES, the only
// scheme pkcs12::parse supports).
#include "nanotest.hh"
#include "nanopdf.hh"
#include "pdf-writer.hh"
#include "cms.hh"
#include "pkcs12.hh"
#include "test_helpers.hh"

#include <ctime>
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

std::string utc_now() {
  std::time_t t = std::time(nullptr);
  std::tm m{};
#if defined(_WIN32)
  gmtime_s(&m, &t);
#else
  gmtime_r(&t, &m);
#endif
  char b[16];
  std::snprintf(b, sizeof(b), "%02d%02d%02d%02d%02d%02dZ",
                (m.tm_year + 1900) % 100, m.tm_mon + 1, m.tm_mday,
                m.tm_hour, m.tm_min, m.tm_sec);
  return std::string(b);
}

}  // namespace

TEST_SUITE("Signing") {

TEST_CASE("Sign a PDF with a PKCS#12 bundle and validate the result") {
  std::vector<uint8_t> pdf_data, p12_data;
  REQUIRE(test::read_file(fixture_path("textpage.pdf"), pdf_data));
  REQUIRE(test::read_file(fixture_path("test_signer.p12"), p12_data));

  auto bundle = pkcs12::parse(p12_data.data(), p12_data.size(), "testpass");
  REQUIRE(bundle.valid);
  REQUIRE_FALSE(bundle.certs.empty());

  PdfWriter writer;
  std::string err;
  REQUIRE(writer.load_existing(pdf_data, &err));

  SignatureFieldConfig cfg;
  cfg.name = "Signature1";
  cfg.page = 0;
  cfg.x = 36; cfg.y = 36; cfg.width = 200; cfg.height = 48;
  cfg.visible = true;
  cfg.reason = "test";
  cfg.location = "here";
  REQUIRE_FALSE(writer.add_signature_field(cfg).empty());

  std::vector<uint8_t> bytes;
  auto wr = writer.write_incremental_for_signing(bytes, 32768);
  REQUIRE(wr.success);
  const auto& phs = writer.get_signature_placeholders();
  REQUIRE_EQ(phs.size(), size_t(1));

  const std::vector<uint8_t> signer = bundle.certs[0];
  std::vector<std::vector<uint8_t>> chain(bundle.certs.begin() + 1,
                                          bundle.certs.end());
  const std::string utc = utc_now();
  SigningCallback cb =
      [&](const std::vector<uint8_t>& data) -> std::vector<uint8_t> {
    return cms::build_signed_data(data, signer, chain, bundle.key, utc);
  };
  auto ar = apply_signature(bytes, phs[0], cb);
  REQUIRE(ar.success);

  // Re-parse the signed output and validate the embedded signature.
  Pdf pdf;
  REQUIRE(parse_from_memory(bytes.data(), bytes.size(), &pdf));
  REQUIRE(pdf.parse_signature_fields());
  REQUIRE_EQ(pdf.catalog.signature_fields.size(), size_t(1));

  auto vr = validate_signature(pdf, pdf.catalog.signature_fields[0]);
  CHECK(vr.success);
  CHECK(vr.integrity_valid);
  CHECK_EQ(vr.digest_algorithm, std::string("SHA-256"));
  CHECK(vr.signer_name.find("nanopdf Test Signer") != std::string::npos);

  // The original page text must survive (signing is an incremental update).
  pdf.load_document_structure();
  REQUIRE_FALSE(pdf.catalog.pages.empty());
  std::string text = extract_text_from_page(pdf, pdf.catalog.pages[0]);
  CHECK(text.find("Editable") != std::string::npos);
}

// The WASM RFC 3161 timestamping bridge (nanopdf_sign_prepare/finalize) relies
// on the RSA PKCS#1 v1.5 signature being deterministic for fixed signedAttrs:
// the signature timestamped by the TSA in the prepare phase must equal the one
// finally embedded. Guard that property without any network dependency by
// capturing the signature across two independent build_signed_data() runs.
TEST_CASE("CMS signature is deterministic for fixed signedAttrs") {
  std::vector<uint8_t> p12_data;
  REQUIRE(test::read_file(fixture_path("test_signer.p12"), p12_data));
  auto bundle = pkcs12::parse(p12_data.data(), p12_data.size(), "testpass");
  REQUIRE(bundle.valid);
  REQUIRE_FALSE(bundle.certs.empty());

  const std::vector<uint8_t> signer = bundle.certs[0];
  std::vector<std::vector<uint8_t>> chain(bundle.certs.begin() + 1,
                                          bundle.certs.end());
  const std::string utc = "260101000000Z";  // fixed signingTime
  const std::vector<uint8_t> content = {'h', 'e', 'l', 'l', 'o'};

  std::vector<uint8_t> sig1, sig2;
  cms::TsaCallback grab1 =
      [&](const std::vector<uint8_t>& s) -> std::vector<uint8_t> { sig1 = s; return {}; };
  cms::TsaCallback grab2 =
      [&](const std::vector<uint8_t>& s) -> std::vector<uint8_t> { sig2 = s; return {}; };
  cms::build_signed_data(content, signer, chain, bundle.key, utc, grab1);
  cms::build_signed_data(content, signer, chain, bundle.key, utc, grab2);

  REQUIRE_FALSE(sig1.empty());
  CHECK_EQ(sig1.size(), size_t(256));  // 2048-bit RSA
  CHECK(sig1 == sig2);
}

}  // namespace -> TEST_SUITE
