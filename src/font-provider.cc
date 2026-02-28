// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Font provider implementation

#include "font-provider.hh"

#include <cstdlib>
#include <fstream>

namespace nanopdf {

FontProvider& FontProvider::instance() {
  static FontProvider provider;
  return provider;
}

bool FontProvider::register_font_blob(const std::string& name,
                                      FontCategory category,
                                      const uint8_t* data, size_t size) {
  return register_font_blob(name, category, 400, false, data, size);
}

bool FontProvider::register_font_blob(const std::string& name,
                                      FontCategory category,
                                      int weight, bool italic,
                                      const uint8_t* data, size_t size) {
  if (!data || size == 0) return false;

  auto& list = fonts_[category];
  list.emplace_back();
  ProvidedFont& pf = list.back();
  pf.name = name;
  pf.category = category;
  pf.weight = weight;
  pf.italic = italic;
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

const ProvidedFont* FontProvider::find_best_match(FontCategory cat, int weight,
                                                   bool italic) const {
  auto it = fonts_.find(cat);
  if (it == fonts_.end() || it->second.empty()) {
    return nullptr;
  }

  const auto& candidates = it->second;

  // 1. Filter to italic-matching candidates
  std::vector<const ProvidedFont*> matched;
  for (const auto& f : candidates) {
    if (f.italic == italic) matched.push_back(&f);
  }
  // Fall back to all candidates if no italic match
  if (matched.empty()) {
    for (const auto& f : candidates) {
      matched.push_back(&f);
    }
  }

  // 2. Find closest weight by absolute distance
  const ProvidedFont* best = matched[0];
  int best_dist = std::abs(best->weight - weight);
  for (size_t i = 1; i < matched.size(); ++i) {
    int dist = std::abs(matched[i]->weight - weight);
    if (dist < best_dist) {
      best = matched[i];
      best_dist = dist;
    } else if (dist == best_dist) {
      // Tie-break: prefer bolder for weight >= 500, lighter for < 500
      if (weight >= 500) {
        if (matched[i]->weight > best->weight) best = matched[i];
      } else {
        if (matched[i]->weight < best->weight) best = matched[i];
      }
    }
  }

  return best;
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
