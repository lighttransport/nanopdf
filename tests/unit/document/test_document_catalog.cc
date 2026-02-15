/**
 * Document catalog unit tests
 *
 * Tests DocumentCatalog which is the root of the PDF document structure.
 * The catalog contains page tree, outlines, page labels, named destinations,
 * document info, and XMP metadata.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("DocumentCatalog") {
    TEST_CASE("Default catalog") {
        DocumentCatalog catalog;

        CHECK(catalog.pages.empty());
        CHECK(catalog.outline_root == nullptr);
        CHECK(catalog.named_destinations.empty());
    }

    TEST_CASE("Catalog with pages") {
        DocumentCatalog catalog;
        catalog.pages.resize(3);
        catalog.pages[0].page_number = 1;
        catalog.pages[1].page_number = 2;
        catalog.pages[2].page_number = 3;

        CHECK(catalog.pages.size() == 3);
        CHECK(catalog.pages[0].page_number == 1);
        CHECK(catalog.pages[2].page_number == 3);
    }

    TEST_CASE("Catalog with outline") {
        DocumentCatalog catalog;
        catalog.outline_root = std::unique_ptr<OutlineItem>(new OutlineItem());
        catalog.outline_root->title = "Contents";

        auto chapter = std::unique_ptr<OutlineItem>(new OutlineItem());
        chapter->title = "Chapter 1";
        catalog.outline_root->children.push_back(std::move(chapter));

        REQUIRE(catalog.outline_root != nullptr);
        CHECK(catalog.outline_root->title == "Contents");
        CHECK(catalog.outline_root->children.size() == 1);
        CHECK(catalog.outline_root->children[0]->title == "Chapter 1");
    }

    TEST_CASE("Catalog with page labels") {
        DocumentCatalog catalog;

        PageLabel label;
        label.style = PageLabelStyle::DecimalArabic;
        label.start_value = 1;
        catalog.page_labels.labels[0] = label;

        CHECK_FALSE(catalog.page_labels.labels.empty());
        CHECK(catalog.page_labels.labels[0].style == PageLabelStyle::DecimalArabic);
    }

    TEST_CASE("Catalog with named destinations") {
        DocumentCatalog catalog;

        NamedDestination intro;
        intro.name = "intro";
        intro.page_number = 1;
        catalog.named_destinations["intro"] = intro;

        NamedDestination chapter1;
        chapter1.name = "chapter1";
        chapter1.page_number = 5;
        catalog.named_destinations["chapter1"] = chapter1;

        CHECK(catalog.named_destinations.size() == 2);
        CHECK(catalog.named_destinations["intro"].page_number == 1);
        CHECK(catalog.named_destinations["chapter1"].page_number == 5);
    }

    TEST_CASE("Catalog with document info") {
        DocumentCatalog catalog;
        catalog.document_info.title = "Test PDF";
        catalog.document_info.author = "Test Author";
        catalog.document_info.subject = "Testing";

        CHECK(catalog.document_info.title == "Test PDF");
        CHECK(catalog.document_info.author == "Test Author");
        CHECK(catalog.document_info.subject == "Testing");
    }

    TEST_CASE("Catalog with XMP metadata") {
        DocumentCatalog catalog;
        catalog.xmp_metadata.dc_title = "Test PDF XMP";
        catalog.xmp_metadata.dc_creator = "XMP Author";

        CHECK(catalog.xmp_metadata.dc_title == "Test PDF XMP");
        CHECK(catalog.xmp_metadata.dc_creator == "XMP Author");
    }

    TEST_CASE("Complete catalog with all features") {
        DocumentCatalog catalog;

        // Add pages
        catalog.pages.resize(10);

        // Add outline
        catalog.outline_root = std::unique_ptr<OutlineItem>(new OutlineItem());
        catalog.outline_root->title = "Contents";

        // Add page labels
        PageLabel label;
        label.style = PageLabelStyle::DecimalArabic;
        catalog.page_labels.labels[0] = label;

        // Add named destination
        NamedDestination dest;
        dest.name = "intro";
        dest.page_number = 1;
        catalog.named_destinations["intro"] = dest;

        // Add document info
        catalog.document_info.title = "Complete PDF";
        catalog.document_info.author = "Test Author";

        // Add XMP metadata
        catalog.xmp_metadata.dc_title = "Complete PDF XMP";

        // Verify all features
        CHECK(catalog.pages.size() == 10);
        REQUIRE(catalog.outline_root != nullptr);
        CHECK(catalog.outline_root->title == "Contents");
        CHECK_FALSE(catalog.page_labels.labels.empty());
        CHECK(catalog.named_destinations.size() == 1);
        CHECK(catalog.named_destinations["intro"].page_number == 1);
        CHECK(catalog.document_info.title == "Complete PDF");
        CHECK(catalog.xmp_metadata.dc_title == "Complete PDF XMP");
    }

    TEST_CASE("Empty named destinations") {
        DocumentCatalog catalog;

        CHECK(catalog.named_destinations.empty());
    }

    TEST_CASE("Multiple named destinations") {
        DocumentCatalog catalog;

        for (int i = 0; i < 10; i++) {
            NamedDestination dest;
            dest.name = "dest" + std::to_string(i);
            dest.page_number = i;
            catalog.named_destinations[dest.name] = dest;
        }

        CHECK(catalog.named_destinations.size() == 10);
        CHECK(catalog.named_destinations["dest5"].page_number == 5);
    }

    TEST_CASE("Catalog page count") {
        DocumentCatalog catalog;
        catalog.pages.resize(100);

        CHECK(catalog.pages.size() == 100);
    }
}

int main() {
    return nanotest::run_all_tests();
}
