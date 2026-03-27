/**
 * PDF/A conformance validation unit tests
 *
 * Tests validate_pdfa() which checks a parsed PDF for PDF/A compliance
 * including XMP metadata, output intents, font embedding, encryption,
 * document info, and transparency restrictions.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;

// Helper: populate a Pdf struct so that it passes all PDF/A-1b checks.
// The Pdf struct is non-movable (contains std::mutex) so we take a reference.
static void init_valid_pdfa1b(Pdf& pdf) {
  pdf.encrypt = 0;

  // XMP metadata claiming PDF/A-1b
  pdf.catalog.xmp_metadata.pdfa_part = 1;
  pdf.catalog.xmp_metadata.pdfa_conformance = "B";

  // OutputIntent with the required subtype
  OutputIntentInfo oi;
  oi.subtype = "GTS_PDFA1";
  oi.output_condition_id = "sRGB";
  pdf.catalog.output_intents.push_back(oi);

  // Document info with a title
  pdf.catalog.document_info.title = "Valid PDF/A-1b Document";

  // One page with an embedded font
  Page page;
  page.page_number = 1;
  auto font = std::unique_ptr<BaseFont>(new BaseFont());
  font->base_font = "MyEmbeddedFont";
  font->subtype = "TrueType";
  auto* desc = new FontDescriptor();
  desc->font_file_type = FontFileType::FontFile2;
  desc->font_file_length = 1024;
  font->descriptor = desc;
  page.fonts["F1"] = std::move(font);
  page.fonts_loaded = true;
  pdf.catalog.pages.push_back(std::move(page));
}

TEST_SUITE("PdfAValidation") {

    TEST_CASE("Empty PDF with no XMP metadata") {
        Pdf pdf;

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);
        // Should have at least MissingXMPMetadata and MissingOutputIntent
        CHECK(result.violations.size() >= 2);

        bool has_missing_xmp = false;
        bool has_missing_output_intent = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::MissingXMPMetadata)
                has_missing_xmp = true;
            if (v.rule == PdfAViolation::Rule::MissingOutputIntent)
                has_missing_output_intent = true;
        }
        CHECK(has_missing_xmp);
        CHECK(has_missing_output_intent);
    }

    TEST_CASE("PDF with XMP metadata but no output intent") {
        Pdf pdf;
        pdf.catalog.xmp_metadata.pdfa_part = 1;
        pdf.catalog.xmp_metadata.pdfa_conformance = "B";
        pdf.catalog.document_info.title = "Test";

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);
        CHECK(result.claimed_level == "PDF/A-1B");

        bool has_missing_output_intent = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::MissingOutputIntent)
                has_missing_output_intent = true;
        }
        CHECK(has_missing_output_intent);
    }

    TEST_CASE("PDF with encryption") {
        Pdf pdf;
        pdf.catalog.xmp_metadata.pdfa_part = 1;
        pdf.catalog.xmp_metadata.pdfa_conformance = "B";
        pdf.catalog.document_info.title = "Encrypted";
        pdf.encrypt = 5;  // Non-zero means encrypted

        OutputIntentInfo oi;
        oi.subtype = "GTS_PDFA1";
        pdf.catalog.output_intents.push_back(oi);

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        bool has_encryption = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::EncryptionPresent)
                has_encryption = true;
        }
        CHECK(has_encryption);
    }

    TEST_CASE("PDF with all requirements met is valid") {
        Pdf pdf; init_valid_pdfa1b(pdf);

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK(result.valid);
        CHECK(result.violations.empty());
        CHECK(result.claimed_level == "PDF/A-1B");
    }

    TEST_CASE("Unembedded font produces FontNotEmbedded violation") {
        Pdf pdf; init_valid_pdfa1b(pdf);

        // Add a page with a font that has no descriptor
        Page page2;
        page2.page_number = 2;
        auto font = std::unique_ptr<BaseFont>(new BaseFont());
        font->base_font = "ArialMT";
        font->subtype = "TrueType";
        font->descriptor = nullptr;  // No descriptor = not embedded
        page2.fonts["F1"] = std::move(font);
        page2.fonts_loaded = true;
        pdf.catalog.pages.push_back(std::move(page2));

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        bool has_font_not_embedded = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::FontNotEmbedded) {
                has_font_not_embedded = true;
                CHECK(v.detail.find("ArialMT") != std::string::npos);
            }
        }
        CHECK(has_font_not_embedded);
    }

    TEST_CASE("Standard 14 font not embedded is reported") {
        Pdf pdf; init_valid_pdfa1b(pdf);

        // Replace the embedded font on page 1 with an unembedded Helvetica
        pdf.catalog.pages[0].fonts.clear();
        auto font = std::unique_ptr<BaseFont>(new BaseFont());
        font->base_font = "Helvetica";
        font->subtype = "Type1";
        font->descriptor = nullptr;
        pdf.catalog.pages[0].fonts["F1"] = std::move(font);

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        bool found = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::FontNotEmbedded) {
                found = true;
                CHECK(v.message.find("Standard 14") != std::string::npos);
                CHECK(v.detail.find("Helvetica") != std::string::npos);
            }
        }
        CHECK(found);
    }

    TEST_CASE("Missing document title produces MissingDocumentInfo") {
        Pdf pdf; init_valid_pdfa1b(pdf);
        pdf.catalog.document_info.title.clear();

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        bool found = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::MissingDocumentInfo)
                found = true;
        }
        CHECK(found);
    }

    TEST_CASE("Transparency on PDF/A-1 is a violation") {
        Pdf pdf; init_valid_pdfa1b(pdf);

        // Add ExtGState with transparency to the page resources
        Value ca_val;
        ca_val.type = Value::NUMBER;
        ca_val.number = 0.5;

        Value gs_dict_val;
        gs_dict_val.type = Value::DICTIONARY;
        gs_dict_val.dict["ca"] = ca_val;

        Value extgstate_val;
        extgstate_val.type = Value::DICTIONARY;
        extgstate_val.dict["GS1"] = gs_dict_val;

        pdf.catalog.pages[0].resources["ExtGState"] = extgstate_val;

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        bool found_transparency = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::TransparencyUsed)
                found_transparency = true;
        }
        CHECK(found_transparency);
    }

    TEST_CASE("SMask on PDF/A-1 is a violation") {
        Pdf pdf; init_valid_pdfa1b(pdf);

        // Add ExtGState with SMask to the page resources
        Value smask_val;
        smask_val.type = Value::DICTIONARY;  // A non-None SMask dict

        Value gs_dict_val;
        gs_dict_val.type = Value::DICTIONARY;
        gs_dict_val.dict["SMask"] = smask_val;

        Value extgstate_val;
        extgstate_val.type = Value::DICTIONARY;
        extgstate_val.dict["GS1"] = gs_dict_val;

        pdf.catalog.pages[0].resources["ExtGState"] = extgstate_val;

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        bool found_smask = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::TransparencyUsed) {
                if (v.message.find("SMask") != std::string::npos ||
                    v.message.find("Soft mask") != std::string::npos) {
                    found_smask = true;
                }
            }
        }
        CHECK(found_smask);
    }

    TEST_CASE("SMask with value None is not a violation") {
        Pdf pdf; init_valid_pdfa1b(pdf);

        // Add ExtGState with SMask = /None (acceptable)
        Value smask_val;
        smask_val.type = Value::NAME;
        smask_val.name = "None";

        Value gs_dict_val;
        gs_dict_val.type = Value::DICTIONARY;
        gs_dict_val.dict["SMask"] = smask_val;

        Value extgstate_val;
        extgstate_val.type = Value::DICTIONARY;
        extgstate_val.dict["GS1"] = gs_dict_val;

        pdf.catalog.pages[0].resources["ExtGState"] = extgstate_val;

        PdfAValidationResult result = validate_pdfa(pdf);

        // SMask /None should NOT produce a transparency violation
        for (const auto& v : result.violations) {
            CHECK(v.rule != PdfAViolation::Rule::TransparencyUsed);
        }
        CHECK(result.valid);
    }

    TEST_CASE("Transparency allowed for PDF/A-2") {
        Pdf pdf; init_valid_pdfa1b(pdf);
        // Change to PDF/A-2b which allows transparency
        pdf.catalog.xmp_metadata.pdfa_part = 2;
        pdf.catalog.xmp_metadata.pdfa_conformance = "B";

        // Add ExtGState with transparency
        Value ca_val;
        ca_val.type = Value::NUMBER;
        ca_val.number = 0.5;

        Value gs_dict_val;
        gs_dict_val.type = Value::DICTIONARY;
        gs_dict_val.dict["CA"] = ca_val;

        Value extgstate_val;
        extgstate_val.type = Value::DICTIONARY;
        extgstate_val.dict["GS1"] = gs_dict_val;

        pdf.catalog.pages[0].resources["ExtGState"] = extgstate_val;

        PdfAValidationResult result = validate_pdfa(pdf);

        // Transparency should NOT be flagged for PDF/A-2
        for (const auto& v : result.violations) {
            CHECK(v.rule != PdfAViolation::Rule::TransparencyUsed);
        }
        CHECK(result.valid);
        CHECK(result.claimed_level == "PDF/A-2B");
    }

    TEST_CASE("Violation count matches expected") {
        Pdf pdf;
        // Empty PDF: should get MissingXMPMetadata, MissingOutputIntent,
        // MissingDocumentInfo (no pages = no font violations, no
        // transparency violations)
        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        int xmp_count = 0;
        int oi_count = 0;
        int doc_count = 0;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::MissingXMPMetadata) xmp_count++;
            if (v.rule == PdfAViolation::Rule::MissingOutputIntent) oi_count++;
            if (v.rule == PdfAViolation::Rule::MissingDocumentInfo) doc_count++;
        }
        CHECK(xmp_count == 1);
        CHECK(oi_count == 1);
        CHECK(doc_count == 1);
        CHECK(result.violations.size() == 3);
    }

    TEST_CASE("Font with descriptor but None file type is not embedded") {
        Pdf pdf; init_valid_pdfa1b(pdf);

        // Replace page font with one that has a descriptor but no file
        pdf.catalog.pages[0].fonts.clear();
        auto font = std::unique_ptr<BaseFont>(new BaseFont());
        font->base_font = "CustomFont";
        font->subtype = "TrueType";
        auto* desc = new FontDescriptor();
        desc->font_file_type = FontFileType::None;  // Descriptor exists but no file
        desc->font_file_length = 0;
        font->descriptor = desc;
        pdf.catalog.pages[0].fonts["F1"] = std::move(font);

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        bool found = false;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::FontNotEmbedded) {
                found = true;
                CHECK(v.detail.find("CustomFont") != std::string::npos);
            }
        }
        CHECK(found);
    }

    TEST_CASE("Multiple pages with mixed font embedding") {
        Pdf pdf; init_valid_pdfa1b(pdf);

        // Page 1 already has an embedded font. Add page 2 with two fonts:
        // one embedded, one not.
        Page page2;
        page2.page_number = 2;

        auto embedded_font = std::unique_ptr<BaseFont>(new BaseFont());
        embedded_font->base_font = "EmbeddedCFF";
        embedded_font->subtype = "Type1";
        auto* desc1 = new FontDescriptor();
        desc1->font_file_type = FontFileType::FontFile3;
        desc1->font_file_length = 2048;
        embedded_font->descriptor = desc1;
        page2.fonts["F1"] = std::move(embedded_font);

        auto bad_font = std::unique_ptr<BaseFont>(new BaseFont());
        bad_font->base_font = "NotEmbedded";
        bad_font->subtype = "TrueType";
        bad_font->descriptor = nullptr;
        page2.fonts["F2"] = std::move(bad_font);

        page2.fonts_loaded = true;
        pdf.catalog.pages.push_back(std::move(page2));

        PdfAValidationResult result = validate_pdfa(pdf);

        CHECK_FALSE(result.valid);

        // Exactly one FontNotEmbedded violation
        int font_violations = 0;
        for (const auto& v : result.violations) {
            if (v.rule == PdfAViolation::Rule::FontNotEmbedded) {
                font_violations++;
                CHECK(v.detail.find("NotEmbedded") != std::string::npos);
                CHECK(v.detail.find("page=2") != std::string::npos);
            }
        }
        CHECK(font_violations == 1);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}
#endif  // NANOPDF_TEST_SUITE_NO_MAIN
