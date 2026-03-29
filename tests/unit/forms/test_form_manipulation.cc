// Form field manipulation unit tests
#include "nanotest.hh"
#include "nanopdf.hh"

#include <memory>
#include <string>

using namespace nanopdf;

TEST_SUITE("FormManipulation") {

TEST_CASE("Set text field value") {
  auto field = std::make_unique<TextField>();
  field->partial_name = "Name";
  field->full_name = "Name";
  field->max_length = 50;

  bool success = set_text_field_value(field.get(), "John Doe");
  REQUIRE(success);
  CHECK_EQ(field->field_value.type, Value::STRING);
  CHECK_EQ(field->field_value.str, std::string("John Doe"));
}

TEST_CASE("Max length validation") {
  auto field = std::make_unique<TextField>();
  field->max_length = 50;

  std::string long_string(100, 'x');
  bool success = set_text_field_value(field.get(), long_string);
  CHECK_FALSE(success);
}

TEST_CASE("Read-only field protection") {
  auto field = std::make_unique<TextField>();
  field->flags = static_cast<uint32_t>(FormFieldFlags::ReadOnly);

  bool success = set_text_field_value(field.get(), "New Value");
  CHECK_FALSE(success);
}

TEST_CASE("Checkbox toggle") {
  auto checkbox = std::make_unique<ButtonField>();
  checkbox->partial_name = "Accept";
  checkbox->full_name = "Accept";
  checkbox->button_type = ButtonField::CheckBox;

  bool success = set_button_field_checked(checkbox.get(), true);
  REQUIRE(success);
  CHECK_EQ(checkbox->field_value.type, Value::NAME);
  CHECK_EQ(checkbox->field_value.str, std::string("/Yes"));

  success = set_button_field_checked(checkbox.get(), false);
  REQUIRE(success);
  CHECK_EQ(checkbox->field_value.str, std::string("/Off"));
}

TEST_CASE("Push button rejects checked state") {
  auto button = std::make_unique<ButtonField>();
  button->button_type = ButtonField::PushButton;
  CHECK_FALSE(set_button_field_checked(button.get(), true));
}

TEST_CASE("Choice field single selection") {
  auto listbox = std::make_unique<ChoiceField>();
  listbox->partial_name = "Country";
  listbox->full_name = "Country";
  listbox->options = {"USA", "Canada", "Mexico", "UK"};

  bool success = set_choice_field_selection(listbox.get(), {1});
  REQUIRE(success);
  CHECK_EQ(listbox->field_value.type, Value::STRING);
  CHECK_EQ(listbox->field_value.str, std::string("Canada"));
}

TEST_CASE("Multi-select rejected without flag") {
  auto listbox = std::make_unique<ChoiceField>();
  listbox->options = {"USA", "Canada", "Mexico"};

  CHECK_FALSE(set_choice_field_selection(listbox.get(), {0, 2}));
}

TEST_CASE("Multi-select with flag") {
  auto listbox = std::make_unique<ChoiceField>();
  listbox->options = {"USA", "Canada", "Mexico", "UK"};
  listbox->flags = static_cast<uint32_t>(FormFieldFlags::MultiSelect);

  bool success = set_choice_field_selection(listbox.get(), {0, 2});
  REQUIRE(success);
  CHECK_EQ(listbox->field_value.type, Value::ARRAY);
  CHECK_EQ(listbox->field_value.array.size(), size_t(2));
  CHECK_EQ(listbox->field_value.array[0].str, std::string("USA"));
  CHECK_EQ(listbox->field_value.array[1].str, std::string("Mexico"));
}

TEST_CASE("Invalid choice index rejected") {
  auto listbox = std::make_unique<ChoiceField>();
  listbox->options = {"A", "B"};

  CHECK_FALSE(set_choice_field_selection(listbox.get(), {10}));
}

TEST_CASE("Required field validation") {
  auto field = std::make_unique<TextField>();
  field->flags = static_cast<uint32_t>(FormFieldFlags::Required);

  Value empty_value;
  CHECK_FALSE(validate_field_value(field.get(), empty_value));

  Value valid_value;
  valid_value.SetType(Value::STRING);
  valid_value.str = "Test";
  CHECK(validate_field_value(field.get(), valid_value));
}

TEST_CASE("Max length field validation") {
  auto field = std::make_unique<TextField>();
  field->max_length = 5;

  Value long_value;
  long_value.SetType(Value::STRING);
  long_value.str = "Too Long String";
  CHECK_FALSE(validate_field_value(field.get(), long_value));
}

TEST_CASE("Field export value") {
  auto field = std::make_unique<TextField>();
  field->field_value.SetType(Value::STRING);
  field->field_value.str = "Test Value";
  CHECK_EQ(get_field_export_value(field.get()), std::string("Test Value"));
}

TEST_CASE("NoExport flag respected") {
  auto field = std::make_unique<TextField>();
  field->field_value.SetType(Value::STRING);
  field->field_value.str = "Secret";
  field->flags = static_cast<uint32_t>(FormFieldFlags::NoExport);
  CHECK(get_field_export_value(field.get()).empty());
}

TEST_CASE("Mapping name used for export") {
  auto field = std::make_unique<TextField>();
  field->field_value.SetType(Value::STRING);
  field->field_value.str = "Test";
  field->mapping_name = "mapped_name";
  CHECK_EQ(get_field_export_value(field.get()), std::string("mapped_name"));
}

TEST_CASE("Find field by name") {
  DocumentCatalog catalog;

  auto field1 = std::make_unique<TextField>();
  field1->partial_name = "FirstName";
  field1->full_name = "FirstName";

  auto field2 = std::make_unique<TextField>();
  field2->partial_name = "LastName";
  field2->full_name = "LastName";

  auto child = std::make_unique<TextField>();
  child->partial_name = "Middle";
  child->full_name = "Name.Middle";
  field1->children.push_back(std::move(child));

  catalog.form_fields.push_back(std::move(field1));
  catalog.form_fields.push_back(std::move(field2));

  FormField* found = find_field_by_name(catalog, "FirstName");
  REQUIRE(found != nullptr);
  CHECK_EQ(found->partial_name, std::string("FirstName"));

  found = find_field_by_name(catalog, "Name.Middle");
  REQUIRE(found != nullptr);
  CHECK_EQ(found->full_name, std::string("Name.Middle"));

  found = find_field_by_name(catalog, "NonExistent");
  CHECK(found == nullptr);
}

TEST_CASE("FDF export") {
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

  std::string fdf = export_form_data_fdf_string(pdf);
  CHECK(fdf.find("%FDF-1.2") != std::string::npos);
  CHECK(fdf.find("/Fields") != std::string::npos);
  CHECK(fdf.find("Name") != std::string::npos);
  CHECK(fdf.find("John Doe") != std::string::npos);
  CHECK(fdf.find("Accept") != std::string::npos);
}

TEST_CASE("FDF import") {
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

  bool success = import_form_data_fdf_string(pdf, fdf_data);
  REQUIRE(success);

  FormField* name_field = find_field_by_name(pdf.catalog, "Name");
  REQUIRE(name_field != nullptr);
  CHECK_EQ(name_field->field_value.type, Value::STRING);
  CHECK_EQ(name_field->field_value.str, std::string("Test User"));

  FormField* accept_field = find_field_by_name(pdf.catalog, "Accept");
  REQUIRE(accept_field != nullptr);
  CHECK_EQ(accept_field->field_value.type, Value::NAME);
  CHECK_EQ(accept_field->field_value.str, std::string("/Yes"));
}

} // TEST_SUITE
