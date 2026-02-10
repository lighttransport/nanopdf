#include "nanopdf.hh"
#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace nanopdf;

// Test annotation types and structures
void test_annotation_types() {
  std::cout << "Testing Annotation types..." << std::endl;

  // Test annotation type enum values
  assert(static_cast<int>(AnnotationType::Text) == 0);
  assert(static_cast<int>(AnnotationType::Link) == 1);
  assert(static_cast<int>(AnnotationType::Highlight) == 8);

  // Test annotation flags
  assert(static_cast<uint32_t>(AnnotationFlags::Invisible) == 0x0001);
  assert(static_cast<uint32_t>(AnnotationFlags::Print) == 0x0004);
  assert(static_cast<uint32_t>(AnnotationFlags::ReadOnly) == 0x0040);

  std::cout << "  Annotation types: PASSED" << std::endl;
}

// Test text annotation
void test_text_annotation() {
  std::cout << "Testing TextAnnotation..." << std::endl;

  TextAnnotation text_annot;
  assert(text_annot.type == AnnotationType::Text);
  assert(text_annot.icon == "Note");
  assert(!text_annot.open);

  text_annot.contents = "This is a comment";
  text_annot.state = "Marked";
  text_annot.state_model = "Review";
  text_annot.open = true;

  assert(text_annot.contents == "This is a comment");
  assert(text_annot.state == "Marked");
  assert(text_annot.open);

  // Test rectangle
  text_annot.rect = {100, 200, 150, 230};
  assert(text_annot.rect.size() == 4);
  assert(text_annot.rect[0] == 100);
  assert(text_annot.rect[3] == 230);

  std::cout << "  TextAnnotation: PASSED" << std::endl;
}

// Test link annotation
void test_link_annotation() {
  std::cout << "Testing LinkAnnotation..." << std::endl;

  LinkAnnotation link;
  assert(link.type == AnnotationType::Link);
  assert(link.action_type == LinkAnnotation::GoTo);

  // Set URI action
  link.action_type = LinkAnnotation::URI;
  link.uri = "https://example.com";
  link.rect = {50, 50, 200, 70};

  assert(link.action_type == LinkAnnotation::URI);
  assert(link.uri == "https://example.com");
  assert(link.rect.size() == 4);

  std::cout << "  LinkAnnotation: PASSED" << std::endl;
}

// Test markup annotation
void test_markup_annotation() {
  std::cout << "Testing MarkupAnnotation..." << std::endl;

  MarkupAnnotation markup(AnnotationType::Highlight);
  assert(markup.type == AnnotationType::Highlight);
  assert(markup.opacity == 1.0);

  markup.opacity = 0.5;
  markup.title = "Reviewer";
  markup.subject = "Important";

  // Add quad points for text region
  std::vector<double> quad = {100, 100, 200, 100, 200, 120, 100, 120};
  markup.quad_points.push_back(quad);

  assert(markup.opacity == 0.5);
  assert(markup.title == "Reviewer");
  assert(markup.quad_points.size() == 1);
  assert(markup.quad_points[0].size() == 8);

  std::cout << "  MarkupAnnotation: PASSED" << std::endl;
}

// Test free text annotation
void test_freetext_annotation() {
  std::cout << "Testing FreeTextAnnotation..." << std::endl;

  FreeTextAnnotation freetext;
  assert(freetext.type == AnnotationType::FreeText);
  assert(freetext.quadding == 0);  // Left justified

  freetext.contents = "Sample text";
  freetext.default_appearance = "/Helv 12 Tf 0 g";
  freetext.quadding = 1;  // Center

  assert(freetext.contents == "Sample text");
  assert(freetext.quadding == 1);

  std::cout << "  FreeTextAnnotation: PASSED" << std::endl;
}

// Test annotation border
void test_annotation_border() {
  std::cout << "Testing AnnotationBorder..." << std::endl;

  AnnotationBorder border;
  assert(border.width == 1.0);
  assert(border.style == AnnotationBorder::Solid);

  border.width = 2.5;
  border.style = AnnotationBorder::Dashed;
  border.dash_pattern = {3, 2};

  assert(border.width == 2.5);
  assert(border.style == AnnotationBorder::Dashed);
  assert(border.dash_pattern.size() == 2);
  assert(border.dash_pattern[0] == 3);

  std::cout << "  AnnotationBorder: PASSED" << std::endl;
}

