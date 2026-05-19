// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Shared font cache across all rendering backends.
//
// Font data (raw bytes decoded from PDF streams or loaded from disk) can be
// large and expensive to produce.  This cache ensures that the same font is
// loaded exactly once per Pdf document, even when multiple backends are used
// (e.g. LightVG first, then ThorVG) or the same page is rendered twice.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nanopdf {

struct SharedFontEntry {
  std::vector<uint8_t> font_data;
  bool is_embedded{false};
  bool has_ttf_parse{false};
  std::vector<uint16_t> cid_to_gid;
};

class SharedFontCache {
 public:
  static SharedFontCache& instance();

  // Returns true and fills |out| if (pdf, font_name) is in the cache.
  bool find(const void* pdf, const std::string& font_name,
            SharedFontEntry& out);

  // Inserts (pdf, font_name) -> entry into the cache.
  void store(const void* pdf, const std::string& font_name,
             SharedFontEntry&& entry);

 private:
  struct Key {
    const void* pdf;
    std::string name;

    bool operator==(const Key& o) const {
      return pdf == o.pdf && name == o.name;
    }
  };

  struct KeyHash {
    size_t operator()(const Key& k) const {
      size_t h = std::hash<const void*>{}(k.pdf);
      h ^= std::hash<std::string>{}(k.name) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  std::unordered_map<Key, SharedFontEntry, KeyHash> cache_;
  std::mutex mutex_;
};

}  // namespace nanopdf
