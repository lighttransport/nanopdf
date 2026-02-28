// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Font provider for runtime font registration (delayed/on-demand loading)

#ifndef NANOPDF_FONT_PROVIDER_HH
#define NANOPDF_FONT_PROVIDER_HH

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nanopdf {

enum class FontCategory {
  kSans = 0,
  kMono = 1,
  kSerif = 2,
  kSymbol = 3,
  kCJKSans = 4,
  kCJKSerif = 5
};

struct ProvidedFont {
  std::string name;
  FontCategory category;
  int weight = 400;    // CSS font weight 100-900
  bool italic = false;
  std::vector<uint8_t> data;
};

class FontProvider {
 public:
  static FontProvider& instance();

  // Register a font from a memory blob (copies data)
  bool register_font_blob(const std::string& name, FontCategory category,
                          const uint8_t* data, size_t size);

  // Register a font from a memory blob with weight and italic (copies data)
  bool register_font_blob(const std::string& name, FontCategory category,
                          int weight, bool italic,
                          const uint8_t* data, size_t size);

  // Register a font from a file path (reads and stores data)
  bool register_font_file(const std::string& name, FontCategory category,
                          const std::string& file_path);

  // Find a registered font by category (returns first match, or nullptr)
  const ProvidedFont* find_by_category(FontCategory cat) const;

  // Find best matching font by category + weight + italic.
  // Uses CSS font matching rules (prefer italic match, then closest weight).
  const ProvidedFont* find_best_match(FontCategory cat, int weight,
                                      bool italic) const;

  // Find a registered font by name (returns match, or nullptr)
  const ProvidedFont* find_by_name(const std::string& name) const;

  // Check if any CJK fonts are registered
  bool has_cjk_fonts() const;

  // Remove all registered fonts
  void clear();

 private:
  FontProvider() = default;
  FontProvider(const FontProvider&) = delete;
  FontProvider& operator=(const FontProvider&) = delete;

  std::map<FontCategory, std::vector<ProvidedFont>> fonts_;
  std::map<std::string, const ProvidedFont*> by_name_;
};

}  // namespace nanopdf

#endif  // NANOPDF_FONT_PROVIDER_HH
