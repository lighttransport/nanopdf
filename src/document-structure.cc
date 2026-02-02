// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Phase 4: Document Structure and Navigation

#include "nanopdf.hh"

#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>

namespace nanopdf {

// Forward declarations
static std::string to_roman_numeral(uint32_t value, bool uppercase);
static std::string to_letter_sequence(uint32_t value, bool uppercase);

// Parse outline item (bookmark) recursively
std::unique_ptr<OutlineItem> parse_outline_item(const Pdf& pdf, const Dictionary& outline_dict) {
  auto item = std::make_unique<OutlineItem>();

  // Title (required)
  auto title_it = outline_dict.find("Title");
  if (title_it != outline_dict.end() && title_it->second.type == Value::STRING) {
    item->title = title_it->second.str;
  }

  // Destination or Action
  auto dest_it = outline_dict.find("Dest");
  auto action_it = outline_dict.find("A");

  if (dest_it != outline_dict.end()) {
    // Direct destination
    const Value& dest = dest_it->second;

    if (dest.type == Value::ARRAY && !dest.array.empty()) {
      // Explicit destination array: [page /Type ...]
      if (dest.array[0].type == Value::REFERENCE) {
        // Find page by reference
        uint32_t page_obj = dest.array[0].ref_object_number;
        for (size_t i = 0; i < pdf.catalog.pages.size(); ++i) {
          if (pdf.catalog.pages[i].object_number == page_obj) {
            item->dest_page = static_cast<uint32_t>(i);
            break;
          }
        }
      }

      // Parse position parameters (if any)
      if (dest.array.size() >= 2 && dest.array[1].type == Value::NAME) {
        item->action_type = OutlineAction::GoTo;
        // Store fit type and parameters
        for (size_t i = 2; i < dest.array.size(); ++i) {
          if (dest.array[i].type == Value::NUMBER) {
            item->dest_position.push_back(dest.array[i].number);
          }
        }
      }
    } else if (dest.type == Value::STRING || dest.type == Value::NAME) {
      // Named destination - would need to resolve from Names dictionary
      item->action_type = OutlineAction::GoTo;
    }
  } else if (action_it != outline_dict.end() && action_it->second.type == Value::DICTIONARY) {
    // Action dictionary
    const Dictionary& action_dict = action_it->second.dict;

    auto s_it = action_dict.find("S");
    if (s_it != action_dict.end() && s_it->second.type == Value::NAME) {
      std::string action_type = s_it->second.str;

      if (action_type == "/GoTo") {
        item->action_type = OutlineAction::GoTo;
        // Parse D (destination)
        auto d_it = action_dict.find("D");
        if (d_it != action_dict.end() && d_it->second.type == Value::ARRAY) {
          // Similar to above
          const auto& dest_array = d_it->second.array;
          if (!dest_array.empty() && dest_array[0].type == Value::REFERENCE) {
            uint32_t page_obj = dest_array[0].ref_object_number;
            for (size_t i = 0; i < pdf.catalog.pages.size(); ++i) {
              if (pdf.catalog.pages[i].object_number == page_obj) {
                item->dest_page = static_cast<uint32_t>(i);
                break;
              }
            }
          }
        }
      } else if (action_type == "/URI") {
        item->action_type = OutlineAction::URI;
        auto uri_it = action_dict.find("URI");
        if (uri_it != action_dict.end() && uri_it->second.type == Value::STRING) {
          item->uri = uri_it->second.str;
        }
      } else if (action_type == "/GoToR") {
        item->action_type = OutlineAction::GoToR;
        auto f_it = action_dict.find("F");
        if (f_it != action_dict.end() && f_it->second.type == Value::STRING) {
          item->file = f_it->second.str;
        }
      } else if (action_type == "/Launch") {
        item->action_type = OutlineAction::Launch;
        auto f_it = action_dict.find("F");
        if (f_it != action_dict.end() && f_it->second.type == Value::STRING) {
          item->file = f_it->second.str;
        }
      }
    }
  }

  // Color
  auto c_it = outline_dict.find("C");
  if (c_it != outline_dict.end() && c_it->second.type == Value::ARRAY) {
    for (const auto& val : c_it->second.array) {
      if (val.type == Value::NUMBER) {
        item->color.push_back(val.number);
      }
    }
  }

  // Style flags
  auto f_it = outline_dict.find("F");
  if (f_it != outline_dict.end() && f_it->second.type == Value::NUMBER) {
    int flags = static_cast<int>(f_it->second.number);
    item->italic = (flags & 1) != 0;
    item->bold = (flags & 2) != 0;
  }

  // Parse children recursively
  auto first_it = outline_dict.find("First");
  if (first_it != outline_dict.end() && first_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, first_it->second.ref_object_number,
                                      first_it->second.ref_generation_number);
    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
      auto child = parse_outline_item(pdf, resolved.value.dict);
      if (child) {
        item->children.push_back(std::move(child));

        // Parse siblings
        const Dictionary* current_dict = &resolved.value.dict;
        while (true) {
          auto next_it = current_dict->find("Next");
          if (next_it == current_dict->end() || next_it->second.type != Value::REFERENCE) {
            break;
          }

          auto next_resolved = resolve_reference(pdf, next_it->second.ref_object_number,
                                                 next_it->second.ref_generation_number);
          if (!next_resolved.success || next_resolved.value.type != Value::DICTIONARY) {
            break;
          }

          auto sibling = parse_outline_item(pdf, next_resolved.value.dict);
          if (sibling) {
            item->children.push_back(std::move(sibling));
          }

          current_dict = &next_resolved.value.dict;
        }
      }
    }
  }

  return item;
}

