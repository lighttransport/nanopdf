// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Render cache: LRU-evicted byte-size-bounded cache for rendered tiles,
// glyphs, and images.  Shared process-wide across all backend instances.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nanopdf {

struct RenderCacheEntry {
  std::vector<uint8_t> data;  // cached pixel or bitmap data
  uint32_t width{0};          // pixel width (0 = metadata-only)
  uint32_t height{0};          // pixel height (0 = metadata-only)
  uint32_t stride{0};          // bytes per row (0 = packed)
};

class RenderCache {
 public:
  static RenderCache& instance();

  // Maximum cache size in bytes (default 128 MB).
  void set_max_size(size_t bytes);

  // Current max size.
  size_t max_size() const;

  // Current total bytes used.
  size_t used_size() const;

  // Look up a key.  Returns true and fills |out| on hit.
  bool find(const std::string& key, RenderCacheEntry& out);

  // Insert or replace an entry.  Evicts LRU entries if the total
  // would exceed max_size.
  void store(const std::string& key, RenderCacheEntry&& entry);

  // Remove a single entry.  Returns true if found.
  bool erase(const std::string& key);

  // Remove all entries.
  void clear();

  // Statistics.
  struct Stats {
    size_t hits{0};
    size_t misses{0};
    size_t evictions{0};
    size_t entries{0};
    size_t bytes_used{0};
  };
  Stats stats() const;

 private:
  RenderCache() = default;

  void evict_to_fit(size_t needed);

  struct Item {
    std::string key;
    RenderCacheEntry entry;
    size_t approx_bytes{0};  // entry.data.size() + overhead
  };

  // LRU list: front = most recent, back = least recent
  using LruList = std::list<Item>;
  using LruMap = std::unordered_map<std::string, LruList::iterator>;

  LruList lru_;
  LruMap map_;
  size_t max_size_{128 * 1024 * 1024};  // 128 MB default
  size_t used_{0};
  mutable std::mutex mutex_;

  // Counters (not under mutex for speed — approximate)
  mutable size_t hits_{0};
  mutable size_t misses_{0};
  mutable size_t evictions_{0};
};

}  // namespace nanopdf