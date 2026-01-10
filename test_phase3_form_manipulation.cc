// Copyright 2026 nanopdf Authors
// SPDX-License-Identifier: MIT
//
// Test Phase 3.1: Form Field Value Manipulation

#include "src/nanopdf.hh"

#include <cassert>
#include <iostream>
#include <memory>

using namespace nanopdf;

void test_text_field_manipulation() {
  std::cout << "Test: Text field value manipulation\n";

  // Create a text field
  auto field = std::make_unique<TextField>();
  field->partial_name = "Name";
  field->full_name = "Name";
  field->max_length = 50;

  // Test setting valid value
  bool success = set_text_field_value(field.get(), "John Doe");
  assert(success);
  assert(field->field_value.type == Value::STRING);
  assert(field->field_value.str == "John Doe");
  std::cout << "  ✓ Set text field value\n";

  // Test max length validation
  std::string long_string(100, 'x');
  success = set_text_field_value(field.get(), long_string);
  assert(!success);  // Should fail due to max length
  std::cout << "  ✓ Max length validation working\n";

  // Test read-only field
  field->flags = static_cast<uint32_t>(FormFieldFlags::ReadOnly);
  success = set_text_field_value(field.get(), "New Value");
  assert(!success);  // Should fail due to read-only
  std::cout << "  ✓ Read-only protection working\n";
}

void test_button_field_manipulation() {
  std::cout << "\nTest: Button field value manipulation\n";

  // Create a checkbox
  auto checkbox = std::make_unique<ButtonField>();
  checkbox->partial_name = "Accept";
  checkbox->full_name = "Accept";
  checkbox->button_type = ButtonField::CheckBox;

  // Test checking checkbox
  bool success = set_button_field_checked(checkbox.get(), true);
  assert(success);
  assert(checkbox->field_value.type == Value::NAME);
  assert(checkbox->field_value.str == "/Yes");
  std::cout << "  ✓ Checkbox checked\n";

  // Test unchecking checkbox
  success = set_button_field_checked(checkbox.get(), false);
  assert(success);
  assert(checkbox->field_value.str == "/Off");
  std::cout << "  ✓ Checkbox unchecked\n";

  // Test push button (should fail)
  auto button = std::make_unique<ButtonField>();
  button->button_type = ButtonField::PushButton;
  success = set_button_field_checked(button.get(), true);
  assert(!success);  // Push buttons don't have checked state
  std::cout << "  ✓ Push button correctly rejects checked state\n";
}

void test_choice_field_manipulation() {
  std::cout << "\nTest: Choice field value manipulation\n";

  // Create a choice field (listbox)
  auto listbox = std::make_unique<ChoiceField>();
  listbox->partial_name = "Country";
  listbox->full_name = "Country";
  listbox->options = {"USA", "Canada", "Mexico", "UK"};

  // Test single selection
  bool success = set_choice_field_selection(listbox.get(), {1});  // Select "Canada"
  assert(success);
  assert(listbox->field_value.type == Value::STRING);
  assert(listbox->field_value.str == "Canada");
  std::cout << "  ✓ Single selection working\n";

  // Test multi-select without flag (should fail)
  success = set_choice_field_selection(listbox.get(), {0, 2});
  assert(!success);
  std::cout << "  ✓ Multi-select correctly rejected without flag\n";

  // Enable multi-select
  listbox->flags = static_cast<uint32_t>(FormFieldFlags::MultiSelect);

  // Test multi-select with flag
  success = set_choice_field_selection(listbox.get(), {0, 2});
  assert(success);
  assert(listbox->field_value.type == Value::ARRAY);
  assert(listbox->field_value.array.size() == 2);
  assert(listbox->field_value.array[0].str == "USA");
  assert(listbox->field_value.array[1].str == "Mexico");
  std::cout << "  ✓ Multi-select working\n";

  // Test invalid index
  success = set_choice_field_selection(listbox.get(), {10});
  assert(!success);
  std::cout << "  ✓ Invalid index correctly rejected\n";
}

void test_field_validation() {
  std::cout << "\nTest: Field validation\n";

  // Test required field
  auto field = std::make_unique<TextField>();
  field->flags = static_cast<uint32_t>(FormFieldFlags::Required);

  // Empty value should fail
  Value empty_value;
  bool valid = validate_field_value(field.get(), empty_value);
  assert(!valid);
  std::cout << "  ✓ Required field validation working\n";

  // Valid value should pass
  Value valid_value;
  valid_value.SetType(Value::STRING);
  valid_value.str = "Test";
  valid = validate_field_value(field.get(), valid_value);
  assert(valid);
  std::cout << "  ✓ Valid value accepted\n";

  // Test max length validation
  field->max_length = 5;
  Value long_value;
  long_value.SetType(Value::STRING);
  long_value.str = "Too Long String";
  valid = validate_field_value(field.get(), long_value);
  assert(!valid);
  std::cout << "  ✓ Max length validation working\n";
}

