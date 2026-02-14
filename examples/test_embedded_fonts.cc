// Test program for embedded fonts functionality (with zlib compression)
// Compile with: g++ -std=c++14 -I../build -lz -o test_embedded_fonts test_embedded_fonts.cc
// (Requires NANOPDF_EMBED_FONTS=ON when building nanopdf)

#include <iostream>
#include <iomanip>
#include <chrono>

#ifdef NANOPDF_EMBED_FONTS
#include "embedded-fonts.hh"
#else
#error "This test requires NANOPDF_EMBED_FONTS to be enabled"
#endif

using namespace nanopdf::embedded_fonts;

void print_separator() {
  std::cout << std::string(80, '=') << "\n";
}

int main() {
  std::cout << "Embedded Fonts Test (with zlib Compression)\n";
  print_separator();

  // Test 1: List all embedded fonts with compression stats
  std::cout << "\nTest 1: Listing all embedded fonts\n";
  std::cout << "Total fonts available: " << font_count << "\n\n";

  size_t total_original = 0;
  size_t total_compressed = 0;

  for (size_t i = 0; i < font_count; ++i) {
    const auto& entry = font_registry[i];
    float ratio = 100.0f * (1.0f - (float)entry.compressed_size / entry.original_size);

    std::cout << std::setw(3) << (i + 1) << ". "
              << std::setw(30) << std::left << entry.filename
              << "  Original: " << std::setw(7) << std::right << entry.original_size
              << "  Compressed: " << std::setw(7) << entry.compressed_size
              << "  (" << std::fixed << std::setprecision(1) << ratio << "% reduction)\n";

    total_original += entry.original_size;
    total_compressed += entry.compressed_size;
  }

  float total_ratio = 100.0f * (1.0f - (float)total_compressed / total_original);
  std::cout << "\nTotals:\n";
  std::cout << "  Original size:    " << (total_original / 1024.0 / 1024.0) << " MB\n";
  std::cout << "  Compressed size:  " << (total_compressed / 1024.0 / 1024.0) << " MB\n";
  std::cout << "  Space saved:      " << ((total_original - total_compressed) / 1024.0 / 1024.0) << " MB\n";
  std::cout << "  Compression:      " << total_ratio << "%\n";

  // Test 2: Find fonts by base name
  print_separator();
  std::cout << "\nTest 2: Finding fonts by base name\n";

  const char* test_names[] = {
    "Arimo-Regular",
    "Tinos-Bold",
    "Cousine-Italic",
    "STIXTwoMath-Regular",
    "NotoSansSymbols-Regular",
    "NonExistent-Font"  // Should fail
  };

  for (const char* name : test_names) {
    const auto* font = find_font(name);
    if (font) {
      std::cout << "✓ Found: " << name << " -> " << font->filename
                << " (compressed: " << font->compressed_size << " bytes)\n";
    } else {
      std::cout << "✗ Not found: " << name << "\n";
    }
  }

  // Test 3: PDF Standard 14 mapping
  print_separator();
  std::cout << "\nTest 3: PDF Standard 14 font mapping\n";
  std::cout << "Total mappings: " << pdf_mapping_count << "\n\n";

  for (size_t i = 0; i < pdf_mapping_count; ++i) {
    const auto& mapping = pdf_standard_14_mapping[i];
    std::cout << std::setw(25) << std::left << mapping.pdf_name
              << " -> " << mapping.substitute_name << "\n";
  }

  // Test 4: Get PDF Standard fonts
  print_separator();
  std::cout << "\nTest 4: Getting PDF Standard fonts\n";

  const char* pdf_names[] = {
    "Helvetica",
    "Helvetica-Bold",
    "Times-Roman",
    "Courier-BoldOblique",
    "Symbol",
    "ZapfDingbats",
    "UnknownFont"  // Should fail
  };

  for (const char* pdf_name : pdf_names) {
    const auto* font = get_pdf_standard_font(pdf_name);
    if (font) {
      std::cout << "✓ " << std::setw(25) << std::left << pdf_name
                << " -> " << font->base_name
                << " (" << font->compressed_size << " bytes compressed)\n";
    } else {
      std::cout << "✗ " << pdf_name << " - not found\n";
    }
  }

  // Test 5: Decompression test
  print_separator();
  std::cout << "\nTest 5: Font decompression test\n";

  const char* test_font = "Arimo-Regular";
  const auto* entry = find_font(test_font);

  if (!entry) {
    std::cout << "✗ Could not find test font: " << test_font << "\n";
    return 1;
  }

  std::cout << "Testing decompression of: " << entry->filename << "\n";
  std::cout << "  Compressed size: " << entry->compressed_size << " bytes\n";
  std::cout << "  Expected size: " << entry->original_size << " bytes\n";

  // Test vector decompression
  std::vector<uint8_t> decompressed_data;

  auto start = std::chrono::high_resolution_clock::now();
  bool success = decompress_font(entry, decompressed_data);
  auto end = std::chrono::high_resolution_clock::now();

  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  if (success) {
    std::cout << "✓ Decompression successful\n";
    std::cout << "  Decompressed size: " << decompressed_data.size() << " bytes\n";
    std::cout << "  Decompression time: " << (duration.count() / 1000.0) << " ms\n";

    if (decompressed_data.size() == entry->original_size) {
      std::cout << "✓ Size matches expected size\n";
    } else {
      std::cout << "✗ Size mismatch!\n";
      return 1;
    }

    // Verify font signature (TTF or OTF)
    if (decompressed_data.size() >= 4) {
      bool is_ttf = (decompressed_data[0] == 0x00 && decompressed_data[1] == 0x01 &&
                     decompressed_data[2] == 0x00 && decompressed_data[3] == 0x00);

      bool is_otf = (decompressed_data[0] == 0x4F && decompressed_data[1] == 0x54 &&
                     decompressed_data[2] == 0x54 && decompressed_data[3] == 0x4F);

      if (is_ttf || is_otf) {
        std::cout << "✓ Valid " << (is_ttf ? "TTF" : "OTF") << " signature detected\n";
      } else {
        std::cout << "✗ Invalid font signature\n";
        return 1;
      }
    }
  } else {
    std::cout << "✗ Decompression failed\n";
    return 1;
  }

  // Test buffer decompression
  std::cout << "\nTesting buffer decompression...\n";
  auto buffer = new unsigned char[entry->original_size];

  start = std::chrono::high_resolution_clock::now();
  success = decompress_font_to_buffer(entry, buffer, entry->original_size);
  end = std::chrono::high_resolution_clock::now();

  duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  if (success) {
    std::cout << "✓ Buffer decompression successful\n";
    std::cout << "  Decompression time: " << (duration.count() / 1000.0) << " ms\n";

    // Verify data matches
    if (memcmp(buffer, decompressed_data.data(), entry->original_size) == 0) {
      std::cout << "✓ Buffer matches vector decompression\n";
    } else {
      std::cout << "✗ Buffer data mismatch\n";
      delete[] buffer;
      return 1;
    }
  } else {
    std::cout << "✗ Buffer decompression failed\n";
    delete[] buffer;
    return 1;
  }

  delete[] buffer;

  // Test 6: Decompress all fonts and measure performance
  print_separator();
  std::cout << "\nTest 6: Batch decompression performance test\n";

  size_t total_decompress_time = 0;
  bool all_valid = true;

  for (size_t i = 0; i < font_count; ++i) {
    const auto& entry = font_registry[i];

    std::vector<uint8_t> data;

    start = std::chrono::high_resolution_clock::now();
    success = decompress_font(&entry, data);
    end = std::chrono::high_resolution_clock::now();

    duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    total_decompress_time += duration.count();

    if (!success || data.size() != entry.original_size) {
      std::cout << "✗ " << entry.filename << " decompression failed\n";
      all_valid = false;
      continue;
    }

    // Verify font signature
    if (data.size() >= 4) {
      bool is_ttf = (data[0] == 0x00 && data[1] == 0x01 &&
                     data[2] == 0x00 && data[3] == 0x00);

      bool is_otf = (data[0] == 0x4F && data[1] == 0x54 &&
                     data[2] == 0x54 && data[3] == 0x4F);

      if (!is_ttf && !is_otf) {
        std::cout << "✗ " << entry.filename << " has invalid signature\n";
        all_valid = false;
      }
    }
  }

  if (all_valid) {
    std::cout << "✓ All " << font_count << " fonts decompressed successfully\n";
    std::cout << "  Total decompression time: " << (total_decompress_time / 1000.0) << " ms\n";
    std::cout << "  Average per font: " << (total_decompress_time / font_count / 1000.0) << " ms\n";
  }

  // Summary
  print_separator();
  std::cout << "\nTest Summary:\n";
  std::cout << "  Fonts embedded: " << font_count << "\n";
  std::cout << "  PDF mappings: " << pdf_mapping_count << "\n";
  std::cout << "  Total compressed size: " << std::fixed << std::setprecision(2)
            << (total_compressed / 1024.0 / 1024.0) << " MB\n";
  std::cout << "  Compression ratio: " << total_ratio << "%\n";
  std::cout << "  Decompression: " << (all_valid ? "PASS" : "FAIL") << "\n";

  print_separator();

  return all_valid ? 0 : 1;
}