// Test form field types
void test_form_field_types() {
  std::cout << "Testing FormField types..." << std::endl;

  // Test field type enum
  assert(static_cast<int>(FieldType::Button) == 0);
  assert(static_cast<int>(FieldType::Text) == 1);
  assert(static_cast<int>(FieldType::Choice) == 2);
  assert(static_cast<int>(FieldType::Signature) == 3);

  // Test field flags
  assert(static_cast<uint32_t>(FormFieldFlags::ReadOnly) == 0x00000001);
  assert(static_cast<uint32_t>(FormFieldFlags::Required) == 0x00000002);
  assert(static_cast<uint32_t>(FormFieldFlags::Multiline) == 0x00001000);
  assert(static_cast<uint32_t>(FormFieldFlags::Password) == 0x00002000);

  std::cout << "  FormField types: PASSED" << std::endl;
}

// Test text field
void test_text_field() {
  std::cout << "Testing TextField..." << std::endl;

  TextField text_field;
  assert(text_field.type == FieldType::Text);
  assert(text_field.max_length == 0);
  assert(text_field.quadding == 0);

  text_field.partial_name = "Name";
  text_field.full_name = "Form.Name";
  text_field.max_length = 50;
  text_field.default_appearance = "/Helv 10 Tf";
  text_field.quadding = 1;  // Center

  assert(text_field.partial_name == "Name");
  assert(text_field.max_length == 50);
  assert(text_field.quadding == 1);

  std::cout << "  TextField: PASSED" << std::endl;
}

// Test button field
void test_button_field() {
  std::cout << "Testing ButtonField..." << std::endl;

  ButtonField button;
  assert(button.type == FieldType::Button);
  assert(button.button_type == ButtonField::CheckBox);

  button.button_type = ButtonField::RadioButton;
  button.partial_name = "Option1";
  button.flags = static_cast<uint32_t>(FormFieldFlags::Radio);

  assert(button.button_type == ButtonField::RadioButton);
  assert(button.partial_name == "Option1");

  // Test push button
  ButtonField push_button;
  push_button.button_type = ButtonField::PushButton;
  push_button.normal_caption = "Submit";
  push_button.rollover_caption = "Click to Submit";

  assert(push_button.button_type == ButtonField::PushButton);
  assert(push_button.normal_caption == "Submit");

  std::cout << "  ButtonField: PASSED" << std::endl;
}

// Test choice field
void test_choice_field() {
  std::cout << "Testing ChoiceField..." << std::endl;

  ChoiceField choice;
  assert(choice.type == FieldType::Choice);
  assert(choice.options.empty());

  choice.options.push_back("Option 1");
  choice.options.push_back("Option 2");
  choice.options.push_back("Option 3");
  choice.selected_indices.push_back(1);

  assert(choice.options.size() == 3);
  assert(choice.options[1] == "Option 2");
  assert(choice.selected_indices.size() == 1);
  assert(choice.selected_indices[0] == 1);

  std::cout << "  ChoiceField: PASSED" << std::endl;
}

// Test widget annotation
void test_widget_annotation() {
  std::cout << "Testing WidgetAnnotation..." << std::endl;

  WidgetAnnotation widget;
  assert(widget.type == AnnotationType::Widget);
  assert(widget.field_type == FieldType::Text);

  widget.field_type = FieldType::Button;
  widget.field_name = "CheckBox1";
  widget.field_value = "Yes";
  widget.default_value = "Off";

  assert(widget.field_type == FieldType::Button);
  assert(widget.field_name == "CheckBox1");
  assert(widget.field_value == "Yes");

  std::cout << "  WidgetAnnotation: PASSED" << std::endl;
}

// Test appearance generation
void test_appearance_generation() {
  std::cout << "Testing appearance generation..." << std::endl;

  // Test text annotation appearance
  TextAnnotation text_annot;
  text_annot.contents = "Test note";
  std::string appearance = generate_annotation_appearance(text_annot);
  assert(!appearance.empty());
  assert(appearance.find("BT") != std::string::npos);
  assert(appearance.find("Test note") != std::string::npos);

  // Test highlight appearance
  MarkupAnnotation highlight(AnnotationType::Highlight);
  highlight.color = {1.0, 1.0, 0.0};  // Yellow
  std::vector<double> quad = {10, 10, 100, 10, 100, 20, 10, 20};
  highlight.quad_points.push_back(quad);

  appearance = generate_annotation_appearance(highlight);
  assert(!appearance.empty());
  assert(appearance.find("1 1 0 rg") != std::string::npos);

  std::cout << "  Appearance generation: PASSED" << std::endl;
}

