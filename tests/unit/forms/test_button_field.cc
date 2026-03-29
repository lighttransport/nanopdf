/**
 * ButtonField unit tests
 *
 * Tests ButtonField which represents button form fields including
 * checkboxes, radio buttons, and push buttons. Button fields can
 * have different states (on/off) and appearance streams.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("ButtonField") {
    TEST_CASE("Default values") {
        ButtonField button;

        CHECK(button.type == FieldType::Button);
        CHECK(button.button_type == ButtonField::CheckBox);
    }

    TEST_CASE("CheckBox type") {
        ButtonField button;
        button.button_type == ButtonField::CheckBox;
        button.partial_name = "Agree";

        CHECK(button.button_type == ButtonField::CheckBox);
        CHECK(button.partial_name == "Agree");
    }

    TEST_CASE("RadioButton type") {
        ButtonField button;
        button.button_type = ButtonField::RadioButton;
        button.partial_name = "Option1";
        button.flags = static_cast<uint32_t>(FormFieldFlags::Radio);

        CHECK(button.button_type == ButtonField::RadioButton);
        CHECK(button.partial_name == "Option1");
        CHECK((button.flags & static_cast<uint32_t>(FormFieldFlags::Radio)) != 0);
    }

    TEST_CASE("PushButton type") {
        ButtonField button;
        button.button_type = ButtonField::PushButton;
        button.normal_caption = "Submit";
        button.rollover_caption = "Click to Submit";

        CHECK(button.button_type == ButtonField::PushButton);
        CHECK(button.normal_caption == "Submit");
        CHECK(button.rollover_caption == "Click to Submit");
    }

    TEST_CASE("CheckBox checked state") {
        ButtonField button;
        button.button_type = ButtonField::CheckBox;
        button.field_value.SetType(Value::NAME);
        button.field_value.str = "/Yes";

        CHECK(button.field_value.type == Value::NAME);
        CHECK(button.field_value.str == "/Yes");
    }

    TEST_CASE("CheckBox unchecked state") {
        ButtonField button;
        button.button_type = ButtonField::CheckBox;
        button.field_value.SetType(Value::NAME);
        button.field_value.str = "/Off";

        CHECK(button.field_value.str == "/Off");
    }

    TEST_CASE("RadioButton unselected") {
        ButtonField button;
        button.button_type = ButtonField::RadioButton;
        button.field_value.SetType(Value::NAME);
        button.field_value.str = "/Off";

        CHECK(button.field_value.str == "/Off");
    }

    TEST_CASE("PushButton captions") {
        ButtonField button;
        button.button_type = ButtonField::PushButton;
        button.normal_caption = "Click Me";
        button.rollover_caption = "Click Now";
        button.down_caption = "Clicking...";

        CHECK(button.normal_caption == "Click Me");
        CHECK(button.rollover_caption == "Click Now");
        CHECK(button.down_caption == "Clicking...");
    }

    TEST_CASE("NoToggleToOff flag") {
        ButtonField button;
        button.button_type = ButtonField::RadioButton;
        button.flags = static_cast<uint32_t>(FormFieldFlags::Radio) |
                       static_cast<uint32_t>(FormFieldFlags::NoToggleToOff);

        CHECK((button.flags & static_cast<uint32_t>(FormFieldFlags::NoToggleToOff)) != 0);
    }

    TEST_CASE("RadiosInUnison flag") {
        ButtonField button;
        button.button_type = ButtonField::RadioButton;
        button.flags = static_cast<uint32_t>(FormFieldFlags::Radio) |
                       static_cast<uint32_t>(FormFieldFlags::RadiosInUnison);

        CHECK((button.flags & static_cast<uint32_t>(FormFieldFlags::RadiosInUnison)) != 0);
    }

    TEST_CASE("PushButton flag") {
        ButtonField button;
        button.button_type = ButtonField::PushButton;
        button.flags = static_cast<uint32_t>(FormFieldFlags::Pushbutton);

        CHECK((button.flags & static_cast<uint32_t>(FormFieldFlags::Pushbutton)) != 0);
    }

    TEST_CASE("Field name") {
        ButtonField button;
        button.partial_name = "SubmitButton";
        button.full_name = "Form.SubmitButton";

        CHECK(button.partial_name == "SubmitButton");
        CHECK(button.full_name == "Form.SubmitButton");
    }

    TEST_CASE("Read-only button") {
        ButtonField button;
        button.flags = static_cast<uint32_t>(FormFieldFlags::ReadOnly);

        CHECK((button.flags & static_cast<uint32_t>(FormFieldFlags::ReadOnly)) != 0);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
