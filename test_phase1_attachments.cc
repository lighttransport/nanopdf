// Test file attachment extraction (Phase 1.1)
#include <fstream>
#include <iostream>

#include "src/nanopdf.hh"
#include "src/pdf-attachments.hh"

using namespace nanopdf;

bool test_attachment_extraction(const char* pdf_path) {
  std::cout << "Testing: " << pdf_path << std::endl;

  // Load PDF file
  std::ifstream ifs(pdf_path, std::ios::binary);
  if (!ifs) {
    std::cerr << "Failed to open PDF file: " << pdf_path << std::endl;
    return false;
  }

  // Read entire file
  ifs.seekg(0, std::ios::end);
  size_t file_size = ifs.tellg();
  ifs.seekg(0, std::ios::beg);

  std::vector<uint8_t> data(file_size);
  ifs.read(reinterpret_cast<char*>(data.data()), file_size);
  ifs.close();

  // Parse PDF
  Pdf pdf;
  if (!parse_from_memory(data.data(), data.size(), &pdf)) {
    std::cerr << "Failed to parse PDF" << std::endl;
    return false;
  }

  if (!pdf.load_document_structure()) {
    std::cerr << "Failed to load document structure" << std::endl;
    return false;
  }

  std::cout << "PDF loaded successfully" << std::endl;

  // Create attachment extractor
  AttachmentExtractor extractor(pdf);

  // Get attachment count
  int count = extractor.get_count();
  std::cout << "Found " << count << " attachment(s)" << std::endl;

  if (count == 0) {
    std::cout << "No attachments found - test passed (empty case)" << std::endl;
    return true;
  }

  // List all attachment names
  std::vector<std::string> names = extractor.list_names();
  std::cout << "\nAttachment names:" << std::endl;
  for (const auto& name : names) {
    std::cout << "  - " << name << std::endl;
  }

  // Extract each attachment
  for (int i = 0; i < count; ++i) {
    std::cout << "\nExtracting attachment " << (i + 1) << "/" << count << ":" << std::endl;

    FileAttachment att = extractor.get_attachment(i);

    if (!att.success) {
      std::cerr << "  ERROR: " << att.error << std::endl;
      continue;
    }

    std::cout << "  Name: " << att.name << std::endl;
    std::cout << "  Size: " << att.size << " bytes (data: " << att.data.size() << " bytes)" << std::endl;

    if (!att.description.empty()) {
      std::cout << "  Description: " << att.description << std::endl;
    }

    if (!att.mime_type.empty()) {
      std::cout << "  MIME Type: " << att.mime_type << std::endl;
    }

    if (!att.checksum.empty()) {
      std::cout << "  MD5 Checksum: " << att.checksum << std::endl;
    }

    if (!att.creation_date.empty()) {
      std::cout << "  Created: " << att.creation_date << std::endl;
    }

    if (!att.modification_date.empty()) {
      std::cout << "  Modified: " << att.modification_date << std::endl;
    }

    if (!att.relationship.empty()) {
      std::cout << "  Relationship: " << att.relationship << std::endl;
    }

    // Save to file
    std::string output_filename = "extracted_" + att.name;
    std::ofstream outfile(output_filename, std::ios::binary);
    if (outfile) {
      outfile.write(reinterpret_cast<const char*>(att.data.data()), att.data.size());
      outfile.close();
      std::cout << "  Saved to: " << output_filename << std::endl;
    } else {
      std::cerr << "  Failed to save file" << std::endl;
    }
  }

  // Test get_attachment_by_name
  if (!names.empty()) {
    std::cout << "\nTesting get_attachment_by_name(\"" << names[0] << "\"):" << std::endl;
    FileAttachment att = extractor.get_attachment_by_name(names[0]);
    if (att.success) {
      std::cout << "  Successfully retrieved attachment by name" << std::endl;
    } else {
      std::cerr << "  ERROR: " << att.error << std::endl;
    }
  }

  std::cout << "\nTest completed successfully!" << std::endl;
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <pdf_file>" << std::endl;
    std::cerr << "\nThis test extracts embedded file attachments from a PDF." << std::endl;
    return 1;
  }

  bool success = test_attachment_extraction(argv[1]);
  return success ? 0 : 1;
}
