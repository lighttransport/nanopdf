/**
 * ChoiceField unit tests
 *
 * Tests ChoiceField which represents dropdown lists and list boxes
 * in PDF forms. Choice fields support single and multiple selection,
 * editable combos, and sorting.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("ChoiceField") {
    TEST_CASE("Default values") {
        ChoiceField choice;

        CHECK(choice.type == FieldType::Choice);
        CHECK(choice.options.empty());
        CHECK(choice.selected_indices.empty());
    }

    TEST_CASE("Basic dropdown") {
        ChoiceField choice;
        choice.options.push_back("Option 1");
        choice.options.push_back("Option 2");
        choice.options.push_back("Option 3");
        choice.selected_indices.push_back(1);

        REQUIRE(choice.options.size() == 3);
        CHECK(choice.options[0] == "Option 1");
        CHECK(choice.options[1] == "Option 2");
        CHECK(choice.options[2] == "Option 3");
        REQUIRE(choice.selected_indices.size() == 1);
        CHECK(choice.selected_indices[0] == 1);
    }

    TEST_CASE("List box") {
        ChoiceField choice;
        choice.options.push_back("Item A");
        choice.options.push_back("Item B");
        choice.options.push_back("Item C");

        CHECK(choice.options.size() == 3);
    }

    TEST_CASE("Single selection") {
        ChoiceField choice;
        choice.options = {"Red", "Green", "Blue"};
        choice.selected_indices.push_back(2);  // Blue selected

        CHECK(choice.selected_indices.size() == 1);
        CHECK(choice.selected_indices[0] == 2);
    }

    TEST_CASE("Multiple selection") {
        ChoiceField choice;
        choice.flags = static_cast<uint32_t>(FormFieldFlags::MultiSelect);
        choice.options = {"Apple", "Banana", "Cherry", "Date"};
        choice.selected_indices.push_back(0);  // Apple
        choice.selected_indices.push_back(2);  // Cherry

        CHECK((choice.flags & static_cast<uint32_t>(FormFieldFlags::MultiSelect)) != 0);
        REQUIRE(choice.selected_indices.size() == 2);
        CHECK(choice.selected_indices[0] == 0);
        CHECK(choice.selected_indices[1] == 2);
    }

    TEST_CASE("Combo box (dropdown)") {
        ChoiceField choice;
        choice.flags = static_cast<uint32_t>(FormFieldFlags::Combo);
        choice.options = {"Small", "Medium", "Large"};

        CHECK((choice.flags & static_cast<uint32_t>(FormFieldFlags::Combo)) != 0);
    }

    TEST_CASE("Editable combo box") {
        ChoiceField choice;
        choice.flags = static_cast<uint32_t>(FormFieldFlags::Combo) |
                       static_cast<uint32_t>(FormFieldFlags::Edit);
        choice.options = {"Preset 1", "Preset 2"};

        CHECK((choice.flags & static_cast<uint32_t>(FormFieldFlags::Combo)) != 0);
        CHECK((choice.flags & static_cast<uint32_t>(FormFieldFlags::Edit)) != 0);
    }

    TEST_CASE("Sorted options") {
        ChoiceField choice;
        choice.flags = static_cast<uint32_t>(FormFieldFlags::Sort);
        choice.options = {"Zebra", "Apple", "Mango"};

        CHECK((choice.flags & static_cast<uint32_t>(FormFieldFlags::Sort)) != 0);
    }

    TEST_CASE("No selection") {
        ChoiceField choice;
        choice.options = {"Option 1", "Option 2"};

        CHECK(choice.selected_indices.empty());
    }

    TEST_CASE("Partial and full names") {
        ChoiceField choice;
        choice.partial_name = "Country";
        choice.full_name = "Form.Country";
        choice.options = {"USA", "Canada", "Mexico"};

        CHECK(choice.partial_name == "Country");
        CHECK(choice.full_name == "Form.Country");
    }

    TEST_CASE("Commit on selection change") {
        ChoiceField choice;
        choice.flags = static_cast<uint32_t>(FormFieldFlags::CommitOnSelChange);

        CHECK((choice.flags & static_cast<uint32_t>(FormFieldFlags::CommitOnSelChange)) != 0);
    }

    TEST_CASE("Required field") {
        ChoiceField choice;
        choice.flags = static_cast<uint32_t>(FormFieldFlags::Required);
        choice.options = {"Must", "Select", "One"};

        CHECK((choice.flags & static_cast<uint32_t>(FormFieldFlags::Required)) != 0);
    }

    TEST_CASE("Read-only field") {
        ChoiceField choice;
        choice.flags = static_cast<uint32_t>(FormFieldFlags::ReadOnly);
        choice.options = {"Locked", "Selection"};
        choice.selected_indices.push_back(0);

        CHECK((choice.flags & static_cast<uint32_t>(FormFieldFlags::ReadOnly)) != 0);
    }

    TEST_CASE("Field value") {
        ChoiceField choice;
        choice.options = {"Value 1", "Value 2"};
        choice.selected_indices.push_back(1);
        choice.field_value.SetType(Value::STRING);
        choice.field_value.str = "Value 2";

        CHECK(choice.field_value.type == Value::STRING);
        CHECK(choice.field_value.str == "Value 2");
    }
}

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
