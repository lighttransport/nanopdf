// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Render cache: LRU-evicted byte-size-bounded cache implementation.

#include "render-cache.hh"

namespace nanopdf {

RenderCache& RenderCache::instance() {
  static RenderCache cache;
  return cache;
}

void RenderCache::set_max_size(size_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  max_size_ = bytes;
  evict_to_fit(0);
}

size_t RenderCache::max_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return max_size_;
}

size_t RenderCache::used_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return used_;
}

bool RenderCache::find(const std::string& key, RenderCacheEntry& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = map_.find(key);
  if (it == map_.end()) {
    ++misses_;
    return false;
  }
  // Move to front of LRU (most recently used)
  lru_.splice(lru_.begin(), lru_, it->second);
  out = it->second->entry;
  ++hits_;
  return true;
}

void RenderCache::store(const std::string& key, RenderCacheEntry&& entry) {
  size_t entry_bytes = entry.data.size() + sizeof(RenderCacheEntry) + key.size() + 64;

  std::lock_guard<std::mutex> lock(mutex_);

  // If key already exists, remove old entry
  auto it = map_.find(key);
  if (it != map_.end()) {
    used_ -= it->second->approx_bytes;
    lru_.erase(it->second);
    map_.erase(it);
  }

  // Evict until there is room
  evict_to_fit(entry_bytes);

  // Insert at front of LRU
  lru_.push_front({key, std::move(entry), entry_bytes});
  map_[lru_.front().key] = lru_.begin();
  used_ += entry_bytes;
}

bool RenderCache::erase(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = map_.find(key);
  if (it == map_.end()) return false;
  used_ -= it->second->approx_bytes;
  lru_.erase(it->second);
  map_.erase(it);
  return true;
}

void RenderCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  lru_.clear();
  map_.clear();
  used_ = 0;
}

RenderCache::Stats RenderCache::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats s;
  s.hits = hits_;
  s.misses = misses_;
  s.evictions = evictions_;
  s.entries = map_.size();
  s.bytes_used = used_;
  return s;
}

void RenderCache::evict_to_fit(size_t needed) {
  while (used_ + needed > max_size_ && !lru_.empty()) {
    auto& back = lru_.back();
    used_ -= back.approx_bytes;
    map_.erase(back.key);
    lru_.pop_back();
    ++evictions_;
  }
}

}  // namespace nanopdf