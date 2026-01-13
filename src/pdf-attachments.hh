// Copyright 2017 The PDFium Authors
// Copyright 2026 nanopdf Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. Original code copyright 2014 Foxit Software Inc.
// http://www.foxitsoftware.com
//
// Ported to nanopdf from PDFium with modifications for C++11 compatibility
// and integration with nanopdf architecture.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nanopdf {

// Forward declarations
struct Pdf;
struct Value;

// Represents a single file attachment extracted from a PDF
struct FileAttachment {
  std::string name;              // Filename (UF or F key)
  std::string description;       // Desc entry (optional)
  std::string mime_type;         // Subtype entry (e.g., "application/pdf")
  std::string checksum;          // MD5 CheckSum as hex string
  std::vector<uint8_t> data;     // Decoded file content

  // Metadata from Params dictionary
  std::string creation_date;     // CreationDate (D:YYYYMMDDHHmmSS format)
  std::string modification_date; // ModDate
  size_t size{0};                // Uncompressed size in bytes

  // PDF 2.0 relationship (optional)
  std::string relationship;      // AFRelationship (Source, Data, Alternative, etc.)

  // Status
  bool success{false};
  std::string error;
};

// Extracts file attachments from PDF portfolios and embedded files
class AttachmentExtractor {
 public:
  explicit AttachmentExtractor(const Pdf& pdf);
  ~AttachmentExtractor();

  // Get total number of attachments in the document
  // Returns 0 if no EmbeddedFiles name tree exists
  int get_count() const;

  // Get attachment by index (0-based)
  // Returns FileAttachment with success=false if index out of range
  FileAttachment get_attachment(int index) const;

  // Get attachment by exact name match
  // Returns FileAttachment with success=false if not found
  FileAttachment get_attachment_by_name(const std::string& name) const;

  // List all attachment names in the document
  // Returns empty vector if no attachments
  std::vector<std::string> list_names() const;

 private:
  const Pdf& pdf_;

  // Internal helpers
  struct NameTreeNode {
    std::vector<std::pair<std::string, const Value*>> entries;
    std::vector<NameTreeNode> kids;
  };

  // Parse the EmbeddedFiles name tree from /Catalog/Names/EmbeddedFiles
  bool parse_name_tree(NameTreeNode& root) const;

  // Recursively collect all name-value pairs from name tree
  void collect_all_entries(const NameTreeNode& node,
                          std::vector<std::pair<std::string, const Value*>>& out) const;

  // Parse a single filespec dictionary into FileAttachment
  FileAttachment parse_filespec(const Value& filespec_value) const;

  // Extract filename from filespec (prefers UF over F)
  std::string get_filename(const Value& filespec) const;

  // Get the embedded file stream from filespec's /EF/F entry
  const Value* get_file_stream(const Value& filespec) const;

  // Get Params dictionary from embedded file stream
  const Value* get_params_dict(const Value& stream) const;

  // Convert PDF date string (D:YYYYMMDDHHmmSS) to readable format
  std::string parse_pdf_date(const std::string& pdf_date) const;

  // Convert binary checksum to hex string
  std::string checksum_to_hex(const std::vector<uint8_t>& checksum) const;
};

}  // namespace nanopdf
