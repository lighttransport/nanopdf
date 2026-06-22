# nanopdf C++ → Pure C11 Migration Roadmap

> Status: **planning document** — no migration code has been written yet. This roadmap
> describes the phased plan to deprecate the C++17 core in favor of a pure-C11 implementation.

## Why

nanopdf's core (`src/`) is ~74K LOC of C++17 (heavy use of `std::string`, `std::vector`,
`std::map`, `std::unique_ptr`, polymorphic class hierarchies, and exceptions). The project
is already moving toward a dependency-light, pure-C posture:

- **`ncrypto/`** — a standalone **pure-C11** crypto + TLS 1.3 stack (13 modules, ~6.9K LOC,
  13/13 tests passing). It already replaces all OpenSSL usage for `pdfview` signing/verify.
- **`src/c/`** — a C API surface (`nanopdf_c.h` + friends) that today is a *thin wrapper*
  bridging into the C++ core (`*_bridge.cc`).

The goal is a single pure-C11 library (`npdf`) that supersedes the C++ core, with `ncrypto`
as its crypto provider and the existing C API as its public surface. C is chosen for maximal
embeddability (ABI stability, trivial FFI, no STL/exception runtime, easier WASM/MCU targets).

## Target end state

```
ncrypto/      pure-C11 crypto + TLS            (DONE)
npdf/         pure-C11 PDF core                (NEW — replaces src/ C++)
  npdf.h        public API (evolves from src/c/nanopdf_c.h)
  internal/     parser, object model, filters, fonts, render, writer
examples/     all consume npdf.h (C API) only
```

`src/` (C++) is removed at cutover; `src/c/*_bridge.cc` shims are deleted (no longer needed
once the implementation behind the C API is itself C).

## Guiding principles

- **Coexistence behind a flag.** Build option `NANOPDF_PURE_C` selects the C core; the C++
  core remains buildable until cutover so each phase can be validated against it.
- **C API is the contract.** Every phase keeps `src/c/` (the public C API) green. Examples
  and tests exercise only the C API, so the implementation can be swapped underneath.
- **Differential testing.** For each ported module, run the C and C++ implementations over
  the same corpus and diff outputs (parse trees, decoded streams, extracted text, rendered
  pixels) until byte/pixel parity. Reuse the existing corpus + visual-regression harnesses.
- **Memory model.** Replace RAII with explicit ownership: arena allocators for parse-scoped
  data, intrusive refcounts for shared/cached objects (mirrors the C++ object cache), and a
  consistent `npdf_status` return-code discipline replacing exceptions.

## Phases

### Phase 0 — Freeze & gate the public surface
- Make `src/c/` the **only** supported public interface. Add deprecation notices to C++
  headers (`src/*.hh`); document that direct C++ use is unsupported going forward.
- Migrate every example (`pdfdump`, `pdfsign`, `rasterize`, `pdfview`, `img2pdf`) to consume
  the C API instead of C++ types. This proves the C API is feature-complete enough to be the
  contract.
- Add a CI job that compiles a C11 conformance canary (`-std=c11 -Wall -Wextra -pedantic`).
- **Exit:** all examples build against `libnanopdf-c` only; no example includes a `src/*.hh`.

### Phase 1 — Foundation primitives (pure C11)
Port leaf utilities with no PDF semantics:
- byte/stream readers (`stream-reader.hh`), logging (`nanopdf-log.hh`),
- miniz / FlateDecode glue, and the simple filters: `ascii85`, `asciihex`, `runlength`,
  `lzw`, PNG predictors.
- Delete C++ crypto (`crypto.cc`, `crypto-pk.cc`) — route everything through `ncrypto` `nc_*`.
- **Exit:** filter unit tests pass against the C implementations; crypto unit tests run on
  `ncrypto` only.

### Phase 2 — Object model & parser
The highest-risk, highest-value phase.
- Re-implement the polymorphic `Value` as a tagged union (`npdf_value` with a `kind` enum).
- Port the lexer/parser, xref tables (classic + xref streams), object-stream resolution, and
  the `Pdf` document container (caches, trailer, catalog) with arena + refcount memory.
- Replace `std::map`/`std::vector` caches with explicit hash maps / dynamic arrays.
- **Exit:** corpus parse-rate and Arlington-validation results match the C++ core on the full
  corpus (CC-MAIN, UNSAFE-DOCS, SafeDocs) with zero crashes under ASan/UBSan.

### Phase 3 — Content streams, text & fonts
- Content-stream tokenizer, `TextState` machine, CMap / Type0 / Type3 handling.
- Font loading: `cff-parser`, `type1-parser`, stb_truetype (already C), font substitution.
- Text layout & table extraction.
- **Exit:** text-extraction integration suite matches C++ output on the test corpus.

### Phase 4 — Images & rendering
- Image XObjects, color-space transforms, decode arrays/masks.
- Decoders: DCT (stb_image, C), CCITT, JBIG2 (currently C++ ported from PDFium — assess a C
  port vs. keeping an isolated C++ TU behind the flag), JPX.
- Rendering: the **LightVG** backend is already C (`src/third_party/lightvg`); wire it to the
  C core. ThorVG / Blend2D are C++ libraries — keep them as optional C++ shims or drop them
  from the pure-C build.
- **Exit:** visual-regression (LightVG) parity within the existing ink-ratio thresholds.

### Phase 5 — Writer & higher-level features
- `pdf-writer` (creation + incremental updates), `PageBuilder`, forms (fill / FDF), the full
  annotation hierarchy, document structure / outlines, and signatures (on `ncrypto` CMS /
  RFC 3161 / PKCS#12).
- **Exit:** writer + signature round-trip tests pass; signatures verify against `pdfsig`.

### Phase 6 — Cutover
- Delete the C++ `src/` core and the `src/c/*_bridge.cc` shims.
- Rename the C core to the canonical library; `npdf.h` becomes the single public header.
- Collapse the `NANOPDF_PURE_C` flag (now the only path).
- **Exit:** the C++ core is gone; CI builds and tests the pure-C library only.

## Testing strategy during migration

- **nanotest is C++** (`tests/nanotest.hh`). Options: (a) keep a thin C++ test harness that
  links the C library through `extern "C"` (lowest friction, recommended during transition),
  or (b) adopt a C unit framework (e.g. the pattern already used by `ncrypto/tests/test_*.c`).
- Per-phase **differential gate**: same input → C core vs. C++ core → assert equal outputs.
- The aggregated suites (`nanopdf_unit_suite`, `nanopdf_integration_suite`,
  `nanopdf_validation_suite`) and the visual-regression / corpus harnesses are reused as the
  acceptance gates for each phase.

## Highest-risk modules (watch list)

| Module | Risk |
| --- | --- |
| Object model / parser (Phase 2) | Foundation for everything; subtle pointer/lifetime bugs |
| CMap / Type0 / font loading (Phase 3) | Complex state machines; Unicode correctness |
| JBIG2 / JPX (Phase 4) | Large C++-from-PDFium code; C port is substantial |
| Writer + signatures (Phase 5) | Byte-exact output + cryptographic correctness |

## Scope note

This is a multi-person-year effort, far larger than `ncrypto` (which covered crypto only).
It should be resourced and scheduled deliberately; the phases above are designed to ship
incrementally so the library stays shippable (C++ core intact) until the final cutover.
