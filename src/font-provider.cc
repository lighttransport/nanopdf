// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Font provider implementation

#include "font-provider.hh"

#include <fstream>

namespace nanopdf {

FontProvider& FontProvider::instance() {
  static FontProvider provider;
  return provider;
}

bool FontProvider::register_font_blob(const std::string& name,
                                      FontCategory category,
                                      const uint8_t* data, size_t size) {
  if (!data || size == 0) return false;

  auto& list = fonts_[category];
  list.emplace_back();
  ProvidedFont& pf = list.back();
  pf.name = name;
  pf.category = category;
  pf.data.assign(data, data + size);

  by_name_[name] = &pf;
  return true;
}

bool FontProvider::register_font_file(const std::string& name,
                                      FontCategory category,
                                      const std::string& file_path) {
  std::ifstream file(file_path, std::ios::binary | std::ios::ate);
  if (!file) return false;

  auto size = file.tellg();
  if (size <= 0) return false;

  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(data.data()), size);
  if (!file) return false;

  return register_font_blob(name, category, data.data(), data.size());
}

const ProvidedFont* FontProvider::find_by_category(FontCategory cat) const {
  auto it = fonts_.find(cat);
  if (it != fonts_.end() && !it->second.empty()) {
    return &it->second.front();
  }
  return nullptr;
}

const ProvidedFont* FontProvider::find_by_name(const std::string& name) const {
  auto it = by_name_.find(name);
  if (it != by_name_.end()) {
    return it->second;
  }
  return nullptr;
}

bool FontProvider::has_cjk_fonts() const {
  return find_by_category(FontCategory::kCJKSans) != nullptr ||
         find_by_category(FontCategory::kCJKSerif) != nullptr;
}

void FontProvider::clear() {
  fonts_.clear();
  by_name_.clear();
}

}  // namespace nanopdf
