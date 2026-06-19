cmake_minimum_required(VERSION 3.5)

project(nanopdf_rasterize_example)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nanopdf library (assumes it's built in the parent build directory)
set(NANOPDF_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(NOT DEFINED NANOPDF_BUILD_DIR OR "${NANOPDF_BUILD_DIR}" STREQUAL "")
  set(NANOPDF_BUILD_DIR ${NANOPDF_ROOT}/build CACHE PATH "Path to nanopdf build directory")
endif()

# Include nanopdf headers
include_directories(${NANOPDF_ROOT}/src)
include_directories(${NANOPDF_ROOT}/src/third_party)

# Render backends
option(USE_LIGHTVG "Enable lightui VG software backend (default)" ON)
if(USE_LIGHTVG)
  add_compile_definitions(NANOPDF_USE_LIGHTVG)
  include_directories(${NANOPDF_ROOT}/src/third_party/lightvg/include
                      ${NANOPDF_ROOT}/src/third_party/lightvg/src)
endif()

# Find ThorVG if enabled. LightVG is the default rasterize backend; ThorVG is
# only needed when explicitly selecting --backend thorvg.
option(USE_THORVG "Enable optional ThorVG backend" OFF)
if(USE_THORVG)
  # First check if ThorVG is in the dist/ directory
  set(THORVG_DIST_DIR ${NANOPDF_ROOT}/dist)
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
  if(NOT THORVG_FOUND)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
      pkg_check_modules(THORVG IMPORTED_TARGET thorvg)
    endif()
  endif()

  if(NOT THORVG_FOUND)
    # Try to find ThorVG manually
    find_path(THORVG_INCLUDE_DIR thorvg.h
      PATHS /usr/local/include/thorvg /usr/include/thorvg
      PATH_SUFFIXES thorvg thorvg-1)

    find_library(THORVG_LIBRARY NAMES thorvg thorvg-1
      PATHS /usr/local/lib /usr/lib
      PATH_SUFFIXES thorvg)

    if(THORVG_INCLUDE_DIR AND THORVG_LIBRARY)
      set(THORVG_FOUND TRUE)
      set(THORVG_INCLUDE_DIRS ${THORVG_INCLUDE_DIR})
      set(THORVG_LIBRARIES ${THORVG_LIBRARY})
      message(STATUS "Found ThorVG: ${THORVG_LIBRARY}")
    else()
      message(STATUS "ThorVG not found. The optional --backend thorvg path will be unavailable.")
    endif()
  endif()

  if(THORVG_FOUND)
    add_compile_definitions(NANOPDF_USE_THORVG)
    if(NOT TARGET PkgConfig::THORVG)
      include_directories(${THORVG_INCLUDE_DIRS})
    endif()
  endif()
endif()

# Embed fonts option (enabled by default for rasterize)
option(EMBED_FONTS "Embed PDF Standard 14 alternative fonts as binary data" ON)

if(EMBED_FONTS)
  set(FONTS_DIR ${NANOPDF_ROOT}/fonts)
  set(EMBEDDED_FONTS_HEADER ${CMAKE_CURRENT_BINARY_DIR}/embedded-fonts.hh)
  set(EMBEDDED_FONTS_ASM ${CMAKE_CURRENT_BINARY_DIR}/embedded-fonts.S)
  set(EMBEDDED_FONTS_CC ${CMAKE_CURRENT_BINARY_DIR}/embedded-fonts.cc)

  # Find all font files to establish dependencies
  file(GLOB_RECURSE FONT_FILES
    "${FONTS_DIR}/*.ttf"
    "${FONTS_DIR}/*.otf"
  )

  if(FONT_FILES)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(
      OUTPUT ${EMBEDDED_FONTS_HEADER} ${EMBEDDED_FONTS_ASM} ${EMBEDDED_FONTS_CC}
      COMMAND ${Python3_EXECUTABLE} ${NANOPDF_ROOT}/cmake/embed_fonts_binary.py
        ${FONTS_DIR} ${CMAKE_CURRENT_BINARY_DIR} embedded-fonts
        --exclude-dirs=noto-sans-jp,noto-serif-jp
      DEPENDS ${FONT_FILES} ${NANOPDF_ROOT}/cmake/embed_fonts_binary.py
      COMMENT "Generating embedded fonts for rasterize (incbin)"
      VERBATIM
    )

    add_custom_target(generate_embedded_fonts_rasterize
      DEPENDS ${EMBEDDED_FONTS_HEADER} ${EMBEDDED_FONTS_ASM} ${EMBEDDED_FONTS_CC}
    )
    set_source_files_properties(${EMBEDDED_FONTS_HEADER} ${EMBEDDED_FONTS_ASM}
                                ${EMBEDDED_FONTS_CC} PROPERTIES GENERATED TRUE)
    enable_language(ASM)

    message(STATUS "Font embedding enabled for rasterize (${FONTS_DIR}, incbin)")
  else()
    message(WARNING "Font embedding enabled but no fonts found in ${FONTS_DIR}")
    set(EMBED_FONTS OFF)
  endif()
endif()

# Create the rasterize executable
if(EMBED_FONTS)
  add_executable(rasterize rasterize.cc ${EMBEDDED_FONTS_ASM} ${EMBEDDED_FONTS_CC})
  add_dependencies(rasterize generate_embedded_fonts_rasterize)
  target_include_directories(rasterize PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${NANOPDF_ROOT}/src)
  target_compile_definitions(rasterize PRIVATE "NANOPDF_EMBED_FONTS=1" "NANOPDF_USE_MINIZ")
else()
  add_executable(rasterize rasterize.cc)
endif()

# Link with nanopdf library
if(EXISTS ${NANOPDF_BUILD_DIR}/libnanopdf.a)
  target_link_libraries(rasterize ${NANOPDF_BUILD_DIR}/libnanopdf.a)
else()
  message(FATAL_ERROR "nanopdf library not found. Please build nanopdf first.")
endif()

# Link with ThorVG if found
if(THORVG_FOUND)
  if(TARGET PkgConfig::THORVG)
    target_link_libraries(rasterize PkgConfig::THORVG)
  else()
    target_link_libraries(rasterize ${THORVG_LIBRARIES})
    # Set RPATH so the executable can find ThorVG at runtime
    if(THORVG_LIBRARY_DIRS)
      set_target_properties(rasterize PROPERTIES
        BUILD_RPATH "${THORVG_LIBRARY_DIRS}"
        INSTALL_RPATH "${THORVG_LIBRARY_DIRS}")
    endif()
  endif()
endif()

if(NOT USE_LIGHTVG AND NOT THORVG_FOUND)
  message(FATAL_ERROR "No raster backend enabled. Enable USE_LIGHTVG or provide ThorVG.")
endif()

# Link with zlib (required by nanopdf)
find_package(ZLIB REQUIRED)
target_link_libraries(rasterize ZLIB::ZLIB)

# Link with libtiff if nanopdf was built with it
option(USE_LIBTIFF "Enable libtiff for CCITT decoding" ON)
if(USE_LIBTIFF)
  # Try to find libtiff in ref/tiff-4.7.1 first
  set(LIBTIFF_REF_DIR ${NANOPDF_ROOT}/ref/tiff-4.7.1)
  if(EXISTS ${LIBTIFF_REF_DIR}/libtiff/tiff.h)
    find_library(LIBTIFF_LIBRARY NAMES tiff
      PATHS ${LIBTIFF_REF_DIR}/build/libtiff
      NO_DEFAULT_PATH)
    if(LIBTIFF_LIBRARY)
      set(LIBTIFF_FOUND TRUE)
      set(LIBTIFF_INCLUDE_DIRS ${LIBTIFF_REF_DIR}/libtiff)
      set(LIBTIFF_LIBRARIES ${LIBTIFF_LIBRARY})
      message(STATUS "Found libtiff in ref/: ${LIBTIFF_LIBRARY}")
    endif()
  endif()

  # Fall back to system libtiff
  if(NOT LIBTIFF_FOUND)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
      pkg_check_modules(LIBTIFF IMPORTED_TARGET libtiff-4)
      if(NOT LIBTIFF_FOUND)
        pkg_check_modules(LIBTIFF IMPORTED_TARGET libtiff)
      endif()
    endif()
  endif()

  if(LIBTIFF_FOUND)
    add_compile_definitions(NANOPDF_USE_LIBTIFF)
    if(TARGET PkgConfig::LIBTIFF)
      target_link_libraries(rasterize PkgConfig::LIBTIFF)
    else()
      include_directories(${LIBTIFF_INCLUDE_DIRS})
      target_link_libraries(rasterize ${LIBTIFF_LIBRARIES})
    endif()
    message(STATUS "libtiff CCITT decoder enabled for rasterize")
  endif()
endif()

# Platform-specific libraries
if(UNIX AND NOT APPLE)
  target_link_libraries(rasterize pthread)
endif()

# OpenMP for optional backend parallel processing
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
  target_link_libraries(rasterize OpenMP::OpenMP_CXX)
endif()

# Installation
install(TARGETS rasterize DESTINATION bin)

# Print configuration summary
message(STATUS "")
message(STATUS "Rasterize Example Configuration:")
message(STATUS "  LightVG support: ${USE_LIGHTVG}")
message(STATUS "  ThorVG support: ${THORVG_FOUND}")
if(THORVG_FOUND)
  message(STATUS "  ThorVG include: ${THORVG_INCLUDE_DIRS}")
  message(STATUS "  ThorVG library: ${THORVG_LIBRARIES}")
endif()
message(STATUS "")
