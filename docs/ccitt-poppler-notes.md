# Poppler CCITTFaxStream logic (deep notes)

These notes distill the CCITT decode path from Poppler's `CCITTFaxStream`
implementation in `ref/poppler/poppler/Stream.cc`. The goal is bit-exact
parity, so the details below focus on the control flow and "edge" behaviors
that affect bit consumption.

## Core data structures and invariants
- `codingLine[]` stores transition positions for the current row. It is
  strictly increasing and ends with `columns` (width).
- `refLine[]` stores transitions from the previous row, plus a sentinel
  entry at the end. It is rebuilt every row from `codingLine`:
  - Copy entries while `codingLine[i] < columns`, then fill the rest with
    `columns`.
  - Left edge exception: `codingLine[0] == 0` and `refLine[0] == 0` is
    allowed (row starts with black).
  - Right edge exception: `refLine[b1i] == refLine[b1i + 1] == columns` is
    allowed (end of row).
- The invariant during 2D decode:
  `refLine[b1i - 1] <= codingLine[a0i] < refLine[b1i] < refLine[b1i + 1]`.

## Bit reading (`lookBits` / `eatBits`)
- `lookBits(n)` returns the next `n` bits MSB-first.
- Near EOF, if fewer than `n` bits are available, Poppler returns the
  partial value left-shifted to `n` bits (it returns `EOF` only if zero
  bits are available). This affects 2D code detection and padding checks.

## 2D mode codes
- `getTwoDimCode()` uses a 7-bit lookup table (`twoDimTab1`).
- When `EndOfBlock` is true, it reads `lookBits(7)` once; otherwise it
  progressively tries `lookBits(n)` for `n = 1..7`, shifting as needed.
- The only outputs are: pass, horiz, vert0, vertL/R {1,2,3}, or `EOF`.

## Run-length code decoding
- White and black run codes are the canonical ITU T.4 tables.
- Poppler scans for the shortest matching terminating or makeup code, using
  table-based lookup that depends on `EndOfBlock`.
- On invalid code: it logs an error, **eats one bit**, and returns run=1.
- On EOF mid-code: it returns run=1 (not an error).
- Makeup codes (>= 64) are accumulated; the final terminating code (< 64)
  completes the run.

## 2D row decode (main loop)
Setup:
- Build `refLine[]` from previous row, then set `codingLine[0] = 0`.
- `a0i = 0`, `b1i = 0`, `blackPixels = 0` (white start).

Modes:
- **Pass**: `addPixels(refLine[b1i + 1], blackPixels)`; if still in row,
  `b1i += 2`.
- **Horizontal**:
  - Decode two runs: current color, then opposite.
  - Call `addPixels` for each (second call is skipped if already past width).
  - After both runs, update `b1i` by skipping transitions while
    `refLine[b1i] <= codingLine[a0i]`.
  - **Do not toggle** `blackPixels` inside H-mode (two runs cancel).
- **Vertical**:
  - Use `refLine[b1i] + delta` as the target.
  - `delta < 0` uses `addPixelsNeg`, which can move `a0i` backward.
  - Toggle `blackPixels`, then update `b1i` (negative deltas move left by 1).

Error handling inside the loop:
- Invalid 2D code: fill the rest of the row with white and set `err`.
- Loop stops when `codingLine[a0i] >= columns` or `err` is set.

## 1D row decode
- Alternates white/black runs using the same run decoder as H-mode.
- The row ends when `codingLine[a0i] >= columns`.

## End-of-line, byte align, and EOFB
- If `EndOfLine` is true, Poppler scans for the 12-bit EOL marker
  `000000000001` after each row.
- If `EndOfLine` is false and `EncodedByteAlign` is false, it skips trailing
  zeros while `lookBits(12) == 0`.
- If `EncodedByteAlign` is true and no EOL was consumed, it aligns to the
  next byte boundary.
- `EndOfBlock` handling checks for RTC (six EOLs) after an EOL when needed.

## Output stage
- If `codingLine[0] > 0`, the row starts with white (`a0i = 0`).
- If `codingLine[0] == 0`, the row starts with black (`a0i = 1`).
- Output bytes are produced by walking transitions and emitting runs.
- `BlackIs1` inverts the output bytes at the end.

## Parity checklist for nanopdf
1) `lookBits()` must return partial values near EOF (not `EOF`).
2) Invalid run codes consume one bit and return run=1, not failure.
3) `EncodedByteAlign` applies **only** when no EOL is consumed.
4) `refLine` should be derived from the previous `codingLine` transitions,
   not re-synthesized from pixels when possible.
5) `b1i` update rules must match Poppler exactly in pass/horiz/vert modes.
