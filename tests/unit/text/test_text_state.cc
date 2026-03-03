/**
 * TextState structure unit tests
 *
 * Tests the TextState structure used to track text positioning,
 * spacing, scaling, and rendering modes during content stream
 * processing.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("TextState") {
    TEST_CASE("Initial values") {
        TextState state;

        // Test default text matrix (identity matrix)
        CHECK(state.text_matrix[0] == 1.0);
        CHECK(state.text_matrix[1] == 0.0);
        CHECK(state.text_matrix[2] == 0.0);
        CHECK(state.text_matrix[3] == 1.0);
        CHECK(state.text_matrix[4] == 0.0);
        CHECK(state.text_matrix[5] == 0.0);

        // Test default spacing and scaling
        CHECK(state.char_spacing == 0.0);
        CHECK(state.word_spacing == 0.0);
        CHECK(state.horizontal_scaling == 100.0);

        // Test default rendering mode
        CHECK(state.render_mode == TextRenderingMode::Fill);
    }

    TEST_CASE("Reset function") {
        TextState state;

        // Modify state
        state.char_spacing = 5.0;
        state.word_spacing = 10.0;
        state.font_size = 12.0;
        state.current_text = "test";

        // Reset
        state.reset();

        // Verify reset
        CHECK(state.char_spacing == 0.0);
        CHECK(state.word_spacing == 0.0);
        CHECK(state.font_size == 0.0);
        CHECK(state.current_text.empty());
    }

    TEST_CASE("Text matrix operations") {
        TextState state;

        // Set translation
        state.text_matrix[4] = 100.0;
        state.text_matrix[5] = 200.0;

        CHECK(state.text_matrix[4] == 100.0);
        CHECK(state.text_matrix[5] == 200.0);
    }

    TEST_CASE("Character and word spacing") {
        TextState state;

        state.char_spacing = 2.5;
        state.word_spacing = 5.0;

        CHECK(state.char_spacing == 2.5);
        CHECK(state.word_spacing == 5.0);
    }

    TEST_CASE("Horizontal scaling") {
        TextState state;

        state.horizontal_scaling = 150.0;
        CHECK(state.horizontal_scaling == 150.0);

        state.horizontal_scaling = 50.0;
        CHECK(state.horizontal_scaling == 50.0);
    }

    TEST_CASE("Font size setting") {
        TextState state;

        state.font_size = 14.0;
        CHECK(state.font_size == 14.0);
    }
}

TEST_SUITE("TextRenderingMode") {
    TEST_CASE("Rendering mode enum values") {
        // Verify enum integer values match PDF spec
        CHECK(static_cast<int>(TextRenderingMode::Fill) == 0);
        CHECK(static_cast<int>(TextRenderingMode::Stroke) == 1);
        CHECK(static_cast<int>(TextRenderingMode::FillAndStroke) == 2);
        CHECK(static_cast<int>(TextRenderingMode::Invisible) == 3);
        CHECK(static_cast<int>(TextRenderingMode::FillAndClip) == 4);
        CHECK(static_cast<int>(TextRenderingMode::StrokeAndClip) == 5);
        CHECK(static_cast<int>(TextRenderingMode::FillStrokeAndClip) == 6);
        CHECK(static_cast<int>(TextRenderingMode::Clip) == 7);
    }

    TEST_CASE("Setting rendering mode") {
        TextState state;

        state.render_mode = TextRenderingMode::Stroke;
        CHECK(state.render_mode == TextRenderingMode::Stroke);

        state.render_mode = TextRenderingMode::Invisible;
        CHECK(state.render_mode == TextRenderingMode::Invisible);

        state.render_mode = TextRenderingMode::FillAndClip;
        CHECK(state.render_mode == TextRenderingMode::FillAndClip);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