// Parse document outline (bookmarks)
void parse_document_outline(const Pdf& pdf, DocumentCatalog& catalog) {
  // Resolve catalog object to get dictionary
  auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
  if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) {
    return;
  }

  const auto& catalog_dict = catalog_obj.value.dict;

  // Find Outlines entry in catalog
  auto outlines_it = catalog_dict.find("Outlines");
  if (outlines_it == catalog_dict.end()) {
    return;  // No outlines
  }

  // Resolve outlines reference
  Value outlines_value = outlines_it->second;
  if (outlines_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, outlines_value.ref_object_number,
                                      outlines_value.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return;
    }
    outlines_value = resolved.value;
  }

  if (outlines_value.type != Value::DICTIONARY) {
    return;
  }

  const Dictionary& outlines_dict = outlines_value.dict;

  // Parse first outline item
  auto first_it = outlines_dict.find("First");
  if (first_it != outlines_dict.end() && first_it->second.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, first_it->second.ref_object_number,
                                      first_it->second.ref_generation_number);
    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
      catalog.outline_root = parse_outline_item(pdf, resolved.value.dict);

      // Parse sibling outline items at root level
      const Dictionary* current_dict = &resolved.value.dict;
      while (true) {
        auto next_it = current_dict->find("Next");
        if (next_it == current_dict->end() || next_it->second.type != Value::REFERENCE) {
          break;
        }

        auto next_resolved = resolve_reference(pdf, next_it->second.ref_object_number,
                                               next_it->second.ref_generation_number);
        if (!next_resolved.success || next_resolved.value.type != Value::DICTIONARY) {
          break;
        }

        auto sibling = parse_outline_item(pdf, next_resolved.value.dict);
        if (sibling && catalog.outline_root) {
          catalog.outline_root->children.push_back(std::move(sibling));
        }

        current_dict = &next_resolved.value.dict;
      }
    }
  }
}

