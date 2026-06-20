cmake_minimum_required(VERSION 3.16)
project(nanopdf_pdfview LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --- nanopdf (prebuilt static lib; provides the single shared lvg_ lightvg) ---
set(NANOPDF_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(NOT DEFINED NANOPDF_BUILD_DIR OR "${NANOPDF_BUILD_DIR}" STREQUAL "")
  set(NANOPDF_BUILD_DIR ${NANOPDF_ROOT}/build CACHE PATH "Path to nanopdf build dir")
endif()
if(NOT EXISTS ${NANOPDF_BUILD_DIR}/libnanopdf.a)
  message(FATAL_ERROR
    "libnanopdf.a not found in ${NANOPDF_BUILD_DIR}. Build nanopdf first:\n"
    "  cmake -S ${NANOPDF_ROOT} -B ${NANOPDF_ROOT}/build -DCMAKE_BUILD_TYPE=Release && cmake --build ${NANOPDF_ROOT}/build")
endif()

set(LUI ${CMAKE_CURRENT_SOURCE_DIR}/deps/lightui)

# --- lightui UI sources -------------------------------------------------------
# Compile lightui's widget/core layer, lighttype font rasteriser, the X11
# platform backend, and lightvg's dirty-region tracker (lvg_dirty_*, used by
# layout.c/text_layout.c and NOT part of libnanopdf.a).
#
# We deliberately do NOT compile lightvg's canvas.c / surface.c /
# backend_registry.c here — those lvg_* symbols come from libnanopdf.a (the
# single shared lightvg copy). Compiling them again would duplicate symbols.
file(GLOB LIGHTUI_CORE_SOURCES CONFIGURE_DEPENDS ${LUI}/src/*.c)
set(LIGHTUI_SOURCES
  ${LIGHTUI_CORE_SOURCES}
  ${LUI}/src/internal/lui_log.c
  ${LUI}/src/platform/x11/platform_x11.c
  ${LUI}/lighttype/src/ttf_parse.c
  ${LUI}/lighttype/src/rasterize.c
  ${LUI}/lighttype/src/font.c
  ${LUI}/lightvg/src/dirty.c
)

# --- Embedded MCP server (lightui MCP) for LLM/VLM-driven control + debug ----
option(PDFVIEW_MCP "Build with the embedded MCP server" ON)
if(PDFVIEW_MCP)
  file(GLOB LIGHTUI_MCP_SOURCES CONFIGURE_DEPENDS ${LUI}/src/mcp/*.c)
  list(APPEND LIGHTUI_SOURCES ${LIGHTUI_MCP_SOURCES})
endif()

# --- nativefiledialog-ng (Open/Save dialogs) ---------------------------------
# Linux: the xdg-desktop-portal backend (dbus only — no hard GTK link). Wayland
# parent-window association is omitted (X11 focus), so only libdbus-1 is needed.
set(NFD ${CMAKE_CURRENT_SOURCE_DIR}/deps/nativefiledialog-ng)
set(NFD_SOURCES "")
set(NFD_DEFINES "")
set(NFD_LIBS "")
if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DBUS REQUIRED dbus-1)
  set(NFD_SOURCES ${NFD}/src/nfd_portal.cpp)
  set(NFD_DEFINES NFD_PORTAL)
  set(NFD_LIBS ${DBUS_LIBRARIES})
elseif(WIN32)
  set(NFD_SOURCES ${NFD}/src/nfd_win.cpp)
endif()

add_executable(pdfview
  pdfview.cc
  pdf_document.cc
  pdf_debug.cc
  pdf_sign.cc
  ${NFD_SOURCES}
  ${LIGHTUI_SOURCES}
)

# OpenSSL is OPTIONAL. The pure-C++ stack in libnanopdf (crypto-pk/asn1-der/cms/
# rfc3161/pkcs12) signs, http-timestamps and verifies with no OpenSSL; OpenSSL is
# only needed for https TSAs (TLS) and the legacy signing path. Disable with
# -DPDFVIEW_USE_OPENSSL=OFF for an OpenSSL-free build.
option(PDFVIEW_USE_OPENSSL "Link OpenSSL for https TSAs / legacy signing" ON)
if(PDFVIEW_USE_OPENSSL)
  find_package(OpenSSL)
  if(OpenSSL_FOUND)
    target_link_libraries(pdfview PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(pdfview PRIVATE PDFVIEW_HAVE_OPENSSL=1)
    message(STATUS "pdfview: OpenSSL enabled (https TSAs available)")
  else()
    message(STATUS "pdfview: OpenSSL not found; building OpenSSL-free "
                   "(http TSAs only)")
  endif()
else()
  message(STATUS "pdfview: OpenSSL disabled; building OpenSSL-free "
                 "(http TSAs only)")
endif()
if(NFD_SOURCES)
  set_source_files_properties(${NFD_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "${NFD_DEFINES}"
    INCLUDE_DIRECTORIES "${NFD}/src/include;${DBUS_INCLUDE_DIRS}")
  target_compile_definitions(pdfview PRIVATE PDFVIEW_HAVE_NFD=1)
endif()

# lightui core is C and must be built with the platform + font feature flags it
# expects. LVG_* PNG/JPEG flags are NOT set: the lvg surface writers live in
# libnanopdf.a (compiled without them); the UI decodes images via bundled Wuffs.
set_source_files_properties(${LIGHTUI_SOURCES} PROPERTIES
  COMPILE_DEFINITIONS "LUI_PLATFORM_X11;LUI_HAVE_FONTS")

target_include_directories(pdfview PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  # lightui public + private headers
  ${LUI}/include
  ${LUI}/lighttype/include
  ${LUI}/lightvg/include
  ${LUI}/src
  ${LUI}/src/mcp
  ${LUI}/src/platform
  ${LUI}/lighttype/src
  ${LUI}/third_party
  # nanopdf headers
  ${NANOPDF_ROOT}/src
  ${NANOPDF_ROOT}/src/third_party
  # native file dialog
  ${NFD}/src/include
)

# Shared fonts: UI text reuses nanopdf's bundled fonts (no separate UI font set).
target_compile_definitions(pdfview PRIVATE
  "PDFVIEW_FONTS_DIR=\"${NANOPDF_ROOT}/fonts\"")

# Enable warnings on pdfview's own sources only (the bundled lightui C layer is
# compiled as-is). Keep these clean.
if(NOT MSVC)
  set_source_files_properties(pdfview.cc pdf_document.cc pdf_debug.cc
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
endif()

if(PDFVIEW_MCP)
  target_compile_definitions(pdfview PRIVATE PDFVIEW_HAVE_MCP=1)
  if(WIN32)
    target_link_libraries(pdfview PRIVATE ws2_32)
  endif()
endif()

target_link_libraries(pdfview PRIVATE ${NANOPDF_BUILD_DIR}/libnanopdf.a)

find_package(ZLIB REQUIRED)
target_link_libraries(pdfview PRIVATE ZLIB::ZLIB)

if(NFD_LIBS)
  target_link_libraries(pdfview PRIVATE ${NFD_LIBS})
  target_link_directories(pdfview PRIVATE ${DBUS_LIBRARY_DIRS})
endif()

# X11 backend dlopen's libX11/libXext at runtime -> needs libdl. Plus m/rt and
# pthread (nanopdf + lightvg AGG path).
target_link_libraries(pdfview PRIVATE ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(pdfview PRIVATE m pthread)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(pdfview PRIVATE rt)
  endif()
endif()

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
  target_link_libraries(pdfview PRIVATE OpenMP::OpenMP_CXX)
endif()

install(TARGETS pdfview DESTINATION bin)

message(STATUS "pdfview: nanopdf lib = ${NANOPDF_BUILD_DIR}/libnanopdf.a")
message(STATUS "pdfview: lightui deps = ${LUI}")
