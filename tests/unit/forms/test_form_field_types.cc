/**
 * Form field types and enums unit tests
 *
 * Tests the basic form field type enumerations and flags used
 * across all interactive form field types (text, button, choice).
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("FieldType") {
    TEST_CASE("Field type enum values") {
        // Verify field type enum matches PDF spec
        CHECK(static_cast<int>(FieldType::Button) == 0);
        CHECK(static_cast<int>(FieldType::Text) == 1);
        CHECK(static_cast<int>(FieldType::Choice) == 2);
        CHECK(static_cast<int>(FieldType::Signature) == 3);
    }
}

TEST_SUITE("FormFieldFlags") {
    TEST_CASE("Common field flags") {
        // Flags common to all field types
        CHECK(static_cast<uint32_t>(FormFieldFlags::ReadOnly) == 0x00000001);
        CHECK(static_cast<uint32_t>(FormFieldFlags::Required) == 0x00000002);
        CHECK(static_cast<uint32_t>(FormFieldFlags::NoExport) == 0x00000004);
    }

    TEST_CASE("Text field specific flags") {
        CHECK(static_cast<uint32_t>(FormFieldFlags::Multiline) == 0x00001000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::Password) == 0x00002000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::FileSelect) == 0x00100000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::DoNotSpellCheck) == 0x00400000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::DoNotScroll) == 0x00800000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::Comb) == 0x01000000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::RichText) == 0x02000000);
    }

    TEST_CASE("Button field specific flags") {
        CHECK(static_cast<uint32_t>(FormFieldFlags::NoToggleToOff) == 0x00004000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::Radio) == 0x00008000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::Pushbutton) == 0x00010000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::RadiosInUnison) == 0x02000000);
    }

    TEST_CASE("Choice field specific flags") {
        CHECK(static_cast<uint32_t>(FormFieldFlags::Combo) == 0x00020000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::Edit) == 0x00040000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::Sort) == 0x00080000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::MultiSelect) == 0x00200000);
        CHECK(static_cast<uint32_t>(FormFieldFlags::CommitOnSelChange) == 0x04000000);
    }

    TEST_CASE("Combined flags") {
        // Test combining multiple flags
        uint32_t flags = static_cast<uint32_t>(FormFieldFlags::Required) |
                        static_cast<uint32_t>(FormFieldFlags::ReadOnly);

        CHECK(flags == 0x00000003);
        CHECK((flags & static_cast<uint32_t>(FormFieldFlags::Required)) != 0);
        CHECK((flags & static_cast<uint32_t>(FormFieldFlags::ReadOnly)) != 0);
        CHECK((flags & static_cast<uint32_t>(FormFieldFlags::NoExport)) == 0);
    }

    TEST_CASE("Multiline and Password flags") {
        uint32_t flags = static_cast<uint32_t>(FormFieldFlags::Multiline) |
                        static_cast<uint32_t>(FormFieldFlags::Password);

        CHECK(flags == 0x00003000);
        CHECK((flags & static_cast<uint32_t>(FormFieldFlags::Multiline)) != 0);
        CHECK((flags & static_cast<uint32_t>(FormFieldFlags::Password)) != 0);
    }

    TEST_CASE("Radio button flags") {
        uint32_t flags = static_cast<uint32_t>(FormFieldFlags::Radio) |
                        static_cast<uint32_t>(FormFieldFlags::NoToggleToOff);

        CHECK((flags & static_cast<uint32_t>(FormFieldFlags::Radio)) != 0);
        CHECK((flags & static_cast<uint32_t>(FormFieldFlags::NoToggleToOff)) != 0);
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