// Parse page labels
void parse_page_labels(const Pdf& pdf, DocumentCatalog& catalog) {
  // Resolve catalog object to get dictionary
  auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
  if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) {
    return;
  }

  const auto& catalog_dict = catalog_obj.value.dict;

  // Find PageLabels entry in catalog
  auto labels_it = catalog_dict.find("PageLabels");
  if (labels_it == catalog_dict.end()) {
    return;  // No page labels
  }

  // Resolve reference if needed
  Value labels_value = labels_it->second;
  if (labels_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, labels_value.ref_object_number,
                                      labels_value.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return;
    }
    labels_value = resolved.value;
  }

  if (labels_value.type != Value::DICTIONARY) {
    return;
  }

  const Dictionary& labels_dict = labels_value.dict;

  // Find Nums array
  auto nums_it = labels_dict.find("Nums");
  if (nums_it == labels_dict.end() || nums_it->second.type != Value::ARRAY) {
    return;
  }

  const std::vector<Value>& nums = nums_it->second.array;

  // Parse page label entries (pairs of: page_index, label_dict)
  for (size_t i = 0; i + 1 < nums.size(); i += 2) {
    if (nums[i].type != Value::NUMBER) {
      continue;
    }

    uint32_t page_index = static_cast<uint32_t>(nums[i].number);

    // Get label dictionary
    Value label_dict_value = nums[i + 1];
    if (label_dict_value.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, label_dict_value.ref_object_number,
                                        label_dict_value.ref_generation_number);
      if (resolved.success) {
        label_dict_value = resolved.value;
      }
    }

    if (label_dict_value.type != Value::DICTIONARY) {
      continue;
    }

    const Dictionary& label_dict = label_dict_value.dict;

    PageLabel label;

    // Type (S)
    auto s_it = label_dict.find("S");
    if (s_it != label_dict.end() && s_it->second.type == Value::NAME) {
      std::string style = s_it->second.str;
      if (style == "/D") {
        label.style = PageLabelStyle::DecimalArabic;
      } else if (style == "/r") {
        label.style = PageLabelStyle::LowercaseRoman;
      } else if (style == "/R") {
        label.style = PageLabelStyle::UppercaseRoman;
      } else if (style == "/a") {
        label.style = PageLabelStyle::LowercaseLetters;
      } else if (style == "/A") {
        label.style = PageLabelStyle::UppercaseLetters;
      }
    }

    // Prefix (P)
    auto p_it = label_dict.find("P");
    if (p_it != label_dict.end() && p_it->second.type == Value::STRING) {
      label.prefix = p_it->second.str;
    }

    // Start value (St)
    auto st_it = label_dict.find("St");
    if (st_it != label_dict.end() && st_it->second.type == Value::NUMBER) {
      label.start_value = static_cast<uint32_t>(st_it->second.number);
    }

    catalog.page_labels.labels[page_index] = label;
  }
}

// Helper: Parse a single destination value into NamedDestination
static void parse_destination_value(const Pdf& pdf, const std::string& name,
                                    const Value& dest_value,
                                    DocumentCatalog& catalog) {
  NamedDestination dest;
  dest.name = name;

  Value dest_array = dest_value;
  if (dest_array.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, dest_array.ref_object_number,
                                      dest_array.ref_generation_number);
    if (resolved.success) {
      dest_array = resolved.value;
    }
  }

  // Handle dictionary with D entry (destination dictionary)
  if (dest_array.type == Value::DICTIONARY) {
    auto d_it = dest_array.dict.find("D");
    if (d_it != dest_array.dict.end()) {
      dest_array = d_it->second;
      if (dest_array.type == Value::REFERENCE) {
        auto resolved = resolve_reference(pdf, dest_array.ref_object_number,
                                          dest_array.ref_generation_number);
        if (resolved.success) {
          dest_array = resolved.value;
        }
      }
    }
  }

  if (dest_array.type == Value::ARRAY && !dest_array.array.empty()) {
    // Parse destination array [page /Type params...]
    if (dest_array.array[0].type == Value::REFERENCE) {
      uint32_t page_obj = dest_array.array[0].ref_object_number;
      for (size_t i = 0; i < pdf.catalog.pages.size(); ++i) {
        if (pdf.catalog.pages[i].object_number == page_obj) {
          dest.page_number = static_cast<uint32_t>(i);
          break;
        }
      }
    } else if (dest_array.array[0].type == Value::NUMBER) {
      // Page number directly (rare but valid)
      dest.page_number = static_cast<uint32_t>(dest_array.array[0].number);
    }

    // Fit type
    if (dest_array.array.size() >= 2 && dest_array.array[1].type == Value::NAME) {
      dest.fit_type = dest_array.array[1].str;
    }

    // Position parameters
    for (size_t i = 2; i < dest_array.array.size(); ++i) {
      if (dest_array.array[i].type == Value::NUMBER) {
        dest.position.push_back(dest_array.array[i].number);
      } else if (dest_array.array[i].type == Value::NULL_OBJ) {
        // null represents "unchanged" - use NaN as placeholder
        dest.position.push_back(std::numeric_limits<double>::quiet_NaN());
      }
    }
  }

  catalog.named_destinations[dest.name] = dest;
}

