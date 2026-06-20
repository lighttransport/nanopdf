cmake_minimum_required(VERSION 3.16)

set(BUILD_TARGET "nanopdf")
set(TEST_TARGET "test_nanopdf")
project(${BUILD_TARGET} LANGUAGES C CXX)

include(CTest)

# options
option(NANOPDF_USE_CCACHE "Use ccache for faster recompile." ON)
option(NANOPDF_USE_NANOSTL "Use nanostl." OFF)
option(NANOPDF_USE_STB_TRUETYPE "Use stb_truetype to load fonts." ON)
option(NANOPDF_BUILD_TESTS "Build test executables" ${BUILD_TESTING})
option(NANOPDF_BUILD_WASM "Build for WebAssembly with Emscripten" OFF)
option(NANOPDF_BUILD_MCP "Build the MCP server target for native builds" ON)
option(NANOPDF_USE_LIGHTVG "Use lightui VG software rasterizer (default backend)" ON)
option(NANOPDF_USE_THORVG "Use ThorVG for rendering backend" OFF)
option(NANOPDF_USE_BLEND2D "Use Blend2D for rendering backend" OFF)
option(NANOPDF_USE_LIBTIFF "Use libtiff for CCITT/Fax decoding" OFF)
option(NANOPDF_USE_FPNGE "Use fpnge for fast PNG encoding when available (AVX2/SSE4.1/SSE2; SSE2->WASM SIMD)" ON)
option(NANOPDF_WASM_SIMD "Enable WASM SIMD128 (lets the SSE2 fpnge path map onto wasm-simd)" OFF)
option(NANOPDF_USE_LIBDEFLATE "Use libdeflate as the fast path for FlateDecode (zlib) decompression" ON)
option(NANOPDF_C_USE_CPP_IMAGE_BRIDGE "Enable the C API stream decoder bridge to the C++ JPX/JBIG2 decoders" OFF)
option(NANOPDF_EXTERNAL_FONTS "Load fonts from external files instead of embedding" OFF)
option(NANOPDF_NO_EXCEPTIONS "Build without C++ exceptions (-fno-exceptions / /EHs-c-)" ON)
option(NANOPDF_NO_RTTI "Build without C++ RTTI (-fno-rtti / /GR-)" ON)

# Log level: 0=None, 1=Error, 2=Warn, 3=Info, 4=Debug, 5=Trace
set(NANOPDF_LOG_LEVEL "3" CACHE STRING "Log level (0=None, 1=Error, 2=Warn, 3=Info, 4=Debug, 5=Trace)")
set_property(CACHE NANOPDF_LOG_LEVEL PROPERTY STRINGS "0" "1" "2" "3" "4" "5")

# cmake modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/sanitizers)
find_package(Sanitizers) # Address sanitizer (-DSANITIZE_ADDRESS=ON)

# Overall compile options
#set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_GLIBCXX_DEBUG -D_LIBCPP_DEBUG")
#set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_GLIBCXX_DEBUG -D_LIBCPP_DEBUG")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build nanopdf (and the examples/tests that pick up these flags) without C++
# exceptions and RTTI. The library replaces every std::stoi / std::stod call
# with the non-throwing parsers in src/string-parse.hh and uses
# static_cast<Type0Font*> guards instead of dynamic_cast<Type0Font*> on
# known-derived pointers, so neither feature is required by library code.
#
# Third-party prebuilts (ThorVG, Blend2D) are consumed as already-compiled
# static libraries from dist/, so these flags do not affect them. If someone
# builds ThorVG or Blend2D from source alongside nanopdf, both projects
# support -DTVG_NO_EXCEPTIONS / -DBL_BUILD_NO_CXX_RUNTIME toggles which map
# to the same intent; wire those up in the respective subproject.
#
# Leave the flags opt-out (defaults ON) because an unchecked std::stoi in
# PDF content-stream parsing is a crash on malformed input.
set(NANOPDF_CXX_OPTIONS "")
set(NANOPDF_CXX_DEFINES "")
if(NANOPDF_NO_EXCEPTIONS)
  if(MSVC)
    list(APPEND NANOPDF_CXX_OPTIONS "/EHs-c-")
  else()
    list(APPEND NANOPDF_CXX_OPTIONS "-fno-exceptions")
  endif()
  # libc++ / libstdc++ use these to avoid throwing from the STL where
  # possible. Setting them unconditionally is safe.
  list(APPEND NANOPDF_CXX_DEFINES "_HAS_EXCEPTIONS=0")
endif()
if(NANOPDF_NO_RTTI)
  if(MSVC)
    list(APPEND NANOPDF_CXX_OPTIONS "/GR-")
  else()
    list(APPEND NANOPDF_CXX_OPTIONS "-fno-rtti")
  endif()
endif()

# Windows/MinGW: enable M_PI and other POSIX math constants
if(WIN32 OR MINGW)
  add_compile_definitions(_USE_MATH_DEFINES)
endif()

# Emscripten-specific settings
if(NANOPDF_BUILD_WASM)
  if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "NANOPDF_BUILD_WASM is ON but Emscripten toolchain is not detected. Use emcmake to configure.")
  endif()

  # Disable ccache for Emscripten builds
  set(NANOPDF_USE_CCACHE OFF)

  # Disable sanitizers for WASM builds
  set(SANITIZE_ADDRESS OFF)

  # Set Emscripten-specific compilation flags (no -s flags during compilation)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")

  # Set Emscripten-specific linker flags (all -s flags go here)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s WASM=1 -s ALLOW_MEMORY_GROWTH=1")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s MODULARIZE=1 -s EXPORT_ES6=1")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','stringToNewUTF8','UTF8ToString'] -s EXPORTED_FUNCTIONS=['_malloc','_free']")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s USE_ZLIB=1")

  # WASM SIMD128: enables the SSE2 fpnge path (Emscripten lowers SSE2 intrinsics
  # to wasm-simd). Applied globally so the whole module can use SIMD.
  if(NANOPDF_WASM_SIMD)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msimd128")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msimd128")
    message(STATUS "WASM SIMD128 enabled")
  endif()
endif()

# [ccache]
if(NANOPDF_USE_CCACHE AND NOT NANOPDF_BUILD_WASM)
  if(MSVC)
    # No ccache support
  else()
    find_program(CCACHE_EXE ccache)
    if(CCACHE_EXE)

      message(STATUS "Use ccache : " ${CCACHE_EXE})
      # CMAKE_C_COMPILER_LAUNCHER = available from cmake 3.4+)
      if(CMAKE_C_COMPILER_LAUNCHER)
        set(CMAKE_C_COMPILER_LAUNCHER "${CMAKE_C_COMPILER_LAUNCHER}"
                                      "${CCACHE_EXE}")
      else()
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_EXE}")
      endif()

      if(CMAKE_CXX_COMPILER_LAUNCHER)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_CXX_COMPILER_LAUNCHER}"
                                        "${CCACHE_EXE}")
      else()
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_EXE}")
      endif()

    endif(CCACHE_EXE)
  endif()