// Test page with annotations
void test_page_annotations() {
  std::cout << "Testing Page annotations..." << std::endl;

  Page page;
  page.object_number = 1;
  page.page_number = 1;

  // Add annotations
  auto text_annot = std::unique_ptr<TextAnnotation>(new TextAnnotation());
  text_annot->contents = "Page comment";
  page.annotations.push_back(std::move(text_annot));

  auto link_annot = std::unique_ptr<LinkAnnotation>(new LinkAnnotation());
  link_annot->uri = "https://example.org";
  page.annotations.push_back(std::move(link_annot));

  assert(page.annotations.size() == 2);
  assert(page.annotations[0]->type == AnnotationType::Text);
  assert(page.annotations[1]->type == AnnotationType::Link);

  std::cout << "  Page annotations: PASSED" << std::endl;
}

// Test set_text_field_value
void test_set_text_field_value() {
  std::cout << "Testing set_text_field_value..." << std::endl;

  // Basic set
  TextField field;
  field.full_name = "Name";
  assert(set_text_field_value(&field, "John Doe"));
  assert(field.field_value.type == Value::STRING);
  assert(field.field_value.str == "John Doe");
  std::cout << "  Basic set: PASSED" << std::endl;

  // Null pointer
  assert(!set_text_field_value(nullptr, "test"));
  std::cout << "  Null pointer: PASSED" << std::endl;

  // ReadOnly
  TextField ro_field;
  ro_field.flags = static_cast<uint32_t>(FormFieldFlags::ReadOnly);
  assert(!set_text_field_value(&ro_field, "test"));
  std::cout << "  ReadOnly: PASSED" << std::endl;

  // Max length exceeded
  TextField ml_field;
  ml_field.max_length = 5;
  assert(!set_text_field_value(&ml_field, "toolong"));
  assert(set_text_field_value(&ml_field, "ok"));
  assert(ml_field.field_value.str == "ok");
  std::cout << "  Max length: PASSED" << std::endl;

  std::cout << "  set_text_field_value: PASSED" << std::endl;
}

// Test set_button_field_checked
void test_set_button_field_checked() {
  std::cout << "Testing set_button_field_checked..." << std::endl;

  // Checkbox check/uncheck
  ButtonField cb;
  cb.button_type = ButtonField::CheckBox;
  assert(set_button_field_checked(&cb, true));
  assert(cb.field_value.type == Value::NAME);
  assert(cb.field_value.str == "/Yes");

  assert(set_button_field_checked(&cb, false));
  assert(cb.field_value.str == "/Off");
  std::cout << "  Checkbox: PASSED" << std::endl;

  // Radio button
  ButtonField radio;
  radio.button_type = ButtonField::RadioButton;
  assert(set_button_field_checked(&radio, true));
  assert(radio.field_value.str == "/Yes");
  std::cout << "  RadioButton: PASSED" << std::endl;

  // PushButton — should fail
  ButtonField push;
  push.button_type = ButtonField::PushButton;
  assert(!set_button_field_checked(&push, true));
  std::cout << "  PushButton rejected: PASSED" << std::endl;

  // Null pointer
  assert(!set_button_field_checked(nullptr, true));

  // ReadOnly
  ButtonField ro;
  ro.button_type = ButtonField::CheckBox;
  ro.flags = static_cast<uint32_t>(FormFieldFlags::ReadOnly);
  assert(!set_button_field_checked(&ro, true));
  std::cout << "  ReadOnly: PASSED" << std::endl;

  std::cout << "  set_button_field_checked: PASSED" << std::endl;
}