// Helper: Recursively parse a name tree node
// Name trees have either:
// - Kids: array of child node references (intermediate node)
// - Names: array of key-value pairs (leaf node)
static void parse_name_tree_node(const Pdf& pdf, const Value& node_value,
                                 DocumentCatalog& catalog) {
  Value node = node_value;
  if (node.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, node.ref_object_number,
                                      node.ref_generation_number);
    if (!resolved.success) {
      return;
    }
    node = resolved.value;
  }

  if (node.type != Value::DICTIONARY) {
    return;
  }

  const auto& node_dict = node.dict;

  // Check for Names array (leaf node)
  auto names_it = node_dict.find("Names");
  if (names_it != node_dict.end() && names_it->second.type == Value::ARRAY) {
    const auto& names_array = names_it->second.array;
    // Names array: [key1 value1 key2 value2 ...]
    for (size_t i = 0; i + 1 < names_array.size(); i += 2) {
      std::string key;
      if (names_array[i].type == Value::STRING) {
        key = names_array[i].str;
      } else if (names_array[i].type == Value::NAME) {
        key = names_array[i].str;
      } else {
        continue;  // Invalid key type
      }

      parse_destination_value(pdf, key, names_array[i + 1], catalog);
    }
  }

  // Check for Kids array (intermediate node)
  auto kids_it = node_dict.find("Kids");
  if (kids_it != node_dict.end() && kids_it->second.type == Value::ARRAY) {
    for (const auto& kid : kids_it->second.array) {
      parse_name_tree_node(pdf, kid, catalog);
    }
  }
}

// Parse named destinations
void parse_named_destinations(const Pdf& pdf, DocumentCatalog& catalog) {
  // Resolve catalog object to get dictionary
  auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
  if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) {
    return;
  }

  const auto& catalog_dict = catalog_obj.value.dict;

  // Find Dests entry in catalog
  auto dests_it = catalog_dict.find("Dests");
  if (dests_it != catalog_dict.end()) {
    // Legacy Dests dictionary
    Value dests_value = dests_it->second;
    if (dests_value.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, dests_value.ref_object_number,
                                        dests_value.ref_generation_number);
      if (resolved.success) {
        dests_value = resolved.value;
      }
    }

    if (dests_value.type == Value::DICTIONARY) {
      for (const auto& entry : dests_value.dict) {
        NamedDestination dest;
        dest.name = entry.first;

        Value dest_array = entry.second;
        if (dest_array.type == Value::REFERENCE) {
          auto resolved = resolve_reference(pdf, dest_array.ref_object_number,
                                            dest_array.ref_generation_number);
          if (resolved.success) {
            dest_array = resolved.value;
          }
        }

        if (dest_array.type == Value::ARRAY && !dest_array.array.empty()) {
          // Parse destination array
          if (dest_array.array[0].type == Value::REFERENCE) {
            uint32_t page_obj = dest_array.array[0].ref_object_number;
            for (size_t i = 0; i < pdf.catalog.pages.size(); ++i) {
              if (pdf.catalog.pages[i].object_number == page_obj) {
                dest.page_number = static_cast<uint32_t>(i);
                break;
              }
            }
          }

          // Fit type
          if (dest_array.array.size() >= 2 && dest_array.array[1].type == Value::NAME) {
            dest.fit_type = dest_array.array[1].str;
          }

          // Position parameters
          for (size_t i = 2; i < dest_array.array.size(); ++i) {
            if (dest_array.array[i].type == Value::NUMBER) {
              dest.position.push_back(dest_array.array[i].number);
            }
          }
        }

        catalog.named_destinations[dest.name] = dest;
      }
    }
  }

  // Check Names/Dests name tree
  auto names_it = catalog_dict.find("Names");
  if (names_it != catalog_dict.end()) {
    Value names_value = names_it->second;
    if (names_value.type == Value::REFERENCE) {
      auto resolved = resolve_reference(pdf, names_value.ref_object_number,
                                        names_value.ref_generation_number);
      if (resolved.success) {
        names_value = resolved.value;
      }
    }

    if (names_value.type == Value::DICTIONARY) {
      auto dests_tree_it = names_value.dict.find("Dests");
      if (dests_tree_it != names_value.dict.end()) {
        // Parse name tree structure recursively
        parse_name_tree_node(pdf, dests_tree_it->second, catalog);
      }
    }
  }
}

