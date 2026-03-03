/**
 * Text annotation unit tests
 *
 * Tests TextAnnotation (sticky notes) which display a pop-up
 * window containing text. Common icons include Note, Comment,
 * Help, Insert, Key, NewParagraph, Paragraph.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("TextAnnotation") {
    TEST_CASE("Default values") {
        TextAnnotation annot;

        CHECK(annot.type == AnnotationType::Text);
        CHECK(annot.icon == "Note");
        CHECK_FALSE(annot.open);
    }

    TEST_CASE("Basic properties") {
        TextAnnotation annot;
        annot.contents = "This is a comment";
        annot.state = "Marked";
        annot.state_model = "Review";
        annot.open = true;

        CHECK(annot.contents == "This is a comment");
        CHECK(annot.state == "Marked");
        CHECK(annot.state_model == "Review");
        CHECK(annot.open);
    }

    TEST_CASE("Rectangle bounds") {
        TextAnnotation annot;
        annot.rect = {100, 200, 150, 230};

        REQUIRE(annot.rect.size() == 4);
        CHECK(annot.rect[0] == 100);  // llx
        CHECK(annot.rect[1] == 200);  // lly
        CHECK(annot.rect[2] == 150);  // urx
        CHECK(annot.rect[3] == 230);  // ury
    }

    TEST_CASE("Icon types") {
        TextAnnotation note;
        note.icon = "Note";
        CHECK(note.icon == "Note");

        TextAnnotation comment;
        comment.icon = "Comment";
        CHECK(comment.icon == "Comment");

        TextAnnotation help;
        help.icon = "Help";
        CHECK(help.icon == "Help");

        TextAnnotation key;
        key.icon = "Key";
        CHECK(key.icon == "Key");
    }

    TEST_CASE("State model - Review") {
        TextAnnotation annot;
        annot.state_model = "Review";
        annot.state = "Accepted";

        CHECK(annot.state_model == "Review");
        CHECK(annot.state == "Accepted");
    }

    TEST_CASE("State model - Marked") {
        TextAnnotation annot;
        annot.state_model = "Marked";
        annot.state = "Marked";

        CHECK(annot.state_model == "Marked");
        CHECK(annot.state == "Marked");
    }

    TEST_CASE("Open vs closed state") {
        TextAnnotation annot;

        annot.open = false;
        CHECK_FALSE(annot.open);

        annot.open = true;
        CHECK(annot.open);
    }

    TEST_CASE("Annotation name") {
        TextAnnotation annot;
        annot.contents = "Please review";
        annot.name = "Comment1";

        CHECK(annot.name == "Comment1");
        CHECK(annot.contents == "Please review");
    }

    TEST_CASE("Color specification") {
        TextAnnotation annot;
        annot.color = {1.0, 1.0, 0.0};  // Yellow

        REQUIRE(annot.color.size() == 3);
        CHECK(annot.color[0] == 1.0);  // Red
        CHECK(annot.color[1] == 1.0);  // Green
        CHECK(annot.color[2] == 0.0);  // Blue
    }

    TEST_CASE("Empty contents") {
        TextAnnotation annot;
        annot.contents = "";

        CHECK(annot.contents.empty());
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
