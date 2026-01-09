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
option(NANOPDF_USE_LIBTIFF "Use libtiff for CCITT/Fax decoding" OFF)
option(NANOPDF_USE_POPPLER_CCITT "Use Poppler for CCITT/Fax decoding (GPL)" OFF)

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
  src/stb_image_impl.cc
  src/stb_image_write_impl.cc
  src/stb_truetype_impl.cc
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
target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_LOG_LEVEL=${NANOPDF_LOG_LEVEL}")

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
  # First check if ThorVG is in the dist/ directory
  set(THORVG_DIST_DIR ${CMAKE_SOURCE_DIR}/dist)
  if(EXISTS ${THORVG_DIST_DIR}/include/thorvg-1/thorvg.h)
    set(THORVG_FOUND TRUE)
    set(THORVG_INCLUDE_DIRS ${THORVG_DIST_DIR}/include/thorvg-1)
    # Find the library based on architecture
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
      set(THORVG_LIB_DIR ${THORVG_DIST_DIR}/lib/x86_64-linux-gnu)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
      set(THORVG_LIB_DIR ${THORVG_DIST_DIR}/lib/aarch64-linux-gnu)
    else()
      set(THORVG_LIB_DIR ${THORVG_DIST_DIR}/lib/${CMAKE_SYSTEM_PROCESSOR}-linux-gnu)
    endif()
    find_library(THORVG_LIBRARY NAMES thorvg-1 thorvg
      PATHS ${THORVG_LIB_DIR}
      NO_DEFAULT_PATH)
    if(THORVG_LIBRARY)
      set(THORVG_LIBRARIES ${THORVG_LIBRARY})
      set(THORVG_LIBRARY_DIRS ${THORVG_LIB_DIR})
      message(STATUS "Found ThorVG in dist/: ${THORVG_LIBRARY}")
    else()
      set(THORVG_FOUND FALSE)
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

# Poppler CCITT decoder support (GPL-licensed)
if (NANOPDF_USE_POPPLER_CCITT)
  message(STATUS "")
  message(WARNING "NANOPDF_USE_POPPLER_CCITT is enabled. This links against GPL-licensed Poppler.")
  message(WARNING "Your binary will be subject to GPL-2.0 licensing requirements.")
  message(STATUS "")

  # Try to find poppler in ref/poppler first
  set(POPPLER_REF_DIR ${CMAKE_SOURCE_DIR}/ref/poppler)
  if(EXISTS ${POPPLER_REF_DIR}/poppler/Stream.h)
    set(POPPLER_FOUND TRUE)
    set(POPPLER_INCLUDE_DIRS ${POPPLER_REF_DIR} ${POPPLER_REF_DIR}/poppler)
    # Try to find built library
    find_library(POPPLER_LIBRARY NAMES poppler
      PATHS ${POPPLER_REF_DIR}/build
      NO_DEFAULT_PATH)
    if(POPPLER_LIBRARY)
      set(POPPLER_LIBRARIES ${POPPLER_LIBRARY})
      message(STATUS "Found Poppler in ref/: ${POPPLER_LIBRARY}")
    else()
      set(POPPLER_FOUND FALSE)
      message(STATUS "Poppler headers found in ref/ but library not built")
      message(STATUS "Build poppler: cd ref/poppler/build && cmake .. && make")
    endif()
  endif()

  if (POPPLER_FOUND)
    # Poppler requires C++17, so upgrade project standard
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    message(STATUS "Upgraded to C++17 for Poppler compatibility")

    # Add poppler wrapper to build
    target_sources(${BUILD_TARGET} PRIVATE src/poppler-ccitt-wrapper.cc)
    # Include both source and build directories (for poppler-config.h)
    target_include_directories(${BUILD_TARGET} PRIVATE
      ${POPPLER_INCLUDE_DIRS}
      ${POPPLER_REF_DIR}/build
      ${POPPLER_REF_DIR}/build/poppler)
    target_link_libraries(${BUILD_TARGET} PRIVATE ${POPPLER_LIBRARIES})
    target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_POPPLER_CCITT")
    message(STATUS "Poppler CCITT decoder enabled (GPL)")
  else()
    message(WARNING "Poppler not found. Please build from ref/poppler or disable NANOPDF_USE_POPPLER_CCITT")
  endif()
endif()

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
    "test_phase2|src/test-phase2-features.cc"
    "test_phase2_standardencoding|src/test-phase2-standardencoding.cc"
    "test_phase2_rendering|src/test-phase2-rendering.cc"
    "test_phase2_realpdf|src/test-phase2-real-pdfs.cc"
    "test_phase2_text|src/test-phase2-text-extraction.cc"
    "test_phase3|src/test-phase3-features.cc"
    "test_phase4|src/test-phase4-features.cc"
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
    add_nanopdf_test(test_thorvg src/test-thorvg-backend.cc ${PROJECT_SOURCE_DIR}/data/HelloWorld_xr.pdf)
    if (TARGET PkgConfig::THORVG)
      target_link_libraries(test_thorvg PRIVATE PkgConfig::THORVG)
    else()
      target_include_directories(test_thorvg PRIVATE ${THORVG_INCLUDE_DIRS})
      target_link_libraries(test_thorvg PRIVATE ${THORVG_LIBRARIES})
    endif()
  endif()

  # Blend2D backend test
  if(NANOPDF_USE_BLEND2D)
    add_nanopdf_test(test_blend2d src/test-blend2d-backend.cc ${PROJECT_SOURCE_DIR}/data/HelloWorld_xr.pdf)
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