// Parse document info dictionary
void parse_document_info(const Pdf& pdf, DocumentCatalog& catalog) {
  // Document info is in trailer, not catalog
  auto info_it = pdf.trailer.find("Info");
  if (info_it == pdf.trailer.end()) {
    return;
  }

  Value info_value = info_it->second;
  if (info_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, info_value.ref_object_number,
                                      info_value.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::DICTIONARY) {
      return;
    }
    info_value = resolved.value;
  }

  if (info_value.type != Value::DICTIONARY) {
    return;
  }

  const Dictionary& info_dict = info_value.dict;

  // Parse standard fields
  auto get_string = [&](const std::string& key) -> std::string {
    auto it = info_dict.find(key);
    if (it != info_dict.end() && it->second.type == Value::STRING) {
      return it->second.str;
    }
    return "";
  };

  catalog.document_info.title = get_string("Title");
  catalog.document_info.author = get_string("Author");
  catalog.document_info.subject = get_string("Subject");
  catalog.document_info.keywords = get_string("Keywords");
  catalog.document_info.creator = get_string("Creator");
  catalog.document_info.producer = get_string("Producer");
  catalog.document_info.creation_date = get_string("CreationDate");
  catalog.document_info.mod_date = get_string("ModDate");
  catalog.document_info.trapped = get_string("Trapped");

  // Parse custom fields (non-standard entries)
  for (const auto& entry : info_dict) {
    if (entry.first != "Title" && entry.first != "Author" &&
        entry.first != "Subject" && entry.first != "Keywords" &&
        entry.first != "Creator" && entry.first != "Producer" &&
        entry.first != "CreationDate" && entry.first != "ModDate" &&
        entry.first != "Trapped") {
      if (entry.second.type == Value::STRING) {
        catalog.document_info.custom[entry.first] = entry.second.str;
      }
    }
  }
}

// Parse XMP metadata
void parse_xmp_metadata(const Pdf& pdf, DocumentCatalog& catalog) {
  // Resolve catalog object to get dictionary
  auto catalog_obj = resolve_reference(pdf, pdf.root, 0);
  if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) {
    return;
  }

  const auto& catalog_dict = catalog_obj.value.dict;

  // Find Metadata entry in catalog
  auto metadata_it = catalog_dict.find("Metadata");
  if (metadata_it == catalog_dict.end()) {
    return;
  }

  Value metadata_value = metadata_it->second;
  if (metadata_value.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, metadata_value.ref_object_number,
                                      metadata_value.ref_generation_number);
    if (!resolved.success || resolved.value.type != Value::STREAM) {
      return;
    }
    metadata_value = resolved.value;
  }

  if (metadata_value.type != Value::STREAM) {
    return;
  }

  // Decode stream to get XML
  auto decoded = decode_stream(pdf, metadata_value);
  if (!decoded.success || decoded.data.empty()) {
    return;
  }

  // Store raw XML
  catalog.xmp_metadata.raw_xml = std::string(
      reinterpret_cast<const char*>(decoded.data.data()),
      decoded.data.size());

  // Parse XML (simplified - just extract common fields)
  catalog.xmp_metadata.parse_xml(catalog.xmp_metadata.raw_xml);
}

