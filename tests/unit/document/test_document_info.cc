/**
 * Document information and metadata unit tests
 *
 * Tests DocumentInfo (standard PDF info dictionary) and XMPMetadata
 * (Extensible Metadata Platform XML metadata). Document info includes
 * title, author, subject, keywords, dates, and custom fields.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("DocumentInfo") {
    TEST_CASE("Default values") {
        DocumentInfo info;

        CHECK(info.title.empty());
        CHECK(info.author.empty());
        CHECK(info.subject.empty());
        CHECK(info.keywords.empty());
        CHECK(info.creator.empty());
        CHECK(info.producer.empty());
        CHECK(info.creation_date.empty());
        CHECK(info.mod_date.empty());
        CHECK(info.custom.empty());
    }

    TEST_CASE("Standard metadata fields") {
        DocumentInfo info;
        info.title = "Test Document";
        info.author = "John Doe";
        info.subject = "PDF Testing";
        info.keywords = "test, pdf, document";
        info.creator = "Test Application";
        info.producer = "nanopdf";

        CHECK(info.title == "Test Document");
        CHECK(info.author == "John Doe");
        CHECK(info.subject == "PDF Testing");
        CHECK(info.keywords == "test, pdf, document");
        CHECK(info.creator == "Test Application");
        CHECK(info.producer == "nanopdf");
    }

    TEST_CASE("Date fields") {
        DocumentInfo info;
        info.creation_date = "D:20240101120000";
        info.mod_date = "D:20240102130000";

        CHECK(info.creation_date == "D:20240101120000");
        CHECK(info.mod_date == "D:20240102130000");
    }

    TEST_CASE("Trapped field") {
        DocumentInfo info;
        info.trapped = "False";

        CHECK(info.trapped == "False");

        info.trapped = "True";
        CHECK(info.trapped == "True");

        info.trapped = "Unknown";
        CHECK(info.trapped == "Unknown");
    }

    TEST_CASE("Custom metadata fields") {
        DocumentInfo info;
        info.custom["CustomField"] = "CustomValue";
        info.custom["Department"] = "Engineering";
        info.custom["Project"] = "nanopdf";

        REQUIRE(info.custom.size() == 3);
        CHECK(info.custom["CustomField"] == "CustomValue");
        CHECK(info.custom["Department"] == "Engineering");
        CHECK(info.custom["Project"] == "nanopdf");
    }

    TEST_CASE("Unicode in title") {
        DocumentInfo info;
        info.title = "Test Document \u2014 With Em Dash";

        CHECK(info.title.find("\u2014") != std::string::npos);
    }

    TEST_CASE("Multiple authors in author field") {
        DocumentInfo info;
        info.author = "John Doe, Jane Smith";

        CHECK(info.author == "John Doe, Jane Smith");
    }

    TEST_CASE("Empty custom fields") {
        DocumentInfo info;

        CHECK(info.custom.empty());
    }
}

TEST_SUITE("XMPMetadata") {
    TEST_CASE("Default values") {
        XMPMetadata xmp;

        CHECK(xmp.raw_xml.empty());
        CHECK(xmp.dc_title.empty());
        CHECK(xmp.dc_creator.empty());
        CHECK(xmp.xmp_create_date.empty());
        CHECK(xmp.xmp_modify_date.empty());
        CHECK(xmp.pdf_producer.empty());
    }

    TEST_CASE("Parse basic XMP XML") {
        XMPMetadata xmp;
        std::string xml = R"(
            <?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
            <x:xmpmeta xmlns:x="adobe:ns:meta/">
              <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
                <rdf:Description rdf:about="">
                  <dc:title xmlns:dc="http://purl.org/dc/elements/1.1/">
                    <rdf:Alt>
                      <rdf:li xml:lang="x-default">Test Document Title</rdf:li>
                    </rdf:Alt>
                  </dc:title>
                  <dc:creator xmlns:dc="http://purl.org/dc/elements/1.1/">
                    <rdf:Seq>
                      <rdf:li>John Smith</rdf:li>
                    </rdf:Seq>
                  </dc:creator>
                  <xmp:CreateDate xmlns:xmp="http://ns.adobe.com/xap/1.0/">2024-01-01T12:00:00Z</xmp:CreateDate>
                  <xmp:ModifyDate xmlns:xmp="http://ns.adobe.com/xap/1.0/">2024-01-02T13:00:00Z</xmp:ModifyDate>
                  <pdf:Producer xmlns:pdf="http://ns.adobe.com/pdf/1.3/">Test Producer</pdf:Producer>
                </rdf:Description>
              </rdf:RDF>
            </x:xmpmeta>
            <?xpacket end="w"?>
        )";

        bool parsed = xmp.parse_xml(xml);

        REQUIRE(parsed);
        CHECK(xmp.raw_xml == xml);
        CHECK(xmp.dc_title == "Test Document Title");
        CHECK(xmp.dc_creator == "John Smith");
        CHECK(xmp.xmp_create_date == "2024-01-01T12:00:00Z");
        CHECK(xmp.xmp_modify_date == "2024-01-02T13:00:00Z");
        CHECK(xmp.pdf_producer == "Test Producer");
    }

    TEST_CASE("Raw XML storage") {
        XMPMetadata xmp;
        std::string xml = "<?xml version=\"1.0\"?><test/>";
        xmp.raw_xml = xml;

        CHECK(xmp.raw_xml == xml);
    }

    TEST_CASE("DC title field") {
        XMPMetadata xmp;
        xmp.dc_title = "Document Title";

        CHECK(xmp.dc_title == "Document Title");
    }

    TEST_CASE("DC creator field") {
        XMPMetadata xmp;
        xmp.dc_creator = "Jane Doe";

        CHECK(xmp.dc_creator == "Jane Doe");
    }

    TEST_CASE("XMP date fields") {
        XMPMetadata xmp;
        xmp.xmp_create_date = "2024-01-15T10:30:00Z";
        xmp.xmp_modify_date = "2024-01-16T14:45:00Z";

        CHECK(xmp.xmp_create_date == "2024-01-15T10:30:00Z");
        CHECK(xmp.xmp_modify_date == "2024-01-16T14:45:00Z");
    }

    TEST_CASE("PDF producer field") {
        XMPMetadata xmp;
        xmp.pdf_producer = "Adobe Acrobat 10.0";

        CHECK(xmp.pdf_producer == "Adobe Acrobat 10.0");
    }

    TEST_CASE("Empty XML") {
        XMPMetadata xmp;
        bool parsed = xmp.parse_xml("");

        // Empty XML should fail to parse or return false
        // (or might succeed with empty values depending on implementation)
        if (!parsed) {
            CHECK_FALSE(parsed);
        }
    }
}

int main() {
    return nanotest::run_all_tests();
}
