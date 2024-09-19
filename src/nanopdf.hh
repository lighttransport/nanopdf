// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstddef.h"
#include "nanocstdint.h"
using namespace nanostl;
#else
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <map>
#endif

namespace nanopdf {

struct Value
{
  std::vector<Value> _arr;
  std::map<std::string, Value> _dict;

  bool _b{false};
  std::vector<uint8_t> _s{false}; // string in bytes
  double _v{0.0}; // int, real
  std::string _n; // Name(Identifier)
};

using Dictionary = std::map<std::string, Value>;

struct Pdf
{
  int version_major{1};
  int version_minor{5};
};

struct XRef {
  uint64_t offset{0};
  uint16_t generation{65535};
  bool use{false};
};


struct XRefSection
{
  std::vector<XRef> xrefs;
  uint32_t start_object_id{0};
  uint32_t num_objectsid{0};
};

bool parse_from_memory(const uint8_t *addr, const size_t size);


} // namespace nanopdf