void test_field_export_value() {
  std::cout << "\nTest: Field export value\n";

  // Test string value
  auto field = std::make_unique<TextField>();
  field->field_value.SetType(Value::STRING);
  field->field_value.str = "Test Value";
  std::string export_val = get_field_export_value(field.get());
  assert(export_val == "Test Value");
  std::cout << "  ✓ String export working\n";

  // Test NoExport flag
  field->flags = static_cast<uint32_t>(FormFieldFlags::NoExport);
  export_val = get_field_export_value(field.get());
  assert(export_val.empty());
  std::cout << "  ✓ NoExport flag respected\n";

  // Test mapping name
  field->flags = 0;
  field->mapping_name = "mapped_name";
  export_val = get_field_export_value(field.get());
  assert(export_val == "mapped_name");
  std::cout << "  ✓ Mapping name used for export\n";
}

void test_find_field_by_name() {
  std::cout << "\nTest: Find field by name\n";

  // Create a mock catalog with fields
  DocumentCatalog catalog;

  auto field1 = std::make_unique<TextField>();
  field1->partial_name = "FirstName";
  field1->full_name = "FirstName";

  auto field2 = std::make_unique<TextField>();
  field2->partial_name = "LastName";
  field2->full_name = "LastName";

  // Add child field
  auto child = std::make_unique<TextField>();
  child->partial_name = "Middle";
  child->full_name = "Name.Middle";
  field1->children.push_back(std::move(child));

  catalog.form_fields.push_back(std::move(field1));
  catalog.form_fields.push_back(std::move(field2));

  // Test finding root field
  FormField* found = find_field_by_name(catalog, "FirstName");
  assert(found != nullptr);
  assert(found->partial_name == "FirstName");
  std::cout << "  ✓ Found root field\n";

  // Test finding child field
  found = find_field_by_name(catalog, "Name.Middle");
  assert(found != nullptr);
  assert(found->full_name == "Name.Middle");
  std::cout << "  ✓ Found child field\n";

  // Test not found
  found = find_field_by_name(catalog, "NonExistent");
  assert(found == nullptr);
  std::cout << "  ✓ Correctly returns null for non-existent field\n";
}

void test_fdf_export() {
  std::cout << "\nTest: FDF export\n";

  // Create a PDF with form fields
  Pdf pdf;

  auto field1 = std::make_unique<TextField>();
  field1->full_name = "Name";
  field1->field_value.SetType(Value::STRING);
  field1->field_value.str = "John Doe";

  auto field2 = std::make_unique<ButtonField>();
  field2->full_name = "Accept";
  field2->button_type = ButtonField::CheckBox;
  field2->field_value.SetType(Value::NAME);
  field2->field_value.str = "/Yes";

  pdf.catalog.form_fields.push_back(std::move(field1));
  pdf.catalog.form_fields.push_back(std::move(field2));

  // Export to FDF string
  std::string fdf = export_form_data_fdf_string(pdf);

  // Check FDF contains expected content
  assert(fdf.find("%FDF-1.2") != std::string::npos);
  assert(fdf.find("/Fields") != std::string::npos);
  assert(fdf.find("Name") != std::string::npos);
  assert(fdf.find("John Doe") != std::string::npos);
  assert(fdf.find("Accept") != std::string::npos);

  std::cout << "  ✓ FDF export working\n";
  std::cout << "  FDF output (first 200 chars):\n";
  std::cout << fdf.substr(0, std::min(size_t(200), fdf.length())) << "...\n";
}

void test_fdf_import() {
  std::cout << "\nTest: FDF import\n";

  // Create a PDF with empty form fields
  Pdf pdf;

  auto field1 = std::make_unique<TextField>();
  field1->full_name = "Name";
  field1->partial_name = "Name";

  auto field2 = std::make_unique<ButtonField>();
  field2->full_name = "Accept";
  field2->partial_name = "Accept";
  field2->button_type = ButtonField::CheckBox;

  pdf.catalog.form_fields.push_back(std::move(field1));
  pdf.catalog.form_fields.push_back(std::move(field2));

  // Create simple FDF data
  std::string fdf_data = R"(%FDF-1.2
1 0 obj
<<
/FDF <<
/Fields [
<<
/T (Name)
/V (Test User)
>>
<<
/T (Accept)
/V (Yes)
>>
]
>>
>>
endobj
trailer
<<
/Root 1 0 R
>>
%%EOF
)";

  // Import FDF data
  bool success = import_form_data_fdf_string(pdf, fdf_data);
  assert(success);

  // Verify field values were set
  FormField* name_field = find_field_by_name(pdf.catalog, "Name");
  assert(name_field != nullptr);
  assert(name_field->field_value.type == Value::STRING);
  assert(name_field->field_value.str == "Test User");

  FormField* accept_field = find_field_by_name(pdf.catalog, "Accept");
  assert(accept_field != nullptr);
  assert(accept_field->field_value.type == Value::NAME);
  assert(accept_field->field_value.str == "/Yes");

  std::cout << "  ✓ FDF import working\n";
}

int main() {
  std::cout << "=== Phase 3.1: Form Field Manipulation Tests ===\n\n";

  try {
    test_text_field_manipulation();
    test_button_field_manipulation();
    test_choice_field_manipulation();
    test_field_validation();
    test_field_export_value();
    test_find_field_by_name();
    test_fdf_export();
    test_fdf_import();

    std::cout << "\n=== All form manipulation tests passed! ===\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "\n!!! Test failed with exception: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "\n!!! Test failed with unknown exception\n";
    return 1;
  }
}
