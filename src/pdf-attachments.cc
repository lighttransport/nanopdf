// Copyright 2017 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility
// and integration with nanopdf architecture.

#include "pdf-attachments.hh"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "nanopdf.hh"

namespace nanopdf {

AttachmentExtractor::AttachmentExtractor(const Pdf& pdf) : pdf_(pdf) {}

AttachmentExtractor::~AttachmentExtractor() {}

int AttachmentExtractor::get_count() const {
  NameTreeNode root;
  if (!parse_name_tree(root)) {
    return 0;
  }

  std::vector<std::pair<std::string, Value>> all_entries;
  collect_all_entries(root, all_entries);
  return static_cast<int>(all_entries.size());
}

FileAttachment AttachmentExtractor::get_attachment(int index) const {
  FileAttachment result;

  if (index < 0) {
    result.error = "Index must be non-negative";
    return result;
  }

  NameTreeNode root;
  if (!parse_name_tree(root)) {
    result.error = "No EmbeddedFiles name tree found";
    return result;
  }

  std::vector<std::pair<std::string, Value>> all_entries;
  collect_all_entries(root, all_entries);

  if (static_cast<size_t>(index) >= all_entries.size()) {
    result.error = "Index out of range";
    return result;
  }

  const auto& entry = all_entries[index];
  result.name = entry.first;

  result = parse_filespec(entry.second);
  if (!result.success && result.name.empty()) {
    result.name = entry.first;
  }

  return result;
}

FileAttachment AttachmentExtractor::get_attachment_by_name(
    const std::string& name) const {
  FileAttachment result;

  NameTreeNode root;
  if (!parse_name_tree(root)) {
    result.error = "No EmbeddedFiles name tree found";
    return result;
  }

  std::vector<std::pair<std::string, Value>> all_entries;
  collect_all_entries(root, all_entries);

  for (const auto& entry : all_entries) {
    if (entry.first == name) {
      return parse_filespec(entry.second);
    }
  }

  result.error = "Attachment not found: " + name;
  return result;
}

std::vector<std::string> AttachmentExtractor::list_names() const {
  std::vector<std::string> names;

  NameTreeNode root;
  if (!parse_name_tree(root)) {
    return names;
  }

  std::vector<std::pair<std::string, Value>> all_entries;
  collect_all_entries(root, all_entries);

  names.reserve(all_entries.size());
  for (const auto& entry : all_entries) {
    names.push_back(entry.first);
  }

  return names;
}

bool AttachmentExtractor::parse_name_tree(NameTreeNode& root) const {
  // Navigate to /Root/Names/EmbeddedFiles
  // pdf_.trailer is already a Dictionary
  auto root_ref_it = pdf_.trailer.find("Root");
  if (root_ref_it == pdf_.trailer.end()) {
    return false;
  }

  const Value& root_ref = root_ref_it->second;
  ResolvedObject catalog_obj;

  if (root_ref.type == Value::REFERENCE) {
    catalog_obj = resolve_reference(pdf_, root_ref.ref_object_number,
                                    root_ref.ref_generation_number);
  } else if (root_ref.type == Value::DICTIONARY) {
    catalog_obj.success = true;
    catalog_obj.value = root_ref;
  } else {
    return false;
  }

  if (!catalog_obj.success || catalog_obj.value.type != Value::DICTIONARY) {
    return false;
  }

  auto names_ref_it = catalog_obj.value.dict.find("Names");
  if (names_ref_it == catalog_obj.value.dict.end()) {
    return false;  // No Names dictionary
  }

  const Value& names_ref = names_ref_it->second;
  ResolvedObject names_obj;

  if (names_ref.type == Value::REFERENCE) {
    names_obj = resolve_reference(pdf_, names_ref.ref_object_number,
                                  names_ref.ref_generation_number);
  } else if (names_ref.type == Value::DICTIONARY) {
    names_obj.success = true;
    names_obj.value = names_ref;
  } else {
    return false;
  }

  if (!names_obj.success || names_obj.value.type != Value::DICTIONARY) {
    return false;
  }

  auto embedded_files_ref_it = names_obj.value.dict.find("EmbeddedFiles");
  if (embedded_files_ref_it == names_obj.value.dict.end()) {
    return false;  // No EmbeddedFiles entry
  }

  const Value& embedded_files_ref = embedded_files_ref_it->second;
  ResolvedObject embedded_files_obj;

  if (embedded_files_ref.type == Value::REFERENCE) {
    embedded_files_obj = resolve_reference(pdf_, embedded_files_ref.ref_object_number,
                                          embedded_files_ref.ref_generation_number);
  } else if (embedded_files_ref.type == Value::DICTIONARY) {
    embedded_files_obj.success = true;
    embedded_files_obj.value = embedded_files_ref;
  } else {
    return false;
  }

  if (!embedded_files_obj.success || embedded_files_obj.value.type != Value::DICTIONARY) {
    return false;
  }

  // Parse the name tree structure
  auto names_array_ref_it = embedded_files_obj.value.dict.find("Names");

  if (names_array_ref_it != embedded_files_obj.value.dict.end()) {
    // Leaf node with Names array
    const Value& names_array_ref = names_array_ref_it->second;
    ResolvedObject names_array_obj;

    if (names_array_ref.type == Value::REFERENCE) {
      names_array_obj = resolve_reference(pdf_, names_array_ref.ref_object_number,
                                         names_array_ref.ref_generation_number);
    } else if (names_array_ref.type == Value::ARRAY) {
      names_array_obj.success = true;
      names_array_obj.value = names_array_ref;
    } else {
      return false;
    }

    if (names_array_obj.success && names_array_obj.value.type == Value::ARRAY) {
      const auto& names_array = names_array_obj.value.array;

      // Names array is: [name1, value1, name2, value2, ...]
      for (size_t i = 0; i + 1 < names_array.size(); i += 2) {
        std::string name;
        if (names_array[i].type == Value::STRING) {
          name = names_array[i].str;
        }

        root.entries.push_back(std::make_pair(name, names_array[i + 1]));
      }
    }
  }

  // For now, we only support single-level name trees
  // Multi-level trees with Kids would require recursive parsing

  return !root.entries.empty();
}

void AttachmentExtractor::collect_all_entries(
    const NameTreeNode& node,
    std::vector<std::pair<std::string, Value>>& out) const {
  // Add entries from this node
  out.insert(out.end(), node.entries.begin(), node.entries.end());

  // Recursively add from children
  for (const auto& kid : node.kids) {
    collect_all_entries(kid, out);
  }
}

FileAttachment AttachmentExtractor::parse_filespec(
    const Value& filespec_value) const {
  FileAttachment result;

  // Resolve reference if needed
  ResolvedObject filespec_obj;
  if (filespec_value.type == Value::REFERENCE) {
    filespec_obj = resolve_reference(pdf_, filespec_value.ref_object_number,
                                     filespec_value.ref_generation_number);
  } else if (filespec_value.type == Value::DICTIONARY) {
    filespec_obj.success = true;
    filespec_obj.value = filespec_value;
  } else {
    result.error = "Filespec is not a dictionary or reference";
    return result;
  }

  if (!filespec_obj.success) {
    result.error = "Failed to resolve filespec reference";
    return result;
  }

  if (filespec_obj.value.type != Value::DICTIONARY) {
    result.error = "Filespec is not a dictionary";
    return result;
  }

  const Dictionary& filespec = filespec_obj.value.dict;

  // Extract filename (prefer UF over F)
  result.name = get_filename(filespec_obj.value);

  // Extract description
  auto desc_it = filespec.find("Desc");
  if (desc_it != filespec.end()) {
    const Value& desc_ref = desc_it->second;
    ResolvedObject desc_obj;

    if (desc_ref.type == Value::REFERENCE) {
      desc_obj = resolve_reference(pdf_, desc_ref.ref_object_number,
                                   desc_ref.ref_generation_number);
    } else {
      desc_obj.success = true;
      desc_obj.value = desc_ref;
    }

    if (desc_obj.success && desc_obj.value.type == Value::STRING) {
      result.description = desc_obj.value.str;
    }
  }

  // Extract AFRelationship (PDF 2.0)
  auto af_rel_it = filespec.find("AFRelationship");
  if (af_rel_it != filespec.end()) {
    const Value& af_rel_ref = af_rel_it->second;
    ResolvedObject af_rel_obj;

    if (af_rel_ref.type == Value::REFERENCE) {
      af_rel_obj = resolve_reference(pdf_, af_rel_ref.ref_object_number,
                                     af_rel_ref.ref_generation_number);
    } else {
      af_rel_obj.success = true;
      af_rel_obj.value = af_rel_ref;
    }

    if (af_rel_obj.success) {
      if (af_rel_obj.value.type == Value::NAME) {
        result.relationship = af_rel_obj.value.name;
      } else if (af_rel_obj.value.type == Value::STRING) {
        result.relationship = af_rel_obj.value.str;
      }
    }
  }

  // Get the embedded file stream
  const Value* stream = get_file_stream(filespec_obj.value);
  if (!stream) {
    result.error = "No embedded file stream found (EF/F missing)";
    return result;
  }

  ResolvedObject stream_obj;
  uint32_t stream_obj_num = 0;
  uint16_t stream_gen_num = 0;
  if (stream->type == Value::REFERENCE) {
    stream_obj_num = stream->ref_object_number;
    stream_gen_num = stream->ref_generation_number;
    stream_obj = resolve_reference(pdf_, stream->ref_object_number,
                                   stream->ref_generation_number);
  } else if (stream->type == Value::STREAM) {
    stream_obj.success = true;
    stream_obj.value = *stream;
  } else {
    result.error = "Embedded file is not a stream or reference";
    return result;
  }

  if (!stream_obj.success || stream_obj.value.type != Value::STREAM) {
    result.error = "Embedded file is not a stream";
    return result;
  }

  // Extract MIME type from stream's Subtype
  auto subtype_it = stream_obj.value.stream.dict.find("Subtype");
  if (subtype_it != stream_obj.value.stream.dict.end()) {
    const Value& subtype_ref = subtype_it->second;
    ResolvedObject subtype_obj;

    if (subtype_ref.type == Value::REFERENCE) {
      subtype_obj = resolve_reference(pdf_, subtype_ref.ref_object_number,
                                      subtype_ref.ref_generation_number);
    } else {
      subtype_obj.success = true;
      subtype_obj.value = subtype_ref;
    }

    if (subtype_obj.success) {
      if (subtype_obj.value.type == Value::NAME) {
        result.mime_type = subtype_obj.value.name;
      } else if (subtype_obj.value.type == Value::STRING) {
        result.mime_type = subtype_obj.value.str;
      }
    }
  }

  // Get Params dictionary for metadata
  const Value* params = get_params_dict(stream_obj.value);
  if (params) {
    ResolvedObject params_obj;

    if (params->type == Value::REFERENCE) {
      params_obj = resolve_reference(pdf_, params->ref_object_number,
                                     params->ref_generation_number);
    } else if (params->type == Value::DICTIONARY) {
      params_obj.success = true;
      params_obj.value = *params;
    }

    if (params_obj.success && params_obj.value.type == Value::DICTIONARY) {
      const Dictionary& params_dict = params_obj.value.dict;

      // Extract Size
      auto size_it = params_dict.find("Size");
      if (size_it != params_dict.end()) {
        const Value& size_ref = size_it->second;
        ResolvedObject size_obj;

        if (size_ref.type == Value::REFERENCE) {
          size_obj = resolve_reference(pdf_, size_ref.ref_object_number,
                                       size_ref.ref_generation_number);
        } else {
          size_obj.success = true;
          size_obj.value = size_ref;
        }

        if (size_obj.success && size_obj.value.type == Value::NUMBER) {
          result.size = static_cast<size_t>(size_obj.value.number);
        }
      }

      // Extract CreationDate
      auto creation_date_it = params_dict.find("CreationDate");
      if (creation_date_it != params_dict.end()) {
        const Value& creation_date_ref = creation_date_it->second;
        ResolvedObject creation_date_obj;

        if (creation_date_ref.type == Value::REFERENCE) {
          creation_date_obj = resolve_reference(pdf_, creation_date_ref.ref_object_number,
                                               creation_date_ref.ref_generation_number);
        } else {
          creation_date_obj.success = true;
          creation_date_obj.value = creation_date_ref;
        }

        if (creation_date_obj.success && creation_date_obj.value.type == Value::STRING) {
          result.creation_date = parse_pdf_date(creation_date_obj.value.str);
        }
      }

      // Extract ModDate
      auto mod_date_it = params_dict.find("ModDate");
      if (mod_date_it != params_dict.end()) {
        const Value& mod_date_ref = mod_date_it->second;
        ResolvedObject mod_date_obj;

        if (mod_date_ref.type == Value::REFERENCE) {
          mod_date_obj = resolve_reference(pdf_, mod_date_ref.ref_object_number,
                                          mod_date_ref.ref_generation_number);
        } else {
          mod_date_obj.success = true;
          mod_date_obj.value = mod_date_ref;
        }

        if (mod_date_obj.success && mod_date_obj.value.type == Value::STRING) {
          result.modification_date = parse_pdf_date(mod_date_obj.value.str);
        }
      }

      // Extract CheckSum (MD5 hex string)
      auto checksum_it = params_dict.find("CheckSum");
      if (checksum_it != params_dict.end()) {
        const Value& checksum_ref = checksum_it->second;
        ResolvedObject checksum_obj;

        if (checksum_ref.type == Value::REFERENCE) {
          checksum_obj = resolve_reference(pdf_, checksum_ref.ref_object_number,
                                          checksum_ref.ref_generation_number);
        } else {
          checksum_obj.success = true;
          checksum_obj.value = checksum_ref;
        }

        if (checksum_obj.success && checksum_obj.value.type == Value::STRING) {
          result.checksum = checksum_to_hex(
              std::vector<uint8_t>(checksum_obj.value.str.begin(),
                                  checksum_obj.value.str.end()));
        }
      }
    }
  }

  // Decode the file stream data through its filter chain (FlateDecode etc.).
  // stream.data holds the raw (still-filtered) bytes; decode_stream applies the
  // filters (and decryption, via the object id) to recover the file contents.
  DecodedStream decoded =
      decode_stream(pdf_, stream_obj.value, stream_obj_num, stream_gen_num);
  if (decoded.success) {
    result.data = std::move(decoded.data);
  } else {
    // Unfiltered or unsupported filter: fall back to the raw bytes.
    result.data = stream_obj.value.stream.data;
  }

  // If size wasn't in Params, use actual data size
  if (result.size == 0) {
    result.size = result.data.size();
  }

  result.success = true;
  return result;
}

std::string AttachmentExtractor::get_filename(const Value& filespec) const {
  if (filespec.type != Value::DICTIONARY) {
    return "";
  }

  const Dictionary& dict = filespec.dict;

  // Prefer UF (Unicode filename) over F (legacy)
  auto uf_it = dict.find("UF");
  if (uf_it != dict.end()) {
    const Value& uf_ref = uf_it->second;
    ResolvedObject uf_obj;

    if (uf_ref.type == Value::REFERENCE) {
      uf_obj = resolve_reference(pdf_, uf_ref.ref_object_number,
                                 uf_ref.ref_generation_number);
    } else {
      uf_obj.success = true;
      uf_obj.value = uf_ref;
    }

    if (uf_obj.success && uf_obj.value.type == Value::STRING) {
      return uf_obj.value.str;
    }
  }

  auto f_it = dict.find("F");
  if (f_it != dict.end()) {
    const Value& f_ref = f_it->second;
    ResolvedObject f_obj;

    if (f_ref.type == Value::REFERENCE) {
      f_obj = resolve_reference(pdf_, f_ref.ref_object_number,
                                f_ref.ref_generation_number);
    } else {
      f_obj.success = true;
      f_obj.value = f_ref;
    }

    if (f_obj.success && f_obj.value.type == Value::STRING) {
      return f_obj.value.str;
    }
  }

  return "";
}

const Value* AttachmentExtractor::get_file_stream(const Value& filespec) const {
  if (filespec.type != Value::DICTIONARY) {
    return nullptr;
  }

  const Dictionary& dict = filespec.dict;

  // Get EF (Embedded File) dictionary
  auto ef_it = dict.find("EF");
  if (ef_it == dict.end()) {
    return nullptr;
  }

  const Value& ef_ref = ef_it->second;
  ResolvedObject ef_obj;

  if (ef_ref.type == Value::REFERENCE) {
    ef_obj = resolve_reference(pdf_, ef_ref.ref_object_number,
                               ef_ref.ref_generation_number);
  } else if (ef_ref.type == Value::DICTIONARY) {
    ef_obj.success = true;
    ef_obj.value = ef_ref;
  } else {
    return nullptr;
  }

  if (!ef_obj.success || ef_obj.value.type != Value::DICTIONARY) {
    return nullptr;
  }

  const Dictionary& ef_dict = ef_obj.value.dict;

  // Get F (file stream) entry
  auto f_it = ef_dict.find("F");
  if (f_it == ef_dict.end()) {
    return nullptr;
  }

  // Return pointer to the stream reference
  return &f_it->second;
}

const Value* AttachmentExtractor::get_params_dict(const Value& stream) const {
  if (stream.type != Value::STREAM) {
    return nullptr;
  }

  const Dictionary& stream_dict = stream.stream.dict;
  auto params_it = stream_dict.find("Params");
  if (params_it == stream_dict.end()) {
    return nullptr;
  }

  return &params_it->second;
}

std::string AttachmentExtractor::parse_pdf_date(
    const std::string& pdf_date) const {
  // PDF date format: D:YYYYMMDDHHmmSSOHH'mm'
  // Example: D:20241010154530+09'00'
  // For simplicity, return as-is
  // Could be enhanced to parse into structured format
  return pdf_date;
}

std::string AttachmentExtractor::checksum_to_hex(
    const std::vector<uint8_t>& checksum) const {
  std::string hex;
  hex.reserve(checksum.size() * 2);

  for (uint8_t byte : checksum) {
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", byte);
    hex += buf;
  }

  return hex;
}

}  // namespace nanopdf