// Test set_choice_field_selection
void test_set_choice_field_selection() {
  std::cout << "Testing set_choice_field_selection..." << std::endl;

  ChoiceField field;
  field.options = {"Red", "Green", "Blue"};

  // Single selection
  assert(set_choice_field_selection(&field, {1}));
  assert(field.selected_indices.size() == 1);
  assert(field.selected_indices[0] == 1);
  assert(field.field_value.type == Value::STRING);
  assert(field.field_value.str == "Green");
  std::cout << "  Single select: PASSED" << std::endl;

  // Out-of-range index
  assert(!set_choice_field_selection(&field, {5}));
  std::cout << "  Invalid index: PASSED" << std::endl;

  // Multi-select without flag — should fail
  assert(!set_choice_field_selection(&field, {0, 2}));
  std::cout << "  Multi without flag: PASSED" << std::endl;

  // Multi-select with flag
  field.flags |= static_cast<uint32_t>(FormFieldFlags::MultiSelect);
  assert(set_choice_field_selection(&field, {0, 2}));
  assert(field.field_value.type == Value::ARRAY);
  assert(field.field_value.array.size() == 2);
  assert(field.field_value.array[0].str == "Red");
  assert(field.field_value.array[1].str == "Blue");
  std::cout << "  Multi select: PASSED" << std::endl;

  // Empty selection
  assert(set_choice_field_selection(&field, {}));
  assert(field.field_value.type == Value::NULL_OBJ);
  std::cout << "  Empty select: PASSED" << std::endl;

  // Null pointer
  assert(!set_choice_field_selection(nullptr, {0}));

  std::cout << "  set_choice_field_selection: PASSED" << std::endl;
}

// Test validate_field_value
void test_validate_field_value() {
  std::cout << "Testing validate_field_value..." << std::endl;

  // Text field validation
  TextField text_field;
  text_field.max_length = 10;
  Value str_val;
  str_val.SetType(Value::STRING);
  str_val.str = "hello";
  assert(validate_field_value(&text_field, str_val));

  // Too long
  str_val.str = "this string is way too long";
  assert(!validate_field_value(&text_field, str_val));
  std::cout << "  Text max length: PASSED" << std::endl;

  // Required field with empty value
  TextField req_field;
  req_field.flags = static_cast<uint32_t>(FormFieldFlags::Required);
  Value null_val;
  null_val.SetType(Value::NULL_OBJ);
  assert(!validate_field_value(&req_field, null_val));
  Value empty_str;
  empty_str.SetType(Value::STRING);
  empty_str.str = "";
  assert(!validate_field_value(&req_field, empty_str));
  std::cout << "  Required: PASSED" << std::endl;

  // Button field — should accept NAME type
  ButtonField btn;
  Value name_val;
  name_val.SetType(Value::NAME);
  name_val.str = "Yes";
  assert(validate_field_value(&btn, name_val));
  // Should reject STRING type
  assert(!validate_field_value(&btn, str_val));
  std::cout << "  Button type: PASSED" << std::endl;

  // Null pointer
  assert(!validate_field_value(nullptr, str_val));

  std::cout << "  validate_field_value: PASSED" << std::endl;
}

// Test get_field_export_value
void test_get_field_export_value() {
  std::cout << "Testing get_field_export_value..." << std::endl;

  // String value
  TextField field;
  field.field_value.SetType(Value::STRING);
  field.field_value.str = "test_value";
  assert(get_field_export_value(&field) == "test_value");
  std::cout << "  String export: PASSED" << std::endl;

  // Mapping name takes priority
  field.mapping_name = "mapped_name";
  assert(get_field_export_value(&field) == "mapped_name");
  std::cout << "  Mapping name: PASSED" << std::endl;

  // NoExport flag
  TextField no_export;
  no_export.flags = static_cast<uint32_t>(FormFieldFlags::NoExport);
  no_export.field_value.SetType(Value::STRING);
  no_export.field_value.str = "secret";
  assert(get_field_export_value(&no_export) == "");
  std::cout << "  NoExport: PASSED" << std::endl;

  // Null pointer
  assert(get_field_export_value(nullptr) == "");

  std::cout << "  get_field_export_value: PASSED" << std::endl;
}