// Implement PageLabels::get_label
std::string PageLabels::get_label(uint32_t page_index) const {
  // Find the applicable label for this page
  const PageLabel* label = nullptr;
  uint32_t label_start_page = 0;

  for (const auto& entry : labels) {
    if (entry.first <= page_index) {
      if (label == nullptr || entry.first > label_start_page) {
        label = &entry.second;
        label_start_page = entry.first;
      }
    }
  }

  if (!label) {
    // No label - use default decimal numbering
    return std::to_string(page_index + 1);
  }

  // Calculate the numeric value for this page
  uint32_t value = label->start_value + (page_index - label_start_page);

  // Format according to style
  std::string number_str;
  switch (label->style) {
    case PageLabelStyle::DecimalArabic:
      number_str = std::to_string(value);
      break;

    case PageLabelStyle::UppercaseRoman:
      number_str = to_roman_numeral(value, true);
      break;

    case PageLabelStyle::LowercaseRoman:
      number_str = to_roman_numeral(value, false);
      break;

    case PageLabelStyle::UppercaseLetters:
      number_str = to_letter_sequence(value, true);
      break;

    case PageLabelStyle::LowercaseLetters:
      number_str = to_letter_sequence(value, false);
      break;

    case PageLabelStyle::None:
      // No numbering
      break;
  }

  return label->prefix + number_str;
}

// Helper: Convert to Roman numeral
static std::string to_roman_numeral(uint32_t value, bool uppercase) {
  if (value == 0 || value > 3999) {
    return std::to_string(value);  // Out of range for Roman numerals
  }

  const char* numerals_upper[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
  const char* numerals_lower[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
  const int values[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};

  const char** numerals = uppercase ? numerals_upper : numerals_lower;

  std::string result;
  for (int i = 0; i < 13; ++i) {
    while (value >= static_cast<uint32_t>(values[i])) {
      result += numerals[i];
      value -= values[i];
    }
  }

  return result;
}

// Helper: Convert to letter sequence (A, B, ..., Z, AA, AB, ...)
static std::string to_letter_sequence(uint32_t value, bool uppercase) {
  if (value == 0) {
    return "";
  }

  char base = uppercase ? 'A' : 'a';
  std::string result;

  value--;  // Convert to 0-based
  while (true) {
    result = std::string(1, base + (value % 26)) + result;
    if (value < 26) {
      break;
    }
    value = value / 26 - 1;
  }

  return result;
}

// Implement XMPMetadata::parse_xml (simplified)
bool XMPMetadata::parse_xml(const std::string& xml) {
  // Simplified XML parsing - just extract common fields using string search
  // A full implementation would use a proper XML parser

  auto extract_tag_content = [&](const std::string& tag) -> std::string {
    std::string open_tag = "<" + tag + ">";
    std::string close_tag = "</" + tag + ">";

    size_t start = xml.find(open_tag);
    if (start == std::string::npos) {
      return "";
    }

    start += open_tag.length();
    size_t end = xml.find(close_tag, start);
    if (end == std::string::npos) {
      return "";
    }

    return xml.substr(start, end - start);
  };

  dc_title = extract_tag_content("dc:title");
  dc_creator = extract_tag_content("dc:creator");
  dc_description = extract_tag_content("dc:description");
  xmp_create_date = extract_tag_content("xmp:CreateDate");
  xmp_modify_date = extract_tag_content("xmp:ModifyDate");
  xmp_metadata_date = extract_tag_content("xmp:MetadataDate");
  pdf_producer = extract_tag_content("pdf:Producer");
  pdf_version = extract_tag_content("pdf:PDFVersion");

  return true;
}

}  // namespace nanopdf
