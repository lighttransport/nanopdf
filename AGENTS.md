# Repository Guidelines

## Project Structure & Module Organization
Source for the core library lives in `src/` (`nanopdf.cc`, `nanopdf.hh`, decoder backends). Test and demo entry points also live under `src/test-*.cc`. Prebuilt artifacts belong in `build/` (ignore it in commits). Supporting assets such as sample PDFs are under `data/`, and runnable examples ship in `examples/` (for example `examples/rasterize`). Helper toolchains and sanitizer CMake modules live in `cmake/`, with bootstrap scripts in `scripts/`.

## Build, Test, and Development Commands
Configure once with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` (toggle options like `-DNANOPDF_USE_THORVG=ON`). Build the library and utilities via `cmake --build build`. To iterate with Clang, run `scripts/bootstrap-clang.sh`. Individual phase test apps are emitted as targets; build a specific one with `cmake --build build --target test_phase3`. Run a parser check against a sample file using `./build/test_phase3 data/HelloWorld_xr.pdf`.

## Coding Style & Naming Conventions
Code is C++11 and formatted with clang-format (`.clang-format` is Google style, 2-space indent, attached braces, no tabs). Types use UpperCamelCase, free functions favor snake_case, and constants stay uppercase. Prefer STL containers; only enable `NANOPDF_USE_NANOSTL` when porting to constrained environments. Keep headers self-contained and include guards via `#pragma once` like the existing files.

## Testing Guidelines
No external framework is used; each `test_phase*` binary exercises a feature band. When adding coverage, mirror the naming (`test-phase7-features.cc`) so CMake picks it up. Ensure new tests accept a PDF path and report clear errors to stderr, matching the current pattern. When touching parsers, run all phase executables against `data/*.pdf` and any new regression sample you add under `data/`.

## Commit & Pull Request Guidelines
Follow the imperative, descriptive style seen in history (e.g., `Add PDF to PNG rasterizer example`). Squash fixups locally before sharing. Pull requests should describe user-visible changes, list build flags exercised, and attach logs or screenshots for rendering updates. Reference related issues with `Fixes #123` syntax, and note any new data files or optional dependencies introduced.

## Security & Configuration Tips
Treat third-party PDFs as untrusted input. Prefer reproducing issues with the redacted samples in `data/`, and document any external fixtures that cannot be shared. When enabling optional backends (ThorVG, stb_truetype, nanostl), echo the flags used so reviewers can reproduce the configuration.
