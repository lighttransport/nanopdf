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

#include "color-transform.hh"
#include "type1-parser.hh"

namespace nanopdf {

struct SharedFontEntry {
  std::vector<uint8_t> font_data;
  bool is_embedded{false};
  bool has_ttf_parse{false};
  std::vector<uint16_t> cid_to_gid;
  bool has_type1{false};
  Type1FontData type1;
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

// Process-wide cache for parsed ICC profiles.
// Keyed by a 64-bit hash of the raw ICC profile data.
// Avoids re-parsing the same ICC profile across multiple images and backends.
class SharedIccCache {
 public:
  static SharedIccCache& instance();

  // Returns true and fills |out| if the profile hash is in the cache.
  bool find(uint64_t hash, color::IccProfileInfo& out);

  // Inserts hash -> entry into the cache.
  void store(uint64_t hash, const color::IccProfileInfo& entry);

 private:
  std::unordered_map<uint64_t, color::IccProfileInfo> cache_;
  std::mutex mutex_;
};

// Process-wide cache for font fallback search results.
// Keyed by font name, stores the resolved file path (or empty string if not found).
// Avoids repeated filesystem probes for the same font name across backends.
class SharedFontFallbackCache {
 public:
  static SharedFontFallbackCache& instance();

  // Returns true and fills |resolved_path| if the font name is in the cache.
  // resolved_path is empty string if the font was previously not found.
  bool find(const std::string& font_name, std::string& resolved_path);

  // Inserts font_name -> resolved_path into the cache.
  void store(const std::string& font_name, const std::string& resolved_path);

 private:
  struct Key {
    std::string name;
    bool operator==(const Key& o) const { return name == o.name; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return std::hash<std::string>{}(k.name);
    }
  };
  std::unordered_map<Key, std::string, KeyHash> cache_;
  std::mutex mutex_;
};

}  // namespace nanopdf
