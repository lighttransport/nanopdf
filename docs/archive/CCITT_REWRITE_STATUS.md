# CCITT Group 4 Decoder Rewrite - Status Report

## Executive Summary

**Current Accuracy**: 100% bit-for-bit match against Poppler pdfimages for all CCITT images in keibai.pdf (222 / 222)
**Status**: Decoder logic and run tables now align with Poppler; no mismatches remain in this corpus

## What Changed Since the Last Report

### 1. Poppler Run Tables + Decode Path ✅
- Replaced legacy run tables with Poppler tables (`whiteTab1/2`, `blackTab1/2/3`, `ccittEOL`)
- Ported Poppler run lookup logic (`get_white_code`, `get_black_code`) and makeup handling
- Invalid run codes now consume 1 bit when `EndOfBlock` is true (Poppler behavior)

### 2. Bit Reader Semantics ✅
- `look_bits()` now returns partial bits if fewer than requested remain, matching Poppler's contract

### 3. Row Start + Alignment ✅
- Row start handling matches Poppler for `EncodedByteAlign` and `EndOfLine`
- Zero padding skip retained for non-EOL, non-aligned cases

## Test Results

All CCITT objects extracted from keibai.pdf match Poppler output bit-for-bit:
```
Images compared: 222
Images mismatched: 0
Bit errors: 0
```

Verification method:
- Extracted CCITT raw data + params via `pdfimages -all`
- Decoded raw CCITT using `tmp/ccitt_decode`
- Compared output against `tmp/ccitt_extract_decoded/*.pbm` (binary P4 payload)

## Files Modified
- `/mnt/nvme02/work/nanopdf/src/nanopdf.cc`
  - Poppler run tables and code lookup logic
  - `BitReader::look_bits()` partial read behavior
  - CCITT run decode / makeup loop alignment

## Next Steps

1. Add a small regression test (one or two CCITT samples under `data/`) to lock in behavior.
2. Optionally trim debug logging or gate it behind a build flag for cleaner decode runs.
