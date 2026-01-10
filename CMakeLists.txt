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
option(NANOPDF_USE_THORVG "Use ThorVG for rendering backend" OFF)
option(NANOPDF_USE_BLEND2D "Use Blend2D for rendering backend" OFF)

# cmake modules
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/sanitizers)
find_package(Sanitizers) # Address sanitizer (-DSANITIZE_ADDRESS=ON)

# Overall compile options
#set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_GLIBCXX_DEBUG -D_LIBCPP_DEBUG")
#set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_GLIBCXX_DEBUG -D_LIBCPP_DEBUG")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS'] -s EXPORTED_FUNCTIONS=['_malloc','_free']")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s USE_ZLIB=1")
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
  src/jbig2-decoder.cc
  src/jpx-decoder.cc
  src/canvas-exporter.cc
  src/crypto.cc
  src/pdf-security.cc
  src/pdf-attachments.cc
  src/color-transform.cc
  src/tiff-decoder.cc
  src/text-layout.cc
  src/table-extraction.cc
  src/form-manipulation.cc
  src/document-structure.cc
  src/stb_image_impl.cc
  src/stb_image_write_impl.cc
  src/stb_truetype_impl.cc
  src/tiny_dng_impl.cc
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
)

if(NANOPDF_USE_THORVG)
  list(APPEND NANOPDF_SOURCES src/thorvg-backend.cc)
endif()

if(NANOPDF_USE_BLEND2D)
  list(APPEND NANOPDF_SOURCES src/blend2d-backend.cc)
endif()

# Add WASM API for Emscripten builds
if(NANOPDF_BUILD_WASM)
  list(APPEND NANOPDF_SOURCES src/wasm-api.cc)
endif()

add_library(${BUILD_TARGET} ${NANOPDF_SOURCES})
target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_DEBUG_PRINT=1")

# Link with zlib - Emscripten provides it via USE_ZLIB flag
if(NANOPDF_BUILD_WASM)
  # For Emscripten, USE_ZLIB must be specified for both compilation and linking
  target_compile_options(${BUILD_TARGET} PRIVATE "SHELL:-s USE_ZLIB=1")
  target_link_options(${BUILD_TARGET} PRIVATE "SHELL:-s USE_ZLIB=1")
else()
  target_link_libraries(${BUILD_TARGET} PUBLIC z)
endif()

if (NANOPDF_USE_NANOSTL)
  # nanostl
  target_include_directories(${BUILD_TARGET} PRIVATE src/nanostl/include)
  target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_NANOSTL")
endif ()

if (NANOPDF_USE_STB_TRUETYPE)
  target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_STB_TRUETYPE")
endif ()

if (NANOPDF_USE_THORVG)
  # Find ThorVG package or use FetchContent
  find_package(PkgConfig)
  if (PkgConfig_FOUND)
    pkg_check_modules(THORVG IMPORTED_TARGET thorvg)
  endif()

  if (NOT THORVG_FOUND)
    # Fallback to manual inclusion if ThorVG is installed locally
    # Assumes ThorVG headers are in /usr/local/include/thorvg or similar
    find_path(THORVG_INCLUDE_DIR thorvg.h
      PATHS /usr/local/include/thorvg /usr/include/thorvg
      PATH_SUFFIXES thorvg)

    find_library(THORVG_LIBRARY NAMES thorvg
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
    endif()
    target_compile_definitions(${BUILD_TARGET} PUBLIC "NANOPDF_USE_THORVG")
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

# Build test executables only if requested and not building for WASM
if(NANOPDF_BUILD_TESTS AND NOT NANOPDF_BUILD_WASM)
  enable_testing()

  function(add_nanopdf_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE ${BUILD_TARGET})
    add_sanitizers(${target})
    if(ARGC GREATER 2)
      add_test(NAME ${target} COMMAND ${target} ${ARGN})
    else()
      add_test(NAME ${target} COMMAND ${target})
    endif()
  endfunction()

  add_nanopdf_test(${TEST_TARGET} src/test-nanopdf.cc ${PROJECT_SOURCE_DIR}/data/blank.pdf)

  set(_NANOPDF_PHASE_TEST_SPECS
    "test_phase1|src/test-phase1-features.cc"
    "test_phase1_simple|src/test-phase1-simple.cc"
    "test_phase1_attachments|test_phase1_attachments.cc"
    "test_phase1_color|test_phase1_color.cc"
    "test_phase1_tiff|test_phase1_tiff.cc"
    "test_phase2|src/test-phase2-features.cc"
    "test_phase2_standardencoding|src/test-phase2-standardencoding.cc"
    "test_phase2_rendering|src/test-phase2-rendering.cc"
    "test_phase2_realpdf|src/test-phase2-real-pdfs.cc"
    "test_phase2_text|src/test-phase2-text-extraction.cc"
    "test_phase2_text_layout|test_phase2_text_layout.cc"
    "test_phase2_text_layout_integration|test_phase2_text_layout_integration.cc"
    "test_phase2_table_extraction|test_phase2_table_extraction.cc"
    "test_phase3|src/test-phase3-features.cc"
    "test_phase3_form_manipulation|test_phase3_form_manipulation.cc"
    "test_phase4|src/test-phase4-features.cc"
    "test_phase4_document_structure|test_phase4_document_structure.cc"
    "test_phase5|src/test-phase5-features.cc"
    "test_phase6|src/test-phase6-features.cc"
  )

  foreach(_spec ${_NANOPDF_PHASE_TEST_SPECS})
    string(REPLACE "|" ";" _spec_items "${_spec}")
    list(GET _spec_items 0 _test_name)
    list(GET _spec_items 1 _test_source)
    add_nanopdf_test(${_test_name} ${_test_source})
  endforeach()

  if(TARGET test_phase2_realpdf)
    target_compile_definitions(test_phase2_realpdf PRIVATE NANOPDF_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  endif()

  # ThorVG backend test
  if(NANOPDF_USE_THORVG AND THORVG_FOUND)
    add_nanopdf_test(test_thorvg src/test-thorvg-backend.cc ${PROJECT_SOURCE_DIR}/data/blank.pdf)
    if (TARGET PkgConfig::THORVG)
      target_link_libraries(test_thorvg PRIVATE PkgConfig::THORVG)
    else()
      target_include_directories(test_thorvg PRIVATE ${THORVG_INCLUDE_DIRS})
      target_link_libraries(test_thorvg PRIVATE ${THORVG_LIBRARIES})
    endif()
  endif()

  # Blend2D backend test
  if(NANOPDF_USE_BLEND2D)
    add_nanopdf_test(test_blend2d src/test-blend2d-backend.cc ${PROJECT_SOURCE_DIR}/data/blank.pdf)
    target_link_libraries(test_blend2d PRIVATE blend2d::blend2d)
  endif()

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
endif()

# For WASM builds, create a JavaScript module
if(NANOPDF_BUILD_WASM)
  # Set output as JavaScript module
  set_target_properties(${BUILD_TARGET} PROPERTIES
    SUFFIX ".js"
    LINK_FLAGS "-s WASM=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','FS'] -s EXPORTED_FUNCTIONS=['_malloc','_free'] -s USE_ZLIB=1 -s ALLOW_MEMORY_GROWTH=1"
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
