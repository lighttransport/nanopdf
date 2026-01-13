# CCITT Group 3/4 decode logic (Poppler-aligned)

This file is the pre-implementation summary and logic reference for the
nanopdf CCITT decoder rewrite. It distills the CCITT T.4/T.6 spec through
Poppler's exact implementation in:

- `ref/poppler/poppler/Stream.cc` (class `CCITTFaxStream`)
- `ref/poppler/poppler/Stream-CCITT.h` (CCITT tables)

The purpose is to lock in the exact algorithm and error semantics before
making any decoder changes.

## 1) Core state and invariants

Poppler represents each row as a transition list:

- `codingLine[]`: transitions for current row, strictly increasing,
  terminated by `columns` (width).
- `refLine[]`: transitions for previous row, terminated by `columns`,
  with one extra sentinel entry (size `columns + 2`).

Invariant during 2D decoding:

- `refLine[b1i - 1] <= codingLine[a0i] < refLine[b1i] < refLine[b1i + 1]`
- Left edge exception: `codingLine[0] = refLine[0] = 0` is allowed.
- Right edge exception: `refLine[b1i] = refLine[b1i + 1] = columns` allowed.

`refLine` is rebuilt every row from the **previous decoded row's transitions**:

1. Copy `codingLine[i]` while `< columns`.
2. Fill the rest with `columns` up to `columns + 1`.

## 2) Bit reader behavior (critical)

Poppler's `lookBits(n)` is **not** a strict "must have n bits" peek:

- If no bits remain: return `EOF`.
- If some bits remain (< n): return partial bits **left-shifted** to width `n`.

This partial return affects:
- 2D mode decoding (it will still match a shorter code).
- Padding detection (e.g., `lookBits(12) == 0`).

## 3) Run-length decode (Poppler exact)

Poppler has specialized lookup paths depending on `EndOfBlock`.

### 3.1 White run code (Poppler `getWhiteCode`)

If `EndOfBlock` is true:
1. `code = lookBits(12)`.
2. If `code == EOF`, return `1`.
3. If `(code >> 5) == 0`, use `whiteTab1[code]` (11-12 bit space).
4. Else use `whiteTab2[code >> 3]` (1-9 bit space).
5. If `p->bits > 0`, `eatBits(p->bits)` and return `p->n`.

If `EndOfBlock` is false:
1. For `n = 1..9`:
   - `code = lookBits(n)`; if EOF, return `1`.
   - If `n < 9`, shift `code <<= 9 - n`.
   - Use `whiteTab2[code]`; if `p->bits == n`, eat and return `p->n`.
2. For `n = 11..12`:
   - `code = lookBits(n)`; if EOF, return `1`.
   - If `n < 12`, shift `code <<= 12 - n`.
   - Use `whiteTab1[code]`; if `p->bits == n`, eat and return `p->n`.
3. If no match: log error, eat 1 bit, return `1`.

### 3.2 Black run code (Poppler `getBlackCode`)

If `EndOfBlock` is true:
1. `code = lookBits(13)`; if EOF, return `1`.
2. If `(code >> 7) == 0`, use `blackTab1[code]`.
3. Else if `(code >> 9) == 0 && (code >> 7) != 0`, use
   `blackTab2[(code >> 1) - 64]`.
4. Else use `blackTab3[code >> 7]`.
5. If `p->bits > 0`, eat and return `p->n`.

If `EndOfBlock` is false:
1. For `n = 2..6`:
   - `code = lookBits(n)`; if EOF, return `1`.
   - If `n < 6`, shift `code <<= 6 - n`.
   - Use `blackTab3[code]`; if `p->bits == n`, eat and return `p->n`.
2. For `n = 7..12`:
   - `code = lookBits(n)`; if EOF, return `1`.
   - If `n < 12`, shift `code <<= 12 - n`.
   - If `code >= 64`, use `blackTab2[code - 64]`; if `p->bits == n`, eat/return.
