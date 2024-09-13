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
#endif

namespace nanopdf {

struct Pdf
{
  int version_major{1};
  int version_minor{5};
};

bool parse_from_memory(const uint8_t *addr, const size_t size);


} // namespace nanopdf