// Test find_field_by_name
void test_find_field_by_name() {
  std::cout << "Testing find_field_by_name..." << std::endl;

  DocumentCatalog catalog;

  auto f1 = std::unique_ptr<TextField>(new TextField());
  f1->full_name = "Name";
  f1->partial_name = "name";

  auto f2 = std::unique_ptr<ButtonField>(new ButtonField());
  f2->full_name = "Submit";

  // Add child to f1
  auto child = std::unique_ptr<TextField>(new TextField());
  child->full_name = "Name.First";
  child->partial_name = "First";
  f1->children.push_back(std::move(child));

  catalog.form_fields.push_back(std::move(f1));
  catalog.form_fields.push_back(std::move(f2));

  // Find top-level by full name
  FormField* found = find_field_by_name(catalog, "Name");
  assert(found != nullptr);
  assert(found->full_name == "Name");
  std::cout << "  Find by full name: PASSED" << std::endl;

  // Find by partial name
  found = find_field_by_name(catalog, "name");
  assert(found != nullptr);
  std::cout << "  Find by partial name: PASSED" << std::endl;

  // Find nested child
  found = find_field_by_name(catalog, "Name.First");
  assert(found != nullptr);
  assert(found->full_name == "Name.First");
  std::cout << "  Find nested: PASSED" << std::endl;

  // Not found
  found = find_field_by_name(catalog, "NonExistent");
  assert(found == nullptr);
  std::cout << "  Not found: PASSED" << std::endl;

  std::cout << "  find_field_by_name: PASSED" << std::endl;
}

// Test FDF export and import round-trip
void test_fdf_round_trip() {
  std::cout << "Testing FDF export/import round-trip..." << std::endl;

  Pdf pdf;

  // Create form fields
  auto text_field = std::unique_ptr<TextField>(new TextField());
  text_field->full_name = "UserName";
  set_text_field_value(text_field.get(), "Alice");
  pdf.catalog.form_fields.push_back(std::move(text_field));

  auto check_field = std::unique_ptr<ButtonField>(new ButtonField());
  check_field->full_name = "Agree";
  check_field->button_type = ButtonField::CheckBox;
  set_button_field_checked(check_field.get(), true);
  pdf.catalog.form_fields.push_back(std::move(check_field));

  // Export to FDF string
  std::string fdf = export_form_data_fdf_string(pdf);
  assert(!fdf.empty());
  assert(fdf.find("%FDF") != std::string::npos);
  assert(fdf.find("UserName") != std::string::npos);
  assert(fdf.find("Alice") != std::string::npos);
  std::cout << "  FDF export: PASSED" << std::endl;

  // Import into a new Pdf with matching fields
  Pdf pdf2;
  auto tf2 = std::unique_ptr<TextField>(new TextField());
  tf2->full_name = "UserName";
  pdf2.catalog.form_fields.push_back(std::move(tf2));

  assert(import_form_data_fdf_string(pdf2, fdf));

  FormField* imported = find_field_by_name(pdf2.catalog, "UserName");
  assert(imported != nullptr);
  assert(imported->field_value.type == Value::STRING);
  assert(imported->field_value.str == "Alice");
  std::cout << "  FDF import: PASSED" << std::endl;

  std::cout << "  FDF round-trip: PASSED" << std::endl;
}

// Test document catalog with forms
void test_document_catalog_forms() {
  std::cout << "Testing DocumentCatalog forms..." << std::endl;

  DocumentCatalog catalog;

  // Add form fields
  auto text_field = std::unique_ptr<TextField>(new TextField());
  text_field->full_name = "Name";
  catalog.form_fields.push_back(std::move(text_field));

  auto button_field = std::unique_ptr<ButtonField>(new ButtonField());
  button_field->full_name = "Submit";
  button_field->button_type = ButtonField::PushButton;
  catalog.form_fields.push_back(std::move(button_field));

  assert(catalog.form_fields.size() == 2);
  assert(catalog.form_fields[0]->type == FieldType::Text);
  assert(catalog.form_fields[1]->type == FieldType::Button);

  std::cout << "  DocumentCatalog forms: PASSED" << std::endl;
}

int main() {
  std::cout << "=== Phase 3 Feature Tests ===" << std::endl << std::endl;

  test_annotation_types();
  test_text_annotation();
  test_link_annotation();
  test_markup_annotation();
  test_freetext_annotation();
  test_annotation_border();
  test_form_field_types();
  test_text_field();
  test_button_field();
  test_choice_field();
  test_widget_annotation();
  test_appearance_generation();
  test_page_annotations();
  test_document_catalog_forms();
  test_set_text_field_value();
  test_set_button_field_checked();
  test_set_choice_field_selection();
  test_validate_field_value();
  test_get_field_export_value();
  test_find_field_by_name();
  test_fdf_round_trip();

  std::cout << std::endl << "=== All Phase 3 tests passed! ===" << std::endl;

  return 0;
}