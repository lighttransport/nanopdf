// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Phase 3.1: Form Field Value Manipulation

#include "nanopdf.hh"

#include <sstream>
#include <fstream>

namespace nanopdf {

// Set text field value
bool set_text_field_value(TextField* field, const std::string& value) {
  if (!field) {
    return false;
  }

  // Check ReadOnly flag
  if (field->flags & static_cast<uint32_t>(FormFieldFlags::ReadOnly)) {
    return false;
  }

  // Validate max length
  if (field->max_length > 0 && value.length() > static_cast<size_t>(field->max_length)) {
    return false;
  }

  // Set value
  field->field_value.SetType(Value::STRING);
  field->field_value.str = value;

  return true;
}

// Set button field checked/unchecked state
bool set_button_field_checked(ButtonField* field, bool checked) {
  if (!field) {
    return false;
  }

  // Check ReadOnly flag
  if (field->flags & static_cast<uint32_t>(FormFieldFlags::ReadOnly)) {
    return false;
  }

  // For checkboxes and radio buttons
  if (field->button_type == ButtonField::CheckBox ||
      field->button_type == ButtonField::RadioButton) {
    // Set value to Name for checked, "Off" for unchecked
    field->field_value.SetType(Value::NAME);
    if (checked) {
      // Use "Yes" as default checked value
      field->field_value.str = "/Yes";
    } else {
      field->field_value.str = "/Off";
    }
    return true;
  }

  // Push buttons don't have checked state
  return false;
}

// Set choice field selection
bool set_choice_field_selection(ChoiceField* field, const std::vector<int>& indices) {
  if (!field) {
    return false;
  }

  // Check ReadOnly flag
  if (field->flags & static_cast<uint32_t>(FormFieldFlags::ReadOnly)) {
    return false;
  }

  // Validate indices
  for (int idx : indices) {
    if (idx < 0 || idx >= static_cast<int>(field->options.size())) {
      return false;
    }
  }

  // Check if multi-select is allowed
  bool multi_select = field->flags & static_cast<uint32_t>(FormFieldFlags::MultiSelect);
  if (!multi_select && indices.size() > 1) {
    return false;
  }

  // Set selected indices
  field->selected_indices = indices;

  // Set field value
  if (indices.empty()) {
    field->field_value.SetType(Value::NULL_OBJ);
  } else if (indices.size() == 1) {
    // Single selection - set to string
    field->field_value.SetType(Value::STRING);
    field->field_value.str = field->options[indices[0]];
  } else {
    // Multiple selections - set to array
    field->field_value.SetType(Value::ARRAY);
    field->field_value.array.clear();
    for (int idx : indices) {
      Value item;
      item.SetType(Value::STRING);
      item.str = field->options[idx];
      field->field_value.array.push_back(item);
    }
  }

  return true;
}

// Validate field value
bool validate_field_value(const FormField* field, const Value& value) {
  if (!field) {
    return false;
  }

  // Check if field is required
  if (field->flags & static_cast<uint32_t>(FormFieldFlags::Required)) {
    if (value.type == Value::NULL_OBJ) {
      return false;
    }
    if (value.type == Value::STRING && value.str.empty()) {
      return false;
    }
  }

  // Type-specific validation
  switch (field->type) {
    case FieldType::Text: {
      const TextField* text_field = static_cast<const TextField*>(field);
      if (value.type != Value::STRING && value.type != Value::NULL_OBJ) {
        return false;
      }
      if (value.type == Value::STRING && text_field->max_length > 0) {
        if (value.str.length() > static_cast<size_t>(text_field->max_length)) {
          return false;
        }
      }
      break;
    }

    case FieldType::Button: {
      // Button values should be Name type
      if (value.type != Value::NAME && value.type != Value::NULL_OBJ) {
        return false;
      }
      break;
    }

    case FieldType::Choice: {
      const ChoiceField* choice_field = static_cast<const ChoiceField*>(field);
      bool multi_select = field->flags & static_cast<uint32_t>(FormFieldFlags::MultiSelect);

      if (multi_select) {
        // Can be array or single string
        if (value.type != Value::ARRAY && value.type != Value::STRING && value.type != Value::NULL_OBJ) {
          return false;
        }
      } else {
        // Must be single string
        if (value.type != Value::STRING && value.type != Value::NULL_OBJ) {
          return false;
        }
      }
      break;
    }

    default:
      break;
  }

  return true;
}

// Get field export value
std::string get_field_export_value(const FormField* field) {
  if (!field) {
    return "";
  }

  // Check NoExport flag
  if (field->flags & static_cast<uint32_t>(FormFieldFlags::NoExport)) {
    return "";
  }

  // Use mapping name if available, otherwise use field value
  if (!field->mapping_name.empty()) {
    return field->mapping_name;
  }

  // Convert field value to string
  if (field->field_value.type == Value::STRING) {
    return field->field_value.str;
  } else if (field->field_value.type == Value::NAME) {
    return field->field_value.str;
  } else if (field->field_value.type == Value::NUMBER) {
    return std::to_string(field->field_value.number);
  } else if (field->field_value.type == Value::ARRAY) {
    // For multi-select, concatenate with commas
    std::ostringstream oss;
    for (size_t i = 0; i < field->field_value.array.size(); ++i) {
      if (i > 0) oss << ",";
      if (field->field_value.array[i].type == Value::STRING) {
        oss << field->field_value.array[i].str;
      }
    }
    return oss.str();
  }

  return "";
}

// Helper to recursively search form fields
static FormField* find_field_recursive(FormField* field, const std::string& field_name) {
  if (!field) return nullptr;

  if (field->full_name == field_name || field->partial_name == field_name) {
    return field;
  }

  for (auto& child : field->children) {
    FormField* found = find_field_recursive(child.get(), field_name);
    if (found) return found;
  }

  return nullptr;
}

// Find field by name
FormField* find_field_by_name(DocumentCatalog& catalog, const std::string& field_name) {
  for (auto& field : catalog.form_fields) {
    FormField* found = find_field_recursive(field.get(), field_name);
    if (found) return found;
  }
  return nullptr;
}

// Export form data to FDF string
std::string export_form_data_fdf_string(const Pdf& pdf) {
  std::ostringstream oss;

  // FDF header
  oss << "%FDF-1.2\n";
  oss << "1 0 obj\n";
  oss << "<<\n";
  oss << "/FDF <<\n";
  oss << "/Fields [\n";

  // Helper to export field and children
  std::function<void(const FormField*)> export_field;
  export_field = [&](const FormField* field) {
    if (!field) return;

    // Skip fields with NoExport flag
    if (field->flags & static_cast<uint32_t>(FormFieldFlags::NoExport)) {
      return;
    }

    // Export this field if it has a value
    if (field->field_value.type != Value::NULL_OBJ) {
      oss << "<<\n";
      oss << "/T (" << field->full_name << ")\n";
      oss << "/V ";

      // Write value based on type
      if (field->field_value.type == Value::STRING) {
        oss << "(" << field->field_value.str << ")";
      } else if (field->field_value.type == Value::NAME) {
        oss << "/" << field->field_value.str;
      } else if (field->field_value.type == Value::NUMBER) {
        oss << field->field_value.number;
      } else if (field->field_value.type == Value::ARRAY) {
        oss << "[ ";
        for (const auto& item : field->field_value.array) {
          if (item.type == Value::STRING) {
            oss << "(" << item.str << ") ";
          }
        }
        oss << "]";
      }

      oss << "\n>>\n";
    }

    // Export children
    for (const auto& child : field->children) {
      export_field(child.get());
    }
  };

  // Export all form fields
  for (const auto& field : pdf.catalog.form_fields) {
    export_field(field.get());
  }

  oss << "]\n";
  oss << ">>\n";
  oss << ">>\n";
  oss << "endobj\n";
  oss << "trailer\n<<\n/Root 1 0 R\n>>\n";
  oss << "%%EOF\n";

  return oss.str();
}

// Export form data to FDF file
bool export_form_data_fdf(const Pdf& pdf, const std::string& output_path) {
  std::string fdf_data = export_form_data_fdf_string(pdf);

  std::ofstream file(output_path, std::ios::binary);
  if (!file) {
    return false;
  }

  file.write(fdf_data.c_str(), fdf_data.size());
  return file.good();
}

// Import form data from FDF string (simplified implementation)
bool import_form_data_fdf_string(Pdf& pdf, const std::string& fdf_data) {
  // This is a simplified parser for FDF data
  // A full implementation would use a proper PDF parser

  // Find the /Fields array
  size_t fields_pos = fdf_data.find("/Fields");
  if (fields_pos == std::string::npos) {
    return false;
  }

  // Find the opening bracket
  size_t bracket_pos = fdf_data.find('[', fields_pos);
  if (bracket_pos == std::string::npos) {
    return false;
  }

  // Simple state machine to parse field dictionaries
  size_t pos = bracket_pos + 1;
  while (pos < fdf_data.length()) {
    // Skip whitespace
    while (pos < fdf_data.length() && std::isspace(fdf_data[pos])) {
      pos++;
    }

    if (pos >= fdf_data.length()) break;

    // Check for end of array
    if (fdf_data[pos] == ']') {
      break;
    }

    // Expect "<<"
    if (fdf_data.substr(pos, 2) != "<<") {
      break;
    }
    pos += 2;

    // Parse field dictionary
    std::string field_name;
    std::string field_value;

    // Look for /T (field name) and /V (field value)
    while (pos < fdf_data.length()) {
      // Skip whitespace
      while (pos < fdf_data.length() && std::isspace(fdf_data[pos])) {
        pos++;
      }

      if (pos >= fdf_data.length()) break;

      // Check for end of dictionary
      if (fdf_data.substr(pos, 2) == ">>") {
        pos += 2;
        break;
      }

      // Parse key
      if (fdf_data[pos] != '/') {
        pos++;
        continue;
      }

      size_t key_start = pos + 1;
      size_t key_end = fdf_data.find_first_of(" \t\r\n(", key_start);
      if (key_end == std::string::npos) break;

      std::string key = fdf_data.substr(key_start, key_end - key_start);
      pos = key_end;

      // Skip whitespace
      while (pos < fdf_data.length() && std::isspace(fdf_data[pos])) {
        pos++;
      }

      // Parse value
      if (key == "T") {
        // Field name - should be in parentheses
        if (fdf_data[pos] == '(') {
          pos++;
          size_t value_end = fdf_data.find(')', pos);
          if (value_end != std::string::npos) {
            field_name = fdf_data.substr(pos, value_end - pos);
            pos = value_end + 1;
          }
        }
      } else if (key == "V") {
        // Field value
        if (fdf_data[pos] == '(') {
          pos++;
          size_t value_end = fdf_data.find(')', pos);
          if (value_end != std::string::npos) {
            field_value = fdf_data.substr(pos, value_end - pos);
            pos = value_end + 1;
          }
        }
      } else {
        // Skip unknown key-value pairs
        if (fdf_data[pos] == '(') {
          size_t value_end = fdf_data.find(')', pos + 1);
          if (value_end != std::string::npos) {
            pos = value_end + 1;
          }
        }
      }
    }

    // Apply field value if we found both name and value
    if (!field_name.empty() && !field_value.empty()) {
      FormField* field = find_field_by_name(pdf.catalog, field_name);
      if (field) {
        // Set value based on field type
        if (field->type == FieldType::Text) {
          TextField* text_field = static_cast<TextField*>(field);
          set_text_field_value(text_field, field_value);
        } else if (field->type == FieldType::Button) {
          ButtonField* button_field = static_cast<ButtonField*>(field);
          bool checked = (field_value == "Yes" || field_value == "On");
          set_button_field_checked(button_field, checked);
        }
        // Add more field types as needed
      }
    }
  }

  return true;
}

// Import form data from FDF file
bool import_form_data_fdf(Pdf& pdf, const std::string& fdf_path) {
  std::ifstream file(fdf_path, std::ios::binary);
  if (!file) {
    return false;
  }

  std::string fdf_data((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());

  return import_form_data_fdf_string(pdf, fdf_data);
}

}  // namespace nanopdf
