/**
 * TextField unit tests
 *
 * Tests TextField which represents text input fields in PDF forms.
 * Text fields support single-line and multi-line input, password
 * masking, maximum length, and text alignment (quadding).
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("TextField") {
    TEST_CASE("Default values") {
        TextField field;

        CHECK(field.type == FieldType::Text);
        CHECK(field.max_length == 0);
        CHECK(field.quadding == 0);  // Left aligned
    }

    TEST_CASE("Basic properties") {
        TextField field;
        field.partial_name = "Name";
        field.full_name = "Form.Name";
        field.max_length = 50;
        field.default_appearance = "/Helv 10 Tf";
        field.quadding = 1;  // Center

        CHECK(field.partial_name == "Name");
        CHECK(field.full_name == "Form.Name");
        CHECK(field.max_length == 50);
        CHECK(field.default_appearance == "/Helv 10 Tf");
        CHECK(field.quadding == 1);
    }

    TEST_CASE("Single-line text field") {
        TextField field;
        field.partial_name = "FirstName";
        field.max_length = 30;

        // No Multiline flag
        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::Multiline)) == 0);
    }

    TEST_CASE("Multi-line text field") {
        TextField field;
        field.partial_name = "Comments";
        field.flags = static_cast<uint32_t>(FormFieldFlags::Multiline);

        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::Multiline)) != 0);
    }

    TEST_CASE("Password field") {
        TextField field;
        field.partial_name = "Password";
        field.flags = static_cast<uint32_t>(FormFieldFlags::Password);

        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::Password)) != 0);
    }

    TEST_CASE("Maximum length constraint") {
        TextField field;
        field.max_length = 10;

        CHECK(field.max_length == 10);
    }

    TEST_CASE("No maximum length") {
        TextField field;
        field.max_length = 0;  // Unlimited

        CHECK(field.max_length == 0);
    }

    TEST_CASE("Text alignment - Left") {
        TextField field;
        field.quadding = 0;

        CHECK(field.quadding == 0);
    }

    TEST_CASE("Text alignment - Center") {
        TextField field;
        field.quadding = 1;

        CHECK(field.quadding == 1);
    }

    TEST_CASE("Text alignment - Right") {
        TextField field;
        field.quadding = 2;

        CHECK(field.quadding == 2);
    }

    TEST_CASE("Default appearance") {
        TextField field;
        field.default_appearance = "/Helv 12 Tf 0 g";

        CHECK(field.default_appearance == "/Helv 12 Tf 0 g");
    }

    TEST_CASE("Field value") {
        TextField field;
        field.field_value.SetType(Value::STRING);
        field.field_value.str = "John Doe";

        CHECK(field.field_value.type == Value::STRING);
        CHECK(field.field_value.str == "John Doe");
    }

    TEST_CASE("Default value") {
        TextField field;
        field.default_value.SetType(Value::STRING);
        field.default_value.str = "Default text";

        CHECK(field.default_value.type == Value::STRING);
        CHECK(field.default_value.str == "Default text");
    }

    TEST_CASE("Read-only field") {
        TextField field;
        field.flags = static_cast<uint32_t>(FormFieldFlags::ReadOnly);

        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::ReadOnly)) != 0);
    }

    TEST_CASE("Required field") {
        TextField field;
        field.flags = static_cast<uint32_t>(FormFieldFlags::Required);

        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::Required)) != 0);
    }

    TEST_CASE("Comb field") {
        TextField field;
        field.flags = static_cast<uint32_t>(FormFieldFlags::Comb);
        field.max_length = 10;

        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::Comb)) != 0);
        CHECK(field.max_length == 10);
    }

    TEST_CASE("File select field") {
        TextField field;
        field.flags = static_cast<uint32_t>(FormFieldFlags::FileSelect);

        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::FileSelect)) != 0);
    }

    TEST_CASE("Do not spell check") {
        TextField field;
        field.flags = static_cast<uint32_t>(FormFieldFlags::DoNotSpellCheck);

        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::DoNotSpellCheck)) != 0);
    }

    TEST_CASE("Do not scroll") {
        TextField field;
        field.flags = static_cast<uint32_t>(FormFieldFlags::DoNotScroll);

        CHECK((field.flags & static_cast<uint32_t>(FormFieldFlags::DoNotScroll)) != 0);
    }
}

int main() {
    return nanotest::run_all_tests();
}