3. For `n = 10..13`:
   - `code = lookBits(n)`; if EOF, return `1`.
   - If `n < 13`, shift `code <<= 13 - n`.
   - Use `blackTab1[code]`; if `p->bits == n`, eat/return.
4. If no match: log error, eat 1 bit, return `1`.

### 3.3 Run-length assembly

Poppler accumulates makeup codes (>= 64) until a terminating code (< 64)
is found. It returns the total run length and does not fail hard on errors.

## 4) 2D mode decoding (T.6 / T.4 2D)

For each 2D row:

1. `codingLine[0] = 0`, `a0i = 0`, `b1i = 0`, `blackPixels = 0`.
2. Loop while `codingLine[a0i] < columns` and not `err`:
   - `code = getTwoDimCode()` (lookup on `twoDimTab1`).
   - Modes:
     - Pass: `addPixels(refLine[b1i + 1], blackPixels)`; if `< columns`,
       `b1i += 2`.
     - Horizontal:
       - Decode two runs (current color, then opposite) using
         `getWhiteCode` / `getBlackCode`, accumulating makeup+term.
       - `addPixels(codingLine[a0i] + run1, blackPixels)`.
       - If still in row, `addPixels(codingLine[a0i] + run2, blackPixels ^ 1)`.
       - Update `b1i` while `refLine[b1i] <= codingLine[a0i] && refLine[b1i] < columns`.
     - Vertical (delta in {-3..+3}):
       - target = `refLine[b1i] + delta`.
       - If delta < 0, use `addPixelsNeg` (can move `a0i` backward).
       - Toggle `blackPixels`.
       - Update `b1i`:
         - delta < 0: `b1i--` if `b1i > 0`, else `b1i++`.
         - delta >= 0: `b1i++`.
         - Then skip while `refLine[b1i] <= codingLine[a0i]`.
   - Invalid 2D code: fill rest of row (`addPixels(columns, 0)`), set `err`.

Notes:
- H-mode does **not** toggle `blackPixels` (two runs cancel).
- `addPixels` and `addPixelsNeg` use parity-aware logic to decide whether
  to overwrite or advance `a0i`.

## 5) 1D mode decoding (T.4 1D)

For each 1D row:

- `codingLine[0] = 0`, `a0i = 0`, `blackPixels = 0`.
- While `codingLine[a0i] < columns`:
  - Decode a run in current color using `getWhiteCode` / `getBlackCode`.
  - `addPixels(codingLine[a0i] + run, blackPixels)`.
  - Toggle `blackPixels`.

## 6) End-of-line, byte alignment, and padding

After each row:

- If `EndOfLine` is true:
  - Scan until 12-bit EOL `000000000001` found, then `eatBits(12)`.
- If `EndOfLine` is false and `EncodedByteAlign` is false:
  - While `lookBits(12) == 0`, eat 1 bit (zero padding).
- If `EncodedByteAlign` is true and no EOL was consumed:
  - Align to next byte boundary.

End-of-block (RTC) logic:

- Only checked when `EndOfBlock` is true. Poppler looks for RTC after
  EOL sequences under specific parameter combinations.

## 7) Output generation

Poppler outputs bytes by walking `codingLine`:

- If `codingLine[0] > 0`, row starts white (`a0i = 0`).
- If `codingLine[0] == 0`, row starts black (`a0i = 1`).
- Output bits are emitted MSB-first; `BlackIs1` inverts output bytes.

## 8) Implementation plan (nanopdf)

Before any heuristics:

1. Port Poppler run decode logic **exactly** using the tables in
   `ref/poppler/poppler/Stream-CCITT.h`:
   - `getWhiteCode` / `getBlackCode` with the same lookup path and
     `EndOfBlock` branching.
2. Preserve Poppler error semantics:
   - Invalid run: eat 1 bit, return `1`, do not treat as fatal.
   - EOF mid-code: return `1`.
3. Ensure `lookBits` returns partial bits near EOF.
4. Keep 2D mode logic identical to Poppler (pass/horiz/vert update rules).

Only after parity is achieved should any resync or heuristic logic be
considered.