endif()


set(NANOPDF_EXT_INCLUDE_DIRS_PRIVATE "")
set(NANOPDF_EXT_LIBRARIES_PRIVATE "")
set(NANOPDF_EXT_LIBRARIES_PUBLIC "")

set(NANOPDF_SOURCES
  src/nanopdf.cc
  src/jpx-decoder.cc
  src/type1-parser.cc
  src/cff-parser.cc
  src/canvas-exporter.cc
  src/crypto.cc
  src/crypto-pk.cc
  src/asn1-der.cc
  src/cms.cc
  src/rfc3161.cc
  src/pdf-security.cc
  src/pdf-attachments.cc
  src/color-transform.cc
  src/tiff-decoder.cc
  src/text-layout.cc
  src/table-extraction.cc
  src/form-manipulation.cc
  src/document-structure.cc
  src/pdf-writer.cc
  src/third_party/stb_image_impl.cc
  src/third_party/stb_image_write_impl.cc
  src/third_party/stb_truetype_impl.cc
  src/third_party/tiny_dng_impl.cc
  # TrueType/CFF font parser + kerning (kern + GPOS PairPos); ported from lightui/src/fonts.
  src/third_party/ttf_parse.c
  # Analytic-area glyph outline rasterizer; pairs with ttf_parse. Ported from lightui/src/fonts.
  src/third_party/rasterize.c
  # JBIG2 decoder infrastructure (Phase 1.4)
  src/jbig2/JBig2_BitStream.cc
  src/jbig2/JBig2_Image.cc
  src/jbig2/JBig2_Segment.cc
  src/jbig2/JBig2_SymbolDict.cc
  src/jbig2/JBig2_PatternDict.cc
  src/jbig2/JBig2_HuffmanTable.cc
  src/jbig2/JBig2_ArithDecoder.cc
  src/jbig2/JBig2_ArithIntDecoder.cc
  src/jbig2/JBig2_GrdProc.cc
  src/jbig2/JBig2_GrrdProc.cc
  # JBIG2 decoder main components (completing Phase 1.4)
  src/jbig2/JBig2_Context.cc
  src/jbig2/JBig2_SddProc.cc
  src/jbig2/JBig2_TrdProc.cc
  # Font provider (runtime font registration)
  src/font-provider.cc
)

# Always-on render-backend abstraction (factory + parse helpers).
list(APPEND NANOPDF_SOURCES src/render-backend.cc)

# Shared font cache (cross-backend font data sharing).
list(APPEND NANOPDF_SOURCES src/shared-font-cache.cc)

# Render cache (LRU-evicted byte-size-bounded cache for glyphs, tiles, images).
list(APPEND NANOPDF_SOURCES src/render-cache.cc)

if(NANOPDF_USE_LIGHTVG)
  # scene.c/dirty.c/layer.c are the UI-side compositor (not needed for raster
  # output) so we skip them. nanopdf only uses the lvg_canvas software path.
  # This vendored copy tracks lightui's current lightvg (lvg_* API); it carries
  # no logging module, so there is no lui_log.c to compile.
  list(APPEND NANOPDF_SOURCES
    src/lightvg-backend.cc
    src/lvg-compat.cc
    src/third_party/lightvg/src/canvas.c
    src/third_party/lightvg/src/surface.c
    src/third_party/lightvg/src/backend_registry.c
  )
endif()

if(NANOPDF_USE_THORVG)
  list(APPEND NANOPDF_SOURCES src/thorvg-backend.cc)
endif()

if(NANOPDF_USE_BLEND2D)
  list(APPEND NANOPDF_SOURCES src/blend2d-backend.cc)
endif()

# Add WASM API for Emscripten builds
if(NANOPDF_BUILD_WASM)
  list(APPEND NANOPDF_SOURCES src/wasm-api.cc)
  # Remove document-structure.cc for WASM builds because nanopdf.cc already
  # contains the same function definitions (parse_document_outline, etc.)
  # and duplicate symbols cause linker errors in executable targets.
  list(REMOVE_ITEM NANOPDF_SOURCES src/document-structure.cc)
endif()

# Generate embedded fonts header for WASM and rasterize builds
option(NANOPDF_EMBED_FONTS "Embed PDF Standard 14 alternative fonts as binary data" OFF)

# Auto-enable font embedding for WASM builds (unless external fonts mode)
if(NANOPDF_BUILD_WASM AND NOT NANOPDF_EXTERNAL_FONTS)
  set(NANOPDF_EMBED_FONTS ON CACHE BOOL "Embed fonts for WASM build" FORCE)
endif()

