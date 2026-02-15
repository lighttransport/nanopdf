/**
 * Document outline (bookmarks) unit tests
 *
 * Tests OutlineItem structures which provide hierarchical navigation
 * through PDF documents. Outlines can have actions (GoTo, URI, etc.),
 * visual formatting (bold, italic, color), and nested children.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("OutlineAction") {
    TEST_CASE("Action type enum values") {
        // Verify outline action enum matches PDF spec
        CHECK(static_cast<int>(OutlineAction::GoTo) == 0);
        CHECK(static_cast<int>(OutlineAction::GoToR) == 1);
        CHECK(static_cast<int>(OutlineAction::URI) == 2);
        CHECK(static_cast<int>(OutlineAction::Launch) == 3);
    }
}

TEST_SUITE("OutlineItem") {
    TEST_CASE("Default values") {
        OutlineItem item;

        CHECK(item.action_type == OutlineAction::GoTo);
        CHECK(item.dest_page == 0);
        CHECK(item.open == true);
        CHECK(item.count == 0);
        CHECK_FALSE(item.italic);
        CHECK_FALSE(item.bold);
    }

    TEST_CASE("Basic properties") {
        OutlineItem item;
        item.title = "Chapter 1";
        item.dest_page = 5;
        item.dest_position = {100, 500, 1.0};
        item.color = {1.0, 0.0, 0.0};  // Red
        item.italic = true;

        CHECK(item.title == "Chapter 1");
        CHECK(item.dest_page == 5);
        REQUIRE(item.dest_position.size() == 3);
        CHECK(item.dest_position[0] == 100);
        CHECK(item.dest_position[1] == 500);
        CHECK(item.dest_position[2] == 1.0);
        REQUIRE(item.color.size() == 3);
        CHECK(item.color[0] == 1.0);  // Red
        CHECK(item.italic);
    }

    TEST_CASE("Bold and italic formatting") {
        OutlineItem item;
        item.bold = true;
        item.italic = true;

        CHECK(item.bold);
        CHECK(item.italic);
    }

    TEST_CASE("Open and closed state") {
        OutlineItem item;

        item.open = true;
        CHECK(item.open);

        item.open = false;
        CHECK_FALSE(item.open);
    }

    TEST_CASE("Color specification") {
        OutlineItem item;

        // Black
        item.color = {0.0, 0.0, 0.0};
        CHECK(item.color[0] == 0.0);
        CHECK(item.color[1] == 0.0);
        CHECK(item.color[2] == 0.0);

        // Blue
        item.color = {0.0, 0.0, 1.0};
        CHECK(item.color[2] == 1.0);
    }

    TEST_CASE("GoTo action with destination") {
        OutlineItem item;
        item.action_type = OutlineAction::GoTo;
        item.dest_page = 10;
        item.dest_position = {72, 720, 0};

        CHECK(item.action_type == OutlineAction::GoTo);
        CHECK(item.dest_page == 10);
    }

    TEST_CASE("URI action") {
        OutlineItem item;
        item.action_type = OutlineAction::URI;
        item.uri = "https://example.com";

        CHECK(item.action_type == OutlineAction::URI);
        CHECK(item.uri == "https://example.com");
    }

    TEST_CASE("GoToR action (remote)") {
        OutlineItem item;
        item.action_type = OutlineAction::GoToR;
        item.file = "other.pdf";
        item.dest_page = 5;

        CHECK(item.action_type == OutlineAction::GoToR);
        CHECK(item.file == "other.pdf");
        CHECK(item.dest_page == 5);
    }

    TEST_CASE("Launch action") {
        OutlineItem item;
        item.action_type = OutlineAction::Launch;
        item.file = "app.exe";

        CHECK(item.action_type == OutlineAction::Launch);
        CHECK(item.file == "app.exe");
    }
}

TEST_SUITE("OutlineHierarchy") {
    TEST_CASE("Single level outline") {
        auto root = std::unique_ptr<OutlineItem>(new OutlineItem());
        root->title = "Table of Contents";

        auto chapter1 = std::unique_ptr<OutlineItem>(new OutlineItem());
        chapter1->title = "Chapter 1";
        chapter1->dest_page = 1;

        auto chapter2 = std::unique_ptr<OutlineItem>(new OutlineItem());
        chapter2->title = "Chapter 2";
        chapter2->dest_page = 10;

        root->children.push_back(std::move(chapter1));
        root->children.push_back(std::move(chapter2));

        REQUIRE(root->children.size() == 2);
        CHECK(root->children[0]->title == "Chapter 1");
        CHECK(root->children[0]->dest_page == 1);
        CHECK(root->children[1]->title == "Chapter 2");
        CHECK(root->children[1]->dest_page == 10);
    }

    TEST_CASE("Nested outline hierarchy") {
        auto root = std::unique_ptr<OutlineItem>(new OutlineItem());
        root->title = "Table of Contents";

        auto chapter1 = std::unique_ptr<OutlineItem>(new OutlineItem());
        chapter1->title = "Chapter 1";
        chapter1->dest_page = 1;

        // Add subsections to chapter 1
        auto section1_1 = std::unique_ptr<OutlineItem>(new OutlineItem());
        section1_1->title = "Section 1.1";
        section1_1->dest_page = 2;

        auto section1_2 = std::unique_ptr<OutlineItem>(new OutlineItem());
        section1_2->title = "Section 1.2";
        section1_2->dest_page = 5;

        chapter1->children.push_back(std::move(section1_1));
        chapter1->children.push_back(std::move(section1_2));
        root->children.push_back(std::move(chapter1));

        // Verify hierarchy
        REQUIRE(root->children.size() == 1);
        CHECK(root->children[0]->title == "Chapter 1");
        REQUIRE(root->children[0]->children.size() == 2);
        CHECK(root->children[0]->children[0]->title == "Section 1.1");
        CHECK(root->children[0]->children[0]->dest_page == 2);
        CHECK(root->children[0]->children[1]->title == "Section 1.2");
        CHECK(root->children[0]->children[1]->dest_page == 5);
    }

    TEST_CASE("Empty outline") {
        auto root = std::unique_ptr<OutlineItem>(new OutlineItem());
        root->title = "Empty TOC";

        CHECK(root->children.empty());
    }

    TEST_CASE("Count field for collapsed items") {
        OutlineItem item;
        item.title = "Collapsed Chapter";
        item.open = false;
        item.count = -5;  // Negative means collapsed with 5 children

        CHECK(item.count == -5);
        CHECK_FALSE(item.open);
    }

    TEST_CASE("Three-level hierarchy") {
        auto root = std::unique_ptr<OutlineItem>(new OutlineItem());

        auto chapter = std::unique_ptr<OutlineItem>(new OutlineItem());
        chapter->title = "Chapter";

        auto section = std::unique_ptr<OutlineItem>(new OutlineItem());
        section->title = "Section";

        auto subsection = std::unique_ptr<OutlineItem>(new OutlineItem());
        subsection->title = "Subsection";
        subsection->dest_page = 42;

        section->children.push_back(std::move(subsection));
        chapter->children.push_back(std::move(section));
        root->children.push_back(std::move(chapter));

        CHECK(root->children[0]->children[0]->children[0]->title == "Subsection");
        CHECK(root->children[0]->children[0]->children[0]->dest_page == 42);
    }
}

int main() {
    return nanotest::run_all_tests();
}