if(NANOPDF_EMBED_FONTS)
  set(FONTS_DIR ${CMAKE_SOURCE_DIR}/fonts)
  set(EMBEDDED_FONTS_HEADER ${CMAKE_BINARY_DIR}/embedded-fonts.hh)
  set(EMBEDDED_FONTS_ASM ${CMAKE_BINARY_DIR}/embedded-fonts.S)
  set(EMBEDDED_FONTS_CC ${CMAKE_BINARY_DIR}/embedded-fonts.cc)

  # Find all font files to establish dependencies
  file(GLOB_RECURSE FONT_FILES
    "${FONTS_DIR}/*.ttf"
    "${FONTS_DIR}/*.otf"
  )

  if(FONT_FILES)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    if(NANOPDF_BUILD_WASM)
      # Emscripten's wasm assembler cannot consume ELF-style .incbin sections,
      # so use the portable C++ array generator for WASM builds.
      add_custom_command(
        OUTPUT ${EMBEDDED_FONTS_HEADER}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.py
          ${FONTS_DIR} ${EMBEDDED_FONTS_HEADER}
          --exclude-dirs=noto-sans-jp,noto-serif-jp
        DEPENDS ${FONT_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.py
        COMMENT "Generating embedded fonts (C++ header)"
        VERBATIM
      )

      add_custom_target(generate_embedded_fonts
        DEPENDS ${EMBEDDED_FONTS_HEADER}
      )

      set_source_files_properties(${EMBEDDED_FONTS_HEADER} PROPERTIES GENERATED TRUE)
      message(STATUS "Font embedding enabled (${FONTS_DIR}, C++ header)")
    else()
      # Generate compressed .bin files + .hh/.S/.cc (ELF .incbin approach) —
      # dramatically faster to compile than a single multi-megabyte hex array.
      add_custom_command(
        OUTPUT ${EMBEDDED_FONTS_HEADER} ${EMBEDDED_FONTS_ASM} ${EMBEDDED_FONTS_CC}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts_binary.py
          ${FONTS_DIR} ${CMAKE_BINARY_DIR} embedded-fonts
          --exclude-dirs=noto-sans-jp,noto-serif-jp
        DEPENDS ${FONT_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts_binary.py
        COMMENT "Generating embedded fonts (incbin)"
        VERBATIM
      )

      add_custom_target(generate_embedded_fonts
        DEPENDS ${EMBEDDED_FONTS_HEADER} ${EMBEDDED_FONTS_ASM} ${EMBEDDED_FONTS_CC}
      )

      set_source_files_properties(${EMBEDDED_FONTS_HEADER} ${EMBEDDED_FONTS_ASM}
                                  ${EMBEDDED_FONTS_CC} PROPERTIES GENERATED TRUE)
      enable_language(ASM)
      list(APPEND NANOPDF_SOURCES ${EMBEDDED_FONTS_ASM} ${EMBEDDED_FONTS_CC})

      message(STATUS "Font embedding enabled (${FONTS_DIR}, incbin)")
    endif()
  else()
    message(WARNING "Font embedding enabled but no fonts found in ${FONTS_DIR}")
    set(NANOPDF_EMBED_FONTS OFF)
  endif()
endif()

# Separate CJK font embedding (Noto Sans/Serif JP, ~61 MB compressed)
option(NANOPDF_EMBED_CJK_FONTS "Embed CJK fonts (Noto Sans/Serif JP) as binary data" OFF)
option(NANOPDF_WASM_EMBED_CJK_FONTS "Allow CJK font embedding in WASM builds despite very high compile cost" OFF)

if(NANOPDF_BUILD_WASM AND NANOPDF_EMBED_CJK_FONTS AND NOT NANOPDF_WASM_EMBED_CJK_FONTS)
  message(WARNING "Disabling NANOPDF_EMBED_CJK_FONTS for WASM. Use external fonts, or set NANOPDF_WASM_EMBED_CJK_FONTS=ON to opt into the slow large-header build.")
  set(NANOPDF_EMBED_CJK_FONTS OFF CACHE BOOL "Embed CJK fonts (Noto Sans/Serif JP) as binary data" FORCE)
endif()

if(NANOPDF_EMBED_CJK_FONTS)
  # CJK embedding requires base font embedding
  if(NOT NANOPDF_EMBED_FONTS)
    message(FATAL_ERROR "NANOPDF_EMBED_CJK_FONTS requires NANOPDF_EMBED_FONTS=ON")
  endif()

  set(EMBEDDED_CJK_FONTS_HEADER ${CMAKE_BINARY_DIR}/embedded-cjk-fonts.hh)
  set(EMBEDDED_CJK_FONTS_ASM ${CMAKE_BINARY_DIR}/embedded-cjk-fonts.S)
  set(EMBEDDED_CJK_FONTS_CC ${CMAKE_BINARY_DIR}/embedded-cjk-fonts.cc)
  file(GLOB_RECURSE CJK_FONT_FILES
    "${FONTS_DIR}/noto-sans-jp/*.otf"
    "${FONTS_DIR}/noto-serif-jp/*.otf"
  )

  if(CJK_FONT_FILES)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(NANOPDF_BUILD_WASM)
      add_custom_command(
        OUTPUT ${EMBEDDED_CJK_FONTS_HEADER}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.py
          ${FONTS_DIR} ${EMBEDDED_CJK_FONTS_HEADER}
          --include-only-dirs=noto-sans-jp,noto-serif-jp
          --header-guard=NANOPDF_EMBEDDED_CJK_FONTS_HH
          --namespace=embedded_cjk_fonts
          --skip-pdf-mapping
        DEPENDS ${CJK_FONT_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts.py
        COMMENT "Generating embedded CJK fonts (C++ header)"
        VERBATIM
      )
      add_custom_target(generate_embedded_cjk_fonts
        DEPENDS ${EMBEDDED_CJK_FONTS_HEADER})
      set_source_files_properties(${EMBEDDED_CJK_FONTS_HEADER} PROPERTIES GENERATED TRUE)
      message(STATUS "CJK font embedding enabled (C++ header)")
    else()
      add_custom_command(
        OUTPUT ${EMBEDDED_CJK_FONTS_HEADER} ${EMBEDDED_CJK_FONTS_ASM} ${EMBEDDED_CJK_FONTS_CC}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts_binary.py
          ${FONTS_DIR} ${CMAKE_BINARY_DIR} embedded-cjk-fonts
          --include-only-dirs=noto-sans-jp,noto-serif-jp
          --header-guard=NANOPDF_EMBEDDED_CJK_FONTS_HH
          --namespace=embedded_cjk_fonts
          --symbol-prefix=nanopdf_cjkfont_
          --skip-pdf-mapping
        DEPENDS ${CJK_FONT_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_fonts_binary.py
        COMMENT "Generating embedded CJK fonts (incbin)"
        VERBATIM
      )
      add_custom_target(generate_embedded_cjk_fonts
        DEPENDS ${EMBEDDED_CJK_FONTS_HEADER} ${EMBEDDED_CJK_FONTS_ASM} ${EMBEDDED_CJK_FONTS_CC})
      set_source_files_properties(${EMBEDDED_CJK_FONTS_HEADER} ${EMBEDDED_CJK_FONTS_ASM}
                                  ${EMBEDDED_CJK_FONTS_CC} PROPERTIES GENERATED TRUE)
      enable_language(ASM)
      list(APPEND NANOPDF_SOURCES ${EMBEDDED_CJK_FONTS_ASM} ${EMBEDDED_CJK_FONTS_CC})
      message(STATUS "CJK font embedding enabled (incbin)")
    endif()
  else()
    message(WARNING "CJK font embedding enabled but no CJK fonts found in ${FONTS_DIR}/noto-sans-jp or ${FONTS_DIR}/noto-serif-jp")
    set(NANOPDF_EMBED_CJK_FONTS OFF)
  endif()
endif()

if(NANOPDF_BUILD_WASM)
  add_executable(${BUILD_TARGET} ${NANOPDF_SOURCES})
else()
  add_library(${BUILD_TARGET} ${NANOPDF_SOURCES})
endif()
target_include_directories(${BUILD_TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party)
target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_LOG_LEVEL=${NANOPDF_LOG_LEVEL}")

# Apply the no-exception / no-rtti toggles to C++ sources only. The C
# third-party code (miniz, stb_image, ttf_parse) doesn't care about -fno-rtti
# but -fno-exceptions is harmless for C. Using a generator expression so the
# flags are scoped to the nanopdf target and don't leak into dependents via
# PUBLIC propagation.
if(NANOPDF_CXX_OPTIONS)
  target_compile_options(${BUILD_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${NANOPDF_CXX_OPTIONS}>)
endif()
if(NANOPDF_CXX_DEFINES)
  target_compile_definitions(${BUILD_TARGET} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${NANOPDF_CXX_DEFINES}>)
endif()

# Add embedded fonts support
if(NANOPDF_EMBED_FONTS)
  # Add dependency on the generated header
  add_dependencies(${BUILD_TARGET} generate_embedded_fonts)

  # Add the build directory to include path so the generated header can be found
  # Also add src/ so generated headers can find miniz.h
  target_include_directories(${BUILD_TARGET} PRIVATE ${CMAKE_BINARY_DIR} ${PROJECT_SOURCE_DIR}/src)

  # Define macro to enable embedded fonts in the code
  target_compile_definitions(${BUILD_TARGET} PUBLIC "NANOPDF_EMBED_FONTS=1")
endif()

# Add embedded CJK fonts support
if(NANOPDF_EMBED_CJK_FONTS AND CJK_FONT_FILES)
  add_dependencies(${BUILD_TARGET} generate_embedded_cjk_fonts)
  target_compile_definitions(${BUILD_TARGET} PUBLIC "NANOPDF_EMBED_CJK_FONTS=1")
endif()

# Link with zlib (or use embedded miniz)
if(NANOPDF_BUILD_WASM)
  # For Emscripten, USE_ZLIB must be specified for both compilation and linking
  target_compile_options(${BUILD_TARGET} PRIVATE "SHELL:-s USE_ZLIB=1")
  target_link_options(${BUILD_TARGET} PRIVATE "SHELL:-s USE_ZLIB=1")
  # stb_image_write_impl.cc routes PNG compression through the bundled miniz
  # prefixed mz_* APIs even in WASM builds.
  target_sources(${BUILD_TARGET} PRIVATE src/third_party/miniz.c)
else()
  # Always use embedded miniz as zlib replacement (no external dependencies)
  target_sources(${BUILD_TARGET} PRIVATE src/third_party/miniz.c)
  target_compile_definitions(${BUILD_TARGET} PUBLIC "NANOPDF_USE_MINIZ")
  message(STATUS "Using embedded miniz (no system zlib dependency)")
endif()

if (NANOPDF_USE_NANOSTL)
  # nanostl
  target_include_directories(${BUILD_TARGET} PRIVATE src/nanostl/include)
  target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_NANOSTL")
endif ()

if (NANOPDF_USE_STB_TRUETYPE)
  target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_STB_TRUETYPE")
endif ()

if (NANOPDF_USE_LIGHTVG)
  target_include_directories(${BUILD_TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/lightvg/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/lightvg/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/lightvg/src/internal
  )
  target_compile_definitions(${BUILD_TARGET} PUBLIC "NANOPDF_USE_LIGHTVG")
endif ()

# --- fpnge: fast PNG encoder with runtime ISA dispatch -----------------------
# fpnge.cc is SIMD-abstracted; it is compiled once per ISA (AVX2 / SSE4.1 /
# pure-SSE2) and a baseline dispatcher picks the best available path at runtime.
# NANOPDF_FPNGE_HAVE_* tell the dispatcher which TUs exist.
#  * Native GCC/Clang on x86: all three paths.
#  * WASM with SIMD: only the SSE2 path — Emscripten lowers its SSE2 intrinsics
#    to wasm-simd128; SSE4.1-specific ops and 256-bit AVX2 have no wasm mapping.
# Elsewhere (or WASM without SIMD) fpnge is off and callers fall back to
# stb_image_write+miniz.
set(FPNGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/fpnge)
set(FPNGE_AVX2_SRC     ${FPNGE_DIR}/fpnge_avx2.cc)
set(FPNGE_SSE41_SRC    ${FPNGE_DIR}/fpnge_sse41.cc)
set(FPNGE_SSE2_SRC     ${FPNGE_DIR}/fpnge_sse2.cc)
set(FPNGE_DISPATCH_SRC ${FPNGE_DIR}/fpnge_dispatch.cc)

if(NANOPDF_USE_FPNGE AND NANOPDF_BUILD_WASM AND NANOPDF_WASM_SIMD)
  # WASM SIMD: SSE2 path only.
  target_sources(${BUILD_TARGET} PRIVATE ${FPNGE_SSE2_SRC} ${FPNGE_DISPATCH_SRC})
  set_source_files_properties(${FPNGE_SSE2_SRC} PROPERTIES
    COMPILE_OPTIONS "-msimd128;-msse2")
  target_include_directories(${BUILD_TARGET} PRIVATE ${FPNGE_DIR})
  target_compile_definitions(${BUILD_TARGET} PRIVATE
    "NANOPDF_USE_FPNGE" "NANOPDF_FPNGE_HAVE_SSE2")
  message(STATUS "fpnge PNG encoder enabled for WASM (SSE2 -> wasm-simd128)")
elseif(NANOPDF_USE_FPNGE AND NOT NANOPDF_BUILD_WASM AND
       (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") AND
       (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64|i[3-6]86|x86"))
  target_sources(${BUILD_TARGET} PRIVATE
    ${FPNGE_AVX2_SRC} ${FPNGE_SSE41_SRC} ${FPNGE_SSE2_SRC} ${FPNGE_DISPATCH_SRC})
  # Per-ISA flags: each encoder TU only ever runs after a matching CPU probe.
  set_source_files_properties(${FPNGE_AVX2_SRC} PROPERTIES
    COMPILE_OPTIONS "-mavx2;-mbmi2;-mpclmul")
  set_source_files_properties(${FPNGE_SSE41_SRC} PROPERTIES
    COMPILE_OPTIONS "-msse4.1;-mssse3;-mpclmul")
  # Pure SSE2: explicitly disable everything above SSE2 so this object stays
  # runnable on the oldest x86-64 hardware even under a global -march=native.
  set_source_files_properties(${FPNGE_SSE2_SRC} PROPERTIES
    COMPILE_OPTIONS "-msse2;-mno-ssse3;-mno-sse4.1;-mno-avx;-mno-avx2;-mno-pclmul")
  target_include_directories(${BUILD_TARGET} PRIVATE ${FPNGE_DIR})
  target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_FPNGE"
    "NANOPDF_FPNGE_HAVE_AVX2" "NANOPDF_FPNGE_HAVE_SSE41" "NANOPDF_FPNGE_HAVE_SSE2")
  message(STATUS "fpnge PNG encoder enabled (AVX2 + SSE4.1 + pure-SSE2 runtime dispatch)")
endif ()

# --- libdeflate: fast path for FlateDecode (zlib) decompression --------------
# Only the decompression translation units are built; libdeflate does its own
# runtime ISA dispatch internally. Portable C (incl. WASM), with a generic
# scalar fallback, so no per-file ISA flags are needed. decode_flate() uses it
# as a fast path and falls back to the existing miniz/zlib inflate on any
# error (libdeflate is stricter about malformed/streaming zlib than zlib).
if(NANOPDF_USE_LIBDEFLATE)
  set(LIBDEFLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/libdeflate)
  target_sources(${BUILD_TARGET} PRIVATE
    ${LIBDEFLATE_DIR}/lib/zlib_decompress.c
    ${LIBDEFLATE_DIR}/lib/deflate_decompress.c
    ${LIBDEFLATE_DIR}/lib/adler32.c
    ${LIBDEFLATE_DIR}/lib/utils.c
    ${LIBDEFLATE_DIR}/lib/x86/cpu_features.c
    ${LIBDEFLATE_DIR}/lib/arm/cpu_features.c)
  target_include_directories(${BUILD_TARGET} PRIVATE ${LIBDEFLATE_DIR})
  target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_LIBDEFLATE")
  message(STATUS "libdeflate FlateDecode fast path enabled")
endif ()

if (NANOPDF_USE_THORVG)
  # For WASM builds, check third_party/thorvg/build_wasm_static/ first
  if(NANOPDF_BUILD_WASM)
    set(_THORVG_WASM_DIR ${CMAKE_SOURCE_DIR}/third_party/thorvg)
    # Check for libthorvg-1.a (new meson name) or libthorvg.a (old name)
    if(EXISTS ${_THORVG_WASM_DIR}/build_wasm_static/src/libthorvg-1.a)
      set(_THORVG_WASM_LIB ${_THORVG_WASM_DIR}/build_wasm_static/src/libthorvg-1.a)
    elseif(EXISTS ${_THORVG_WASM_DIR}/build_wasm_static/src/libthorvg.a)
      set(_THORVG_WASM_LIB ${_THORVG_WASM_DIR}/build_wasm_static/src/libthorvg.a)
    endif()
    if(EXISTS ${_THORVG_WASM_DIR}/inc/thorvg.h AND _THORVG_WASM_LIB)
      set(THORVG_FOUND TRUE)
      set(THORVG_INCLUDE_DIRS ${_THORVG_WASM_DIR}/inc)
      set(THORVG_LIBRARIES ${_THORVG_WASM_LIB})
      message(STATUS "Found ThorVG WASM static library: ${THORVG_LIBRARIES}")
    else()
      message(WARNING "ThorVG WASM static library not found. Build it first with: cd third_party/thorvg && emcmake meson setup build_wasm_static --default-library=static && cd build_wasm_static && ninja")
    endif()
  endif()

  # Check third_party/thorvg_install/ directory (skip for WASM builds where we already found ThorVG)
  if (NOT THORVG_FOUND)
    set(_THORVG_THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party/thorvg_install)
    if(EXISTS ${_THORVG_THIRD_PARTY_DIR}/include/thorvg.h)
      set(_THORVG_DIST_INCLUDE ${_THORVG_THIRD_PARTY_DIR}/include)
    elseif(EXISTS ${_THORVG_THIRD_PARTY_DIR}/include/thorvg-1/thorvg.h)
      set(_THORVG_DIST_INCLUDE ${_THORVG_THIRD_PARTY_DIR}/include/thorvg-1)
    endif()
    if(_THORVG_DIST_INCLUDE)
      set(THORVG_INCLUDE_DIRS ${_THORVG_DIST_INCLUDE})
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        set(_THORVG_LIB_SEARCH_DIRS ${_THORVG_THIRD_PARTY_DIR}/lib ${_THORVG_THIRD_PARTY_DIR}/lib/x86_64-linux-gnu)
      elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(_THORVG_LIB_SEARCH_DIRS ${_THORVG_THIRD_PARTY_DIR}/lib ${_THORVG_THIRD_PARTY_DIR}/lib/aarch64-linux-gnu)
      else()
        set(_THORVG_LIB_SEARCH_DIRS ${_THORVG_THIRD_PARTY_DIR}/lib ${_THORVG_THIRD_PARTY_DIR}/lib/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu)
      endif()
      find_library(THORVG_LIBRARY NAMES thorvg-1 thorvg
        PATHS ${_THORVG_LIB_SEARCH_DIRS}
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
      if(THORVG_LIBRARY)
        set(THORVG_FOUND TRUE)
        set(THORVG_LIBRARIES ${THORVG_LIBRARY})
        get_filename_component(THORVG_LIBRARY_DIRS ${THORVG_LIBRARY} DIRECTORY)
        message(STATUS "Found ThorVG in third_party/: ${THORVG_LIBRARY}")
      endif()
    endif()
    unset(_THORVG_DIST_INCLUDE)
  endif()

  # Check dist/ directory
  if (NOT THORVG_FOUND)
    set(THORVG_DIST_DIR ${CMAKE_SOURCE_DIR}/dist)
    # Check dist/include/ for thorvg.h (with or without thorvg-1 subdirectory)
    if(EXISTS ${THORVG_DIST_DIR}/include/thorvg.h)
      set(_THORVG_DIST_INCLUDE ${THORVG_DIST_DIR}/include)
    elseif(EXISTS ${THORVG_DIST_DIR}/include/thorvg-1/thorvg.h)
      set(_THORVG_DIST_INCLUDE ${THORVG_DIST_DIR}/include/thorvg-1)
    endif()
    if(_THORVG_DIST_INCLUDE)
      set(THORVG_INCLUDE_DIRS ${_THORVG_DIST_INCLUDE})
      # Search dist/lib/ directly, then architecture-specific subdirectories
      if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        set(_THORVG_LIB_SEARCH_DIRS ${THORVG_DIST_DIR}/lib ${THORVG_DIST_DIR}/lib/x86_64-linux-gnu)
      elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(_THORVG_LIB_SEARCH_DIRS ${THORVG_DIST_DIR}/lib ${THORVG_DIST_DIR}/lib/aarch64-linux-gnu)
      else()
        set(_THORVG_LIB_SEARCH_DIRS ${THORVG_DIST_DIR}/lib ${THORVG_DIST_DIR}/lib/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu)
      endif()
      find_library(THORVG_LIBRARY NAMES thorvg-1 thorvg
        PATHS ${_THORVG_LIB_SEARCH_DIRS}
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
      if(THORVG_LIBRARY)
        set(THORVG_FOUND TRUE)
        set(THORVG_LIBRARIES ${THORVG_LIBRARY})
        get_filename_component(THORVG_LIBRARY_DIRS ${THORVG_LIBRARY} DIRECTORY)
        message(STATUS "Found ThorVG in dist/: ${THORVG_LIBRARY}")
      endif()
    endif()
  endif()

  # Fall back to pkg-config if not found in dist/
  if (NOT THORVG_FOUND)
    find_package(PkgConfig)
    if (PkgConfig_FOUND)
      pkg_check_modules(THORVG IMPORTED_TARGET thorvg)
    endif()
  endif()

  if (NOT THORVG_FOUND)
    # Fallback to manual inclusion if ThorVG is installed locally
    # Assumes ThorVG headers are in /usr/local/include/thorvg or similar
    find_path(THORVG_INCLUDE_DIR thorvg.h
      PATHS /usr/local/include/thorvg /usr/include/thorvg
      PATH_SUFFIXES thorvg thorvg-1)

    find_library(THORVG_LIBRARY NAMES thorvg thorvg-1
      PATHS /usr/local/lib /usr/lib
      PATH_SUFFIXES thorvg)

    if (THORVG_INCLUDE_DIR AND THORVG_LIBRARY)
      set(THORVG_FOUND TRUE)
      set(THORVG_INCLUDE_DIRS ${THORVG_INCLUDE_DIR})
      set(THORVG_LIBRARIES ${THORVG_LIBRARY})
      message(STATUS "Found ThorVG: ${THORVG_LIBRARY}")
    else()
      message(WARNING "ThorVG not found. Please install ThorVG or disable NANOPDF_USE_THORVG")
    endif()
  endif()

  if (THORVG_FOUND)
    if (TARGET PkgConfig::THORVG)
      # Use imported target from pkg-config (has proper link directories)
      target_link_libraries(${BUILD_TARGET} PUBLIC PkgConfig::THORVG)
    else()
      # Use manual paths
      target_include_directories(${BUILD_TARGET} PUBLIC ${THORVG_INCLUDE_DIRS})
      target_link_libraries(${BUILD_TARGET} PUBLIC ${THORVG_LIBRARIES})
      # Add link directory for runtime library lookup
      if(THORVG_LIBRARY_DIRS)
        target_link_directories(${BUILD_TARGET} PUBLIC ${THORVG_LIBRARY_DIRS})
      endif()
    endif()
    target_compile_definitions(${BUILD_TARGET} PUBLIC "NANOPDF_USE_THORVG")
    # ThorVG may require OpenMP
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
      target_link_libraries(${BUILD_TARGET} PUBLIC OpenMP::OpenMP_CXX)
    endif()
  endif()
endif ()

# libtiff support for CCITT/Fax decoding
if (NANOPDF_USE_LIBTIFF)
  # First try to find libtiff in ref/tiff-4.7.1
  set(LIBTIFF_REF_DIR ${CMAKE_SOURCE_DIR}/ref/tiff-4.7.1)
  if(EXISTS ${LIBTIFF_REF_DIR}/libtiff/tiff.h)
    set(LIBTIFF_FOUND TRUE)
    set(LIBTIFF_INCLUDE_DIRS ${LIBTIFF_REF_DIR}/libtiff)
    # Try to find built library
    find_library(LIBTIFF_LIBRARY NAMES tiff
      PATHS ${LIBTIFF_REF_DIR}/build/libtiff ${LIBTIFF_REF_DIR}/libtiff
      NO_DEFAULT_PATH)
    if(LIBTIFF_LIBRARY)
      set(LIBTIFF_LIBRARIES ${LIBTIFF_LIBRARY})
      message(STATUS "Found libtiff in ref/: ${LIBTIFF_LIBRARY}")
    else()
      set(LIBTIFF_FOUND FALSE)
      message(STATUS "libtiff headers found in ref/ but library not built")
    endif()
  endif()

  # Fall back to system libtiff
  if (NOT LIBTIFF_FOUND)
    find_package(PkgConfig)
    if (PkgConfig_FOUND)
      pkg_check_modules(LIBTIFF IMPORTED_TARGET libtiff-4)
      if (NOT LIBTIFF_FOUND)
        pkg_check_modules(LIBTIFF IMPORTED_TARGET libtiff)
      endif()
    endif()
  endif()

  # Manual search as final fallback
  if (NOT LIBTIFF_FOUND)
    find_path(LIBTIFF_INCLUDE_DIR tiff.h
      PATHS /usr/local/include /usr/include
      PATH_SUFFIXES libtiff)

    find_library(LIBTIFF_LIBRARY NAMES tiff libtiff
      PATHS /usr/local/lib /usr/lib)

    if (LIBTIFF_INCLUDE_DIR AND LIBTIFF_LIBRARY)
      set(LIBTIFF_FOUND TRUE)
      set(LIBTIFF_INCLUDE_DIRS ${LIBTIFF_INCLUDE_DIR})
      set(LIBTIFF_LIBRARIES ${LIBTIFF_LIBRARY})
      message(STATUS "Found libtiff: ${LIBTIFF_LIBRARY}")
    else()
      message(WARNING "libtiff not found. Please install libtiff or disable NANOPDF_USE_LIBTIFF")
      message(STATUS "To build libtiff from ref/: cd ref/tiff-4.7.1 && mkdir -p build && cd build && cmake .. && make")
    endif()
  endif()

  if (LIBTIFF_FOUND)
    if (TARGET PkgConfig::LIBTIFF)
      # Use imported target from pkg-config
      target_link_libraries(${BUILD_TARGET} PUBLIC PkgConfig::LIBTIFF)
    else()
      # Use manual paths
      target_include_directories(${BUILD_TARGET} PRIVATE ${LIBTIFF_INCLUDE_DIRS})
      target_link_libraries(${BUILD_TARGET} PRIVATE ${LIBTIFF_LIBRARIES})
    endif()
    target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_LIBTIFF")
    message(STATUS "libtiff CCITT decoder enabled")
  endif()
endif ()

# Blend2D support
if (NANOPDF_USE_BLEND2D)
  # Disable AsmJit as requested
  set(BLEND2D_NO_JIT ON CACHE BOOL "Disable AsmJit for Blend2D" FORCE)
  set(BLEND2D_STATIC ON CACHE BOOL "Build Blend2D as static library" FORCE)

  # Add Blend2D subdirectory
  add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/blend2d EXCLUDE_FROM_ALL)

  # Add Blend2D include directory (for <blend2d.h>)
  target_include_directories(${BUILD_TARGET} PUBLIC ${PROJECT_SOURCE_DIR}/third_party/blend2d/blend2d)

  # Link Blend2D to nanopdf
  target_link_libraries(${BUILD_TARGET} PUBLIC blend2d::blend2d)
  target_compile_definitions(${BUILD_TARGET} PUBLIC "NANOPDF_USE_BLEND2D")

  message(STATUS "Blend2D support enabled (no JIT)")
endif()

# Only add sanitizers for non-WASM builds
if(NOT NANOPDF_BUILD_WASM)
  add_sanitizers(${BUILD_TARGET})
endif()

if(NOT NANOPDF_BUILD_WASM)
set(NANOPDF_C_API_HEADERS
  src/c/nanopdf_types.h
  src/c/nanopdf_c.h
  src/c/nanopdf_parse.h
  src/c/nanopdf_text.h
  src/c/nanopdf_forms.h
  src/c/nanopdf_write.h
)

set(NANOPDF_C_API_SOURCES
  src/c/nanopdf_alloc.c
  src/c/nanopdf_context.c
  src/c/nanopdf_crypto.c
  src/c/nanopdf_image_decoder.c
  src/c/nanopdf_jbig2.c
  src/c/nanopdf_jpx.c
  src/c/nanopdf_object.c
  src/c/nanopdf_basic_document.c
  src/c/nanopdf_basic_layout.c
  src/c/nanopdf_c_api.c
  src/c/nanopdf_parse_bridge.cc
  src/c/nanopdf_write.cc
  src/third_party/miniz.c
)

if(NANOPDF_C_USE_CPP_IMAGE_BRIDGE)
  list(APPEND NANOPDF_C_API_SOURCES
    src/c/nanopdf_image_decoder_bridge.cc
    src/jpx-decoder.cc
    src/jbig2/JBig2_BitStream.cc
    src/jbig2/JBig2_Image.cc
    src/jbig2/JBig2_Segment.cc
    src/jbig2/JBig2_SymbolDict.cc
    src/jbig2/JBig2_PatternDict.cc
    src/jbig2/JBig2_HuffmanTable.cc
    src/jbig2/JBig2_ArithDecoder.cc
    src/jbig2/JBig2_ArithIntDecoder.cc
    src/jbig2/JBig2_GrdProc.cc
    src/jbig2/JBig2_GrrdProc.cc
    src/jbig2/JBig2_Context.cc
    src/jbig2/JBig2_SddProc.cc
    src/jbig2/JBig2_TrdProc.cc
  )
endif()

add_library(nanopdf-c STATIC ${NANOPDF_C_API_SOURCES})
target_include_directories(nanopdf-c
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/c
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/c
          ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_property(TARGET nanopdf-c PROPERTY C_STANDARD 11)
set_property(TARGET nanopdf-c PROPERTY CXX_STANDARD 17)
set_property(TARGET nanopdf-c PROPERTY CXX_STANDARD_REQUIRED ON)
target_link_libraries(nanopdf-c PUBLIC ${BUILD_TARGET})
if(NANOPDF_C_USE_CPP_IMAGE_BRIDGE)
  target_compile_definitions(nanopdf-c PRIVATE NANOPDF_C_USE_CPP_IMAGE_BRIDGE=1)
  foreach(_nanopdf_cxx_option IN LISTS NANOPDF_CXX_OPTIONS)
    target_compile_options(nanopdf-c PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_nanopdf_cxx_option}>")
  endforeach()
  if(NOT MSVC)
    target_compile_options(nanopdf-c PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-function>"
      "$<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>"
      "$<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-variable>")
  endif()
  target_compile_definitions(nanopdf-c PRIVATE ${NANOPDF_CXX_DEFINES})
endif()
if(NOT NANOPDF_C_USE_CPP_IMAGE_BRIDGE)
  foreach(_nanopdf_cxx_option IN LISTS NANOPDF_CXX_OPTIONS)
    target_compile_options(nanopdf-c PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${_nanopdf_cxx_option}>")
  endforeach()
  if(NOT MSVC)
    target_compile_options(nanopdf-c PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-function>"
      "$<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>"
      "$<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-variable>")
  endif()
  target_compile_definitions(nanopdf-c PRIVATE ${NANOPDF_CXX_DEFINES})
endif()
add_sanitizers(nanopdf-c)

install(TARGETS nanopdf-c DESTINATION lib)
install(FILES ${NANOPDF_C_API_HEADERS} DESTINATION include/nanopdf-c)
endif()

# ============================================================
# MCP (Model Context Protocol) server
# ============================================================
if(NANOPDF_BUILD_MCP AND NOT NANOPDF_BUILD_WASM)
  # MCP library
  add_library(nanopdf_mcp
    src/mcp/mcp-json.cc
    src/mcp/mcp-protocol.cc
    src/mcp/mcp-transport.cc
    src/mcp/mcp-tools.cc
  )
  target_link_libraries(nanopdf_mcp PUBLIC ${BUILD_TARGET})
  target_include_directories(nanopdf_mcp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp)
  add_sanitizers(nanopdf_mcp)

  # MCP server executable
  add_executable(nanopdf-mcp src/mcp/mcp-server.cc)
  target_link_libraries(nanopdf-mcp PRIVATE nanopdf_mcp)
  add_sanitizers(nanopdf-mcp)

  # Install MCP server
  install(TARGETS nanopdf-mcp DESTINATION bin)

  message(STATUS "MCP server target enabled: nanopdf-mcp")
endif()

# Build test executables only if requested and not building for WASM
if(NANOPDF_BUILD_TESTS AND NOT NANOPDF_BUILD_WASM)
  enable_testing()

  # Feature-organized test infrastructure (nanotest-based)
  add_subdirectory(tests)

  # Helper function for standalone test executables (visual regression, etc.)
  function(add_nanopdf_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ${BUILD_TARGET})
    target_include_directories(${target} PRIVATE
      ${PROJECT_SOURCE_DIR}
      ${PROJECT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/src/third_party
    )
    add_sanitizers(${target})
    if(ARGC GREATER 2)
      add_test(NAME ${target} COMMAND ${target} ${ARGN})
    else()
      add_test(NAME ${target} COMMAND ${target})
    endif()
  endfunction()

  # ============================================================================
  # Tools and benchmarks (not tests)
  # ============================================================================

  # Benchmark executable (not a test, just a tool)
  add_executable(benchmark src/benchmark.cc)
  target_link_libraries(benchmark PRIVATE ${BUILD_TARGET})
  add_sanitizers(benchmark)
  if(NANOPDF_USE_THORVG AND THORVG_FOUND)
    if (TARGET PkgConfig::THORVG)
      target_link_libraries(benchmark PRIVATE PkgConfig::THORVG)
    else()
      target_include_directories(benchmark PRIVATE ${THORVG_INCLUDE_DIRS})
      target_link_libraries(benchmark PRIVATE ${THORVG_LIBRARIES})
    endif()
  endif()
  if(NANOPDF_USE_BLEND2D)
    target_link_libraries(benchmark PRIVATE blend2d::blend2d)
  endif()

  add_executable(ccitt_compare tools/ccitt_compare.cc)
  target_include_directories(ccitt_compare PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(ccitt_compare PRIVATE ${BUILD_TARGET})
  add_sanitizers(ccitt_compare)

  # Standalone visual comparison tool
  add_executable(visual_compare tools/visual_compare.cc)
  target_include_directories(visual_compare PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/third_party)
  add_sanitizers(visual_compare)

  # Visual regression test (LightVG backend — default; pure software, no
  # external graphics dependency).
  if(NANOPDF_USE_LIGHTVG)
    add_nanopdf_test(test_visual_lightvg src/test-visual-regression.cc)
    target_compile_definitions(test_visual_lightvg PRIVATE
      NANOPDF_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
      TEST_VISUAL_BACKEND_LIGHTVG)
  endif()

  # Visual regression test (ThorVG backend)
  if(NANOPDF_USE_THORVG AND THORVG_FOUND)
    add_nanopdf_test(test_visual_thorvg src/test-visual-regression.cc)
    target_compile_definitions(test_visual_thorvg PRIVATE
      NANOPDF_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
      TEST_VISUAL_BACKEND_THORVG)
    if (TARGET PkgConfig::THORVG)
      target_link_libraries(test_visual_thorvg PRIVATE PkgConfig::THORVG)
    else()
      target_include_directories(test_visual_thorvg PRIVATE ${THORVG_INCLUDE_DIRS})
      target_link_libraries(test_visual_thorvg PRIVATE ${THORVG_LIBRARIES})
    endif()
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
      target_link_libraries(test_visual_thorvg PRIVATE OpenMP::OpenMP_CXX)
    endif()
  endif()

  # Visual regression test (Blend2D backend)
  if(NANOPDF_USE_BLEND2D)
    add_nanopdf_test(test_visual_blend2d src/test-visual-regression.cc)
    target_compile_definitions(test_visual_blend2d PRIVATE
      NANOPDF_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
      TEST_VISUAL_BACKEND_BLEND2D)
    target_link_libraries(test_visual_blend2d PRIVATE blend2d::blend2d)
  endif()

  add_executable(nanopdf_c_api_smoke tests/c/test_nanopdf_c.c)
  target_link_libraries(nanopdf_c_api_smoke PRIVATE nanopdf-c)
  if(NANOPDF_C_USE_CPP_IMAGE_BRIDGE)
    target_compile_definitions(nanopdf_c_api_smoke PRIVATE NANOPDF_C_USE_CPP_IMAGE_BRIDGE=1)
  endif()
  target_include_directories(nanopdf_c_api_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/src/c
    ${PROJECT_SOURCE_DIR}/tests)
  set_property(TARGET nanopdf_c_api_smoke PROPERTY C_STANDARD 11)
  set_property(TARGET nanopdf_c_api_smoke PROPERTY LINKER_LANGUAGE CXX)
  add_sanitizers(nanopdf_c_api_smoke)
  add_test(NAME nanopdf_c_api_smoke COMMAND nanopdf_c_api_smoke)
  set_tests_properties(nanopdf_c_api_smoke PROPERTIES LABELS "unit")
endif()

# Custom target to generate visual reference images
add_custom_target(generate_visual_refs
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/generate-visual-refs.sh
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  COMMENT "Generating visual reference images using pdftoppm"
)

# CTest targets for visual regression
enable_testing()
if(NANOPDF_USE_THORVG AND THORVG_FOUND)
  add_test(NAME test_visual_shapes_thorvg
    COMMAND test_visual_thorvg
      test_cmyk.pdf test_linestyles.pdf test_curves.pdf
      test_winding.pdf test_transforms.pdf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  add_test(NAME test_visual_external_thorvg
    COMMAND test_visual_thorvg --suite ${PROJECT_SOURCE_DIR}/data/external
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()
if(NANOPDF_USE_BLEND2D)
  add_test(NAME test_visual_shapes_blend2d
    COMMAND test_visual_blend2d
      test_cmyk.pdf test_linestyles.pdf test_curves.pdf
      test_winding.pdf test_transforms.pdf
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()

# For WASM builds, create a JavaScript module
if(NANOPDF_BUILD_WASM)
  # Set output as JavaScript module with --no-entry (no main function)
  set_target_properties(${BUILD_TARGET} PROPERTIES
    SUFFIX ".js"
    LINK_FLAGS "-s WASM=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS','stringToNewUTF8','UTF8ToString','HEAPU8'] -s EXPORTED_FUNCTIONS=['_malloc','_free'] -s USE_ZLIB=1 -s ALLOW_MEMORY_GROWTH=1 --no-entry"
  )

  # Copy .js and .wasm files to examples/wasm/src/ after build
  add_custom_command(TARGET ${BUILD_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE_DIR:${BUILD_TARGET}>/${BUILD_TARGET}.js
      ${PROJECT_SOURCE_DIR}/examples/wasm/src/${BUILD_TARGET}.js
    COMMAND ${CMAKE_COMMAND} -E copy
      $<TARGET_FILE_DIR:${BUILD_TARGET}>/${BUILD_TARGET}.wasm
      ${PROJECT_SOURCE_DIR}/examples/wasm/src/${BUILD_TARGET}.wasm
    COMMENT "Copying WASM build artifacts to examples/wasm/src/"
  )

  # Install headers for potential Node.js usage
  install(FILES src/nanopdf.hh DESTINATION include)
  install(TARGETS ${BUILD_TARGET} DESTINATION lib)
endif()

add_custom_target(ci
  COMMAND ${CMAKE_COMMAND} -P ${PROJECT_SOURCE_DIR}/scripts/run-ci.cmake
  COMMENT "Configure, build, and test Release + Debug via scripts/run-ci.cmake"
)

## [VisualStudio]
#if(MSVC)
#
#  # For easier debugging in VS IDE(cmake 3.8.0 or later required) Set working
#  # directory to ${BUILD_TARGET} git repo root.
#  if(CMAKE_VERSION VERSION_GREATER 3.8.0)
#    set_target_properties(lazulite-i
#                             PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY
#                                      "${CMAKE_CURRENT_SOURCE_DIR}")
#  endif()
#
#  ## For easier debugging in VS IDE, set a demo scene. 
#  #if(CMAKE_VERSION VERSION_GREATER 3.13.0)
#  #    set_target_properties(pbrlab-cli
#  #                          PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS
#  #                                     "./data/cornellbox_suzanne_lucy.obj")
#  #endif()
#
#  set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT pbrlab-cli)
#endif()
