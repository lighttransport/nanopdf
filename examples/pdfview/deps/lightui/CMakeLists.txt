cmake_minimum_required(VERSION 3.15)
project(lightui
    VERSION   0.1.0
    LANGUAGES C
    DESCRIPTION "Lightweight 2D UI library — software-rendered, HiDPI-first"
)

# Default to Release if the developer didn't pick a build type. The
# software AA rasteriser is ~5× slower without -O2; a Debug-default
# would be misleading for tiger_demo / drawing_demo perf comparisons.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug/Release/RelWithDebInfo/MinSizeRel)" FORCE)
endif()

# ---- Language standard -------------------------------------------------------
set(CMAKE_C_STANDARD    99)
set(CMAKE_C_EXTENSIONS  OFF)

# Enable optional PNG export in the VG module (see lightvg/README.md).
add_compile_definitions(LVG_ENABLE_PNG LVG_ENABLE_JPEG)

# MSVC: treat source and execution charsets as UTF-8 so CJK literals in
# examples/tests compile correctly regardless of the system code page (e.g. 932).
if(MSVC)
    add_compile_options(/utf-8)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/lightvg/include)

# ---- Build options -----------------------------------------------------------
option(LUI_BUILD_MCP       "Build MCP (Model Context Protocol) server for LLM debugging" ON)
option(LUI_BUILD_FREETYPE  "Build with FreeType/HarfBuzz font backend for comparison"    OFF)

# ---- Core sources ------------------------------------------------------------
set(LIGHTUI_CORE_SOURCES
    src/internal/lui_log.c
    src/button.c
    src/lightui.c
    src/frame_clock.c
    src/layout.c
    src/widget_cache.c
    src/scroll.c
    src/slider.c
    src/checkbox.c
    src/colorwheel.c
    src/curves.c
    src/timeline.c
    src/nodegraph.c
    src/combo.c
    src/chat.c
    src/treeview.c
    src/table.c
    src/plot.c
    src/menu.c
    src/histogram.c
    src/toolbar.c
    src/progress.c
    src/splitter.c
    src/viewport.c
    src/console.c
    src/dopesheet.c
    src/tabs.c
    src/heatmap.c
    src/gradient.c
    src/tooltip.c
    src/minimap.c
    src/spinner.c
    src/toggle.c
    src/radio.c
    src/knob.c
    src/numentry.c
    src/dialog.c
    src/toast.c
    src/statusbar.c
    src/accordion.c
    src/breadcrumb.c
    src/palette.c
    src/draglist.c
    src/badge.c
    src/propgrid.c
    src/layerstack.c
    src/rangeslider.c
    src/gauge.c
    src/filebrowser.c
    src/taginput.c
    src/searchbar.c
    src/popover.c
    src/ruler.c
    src/stepper.c
    src/rating.c
    src/imagecrop.c
    src/waveform.c
    src/pagination.c
    src/card.c
    src/sparkline.c
    src/theme.c
    src/text_layout.c
    src/markdown.c
    src/markdown_render.c
    src/markdown_widget.c
    src/html.c
    src/html_render.c
    src/html_widget.c
    src/export_record.c
    src/export_svg.c
    src/export_pdf.c
    src/image.c
    # Font support via LightType (custom parser + rasterizer, no external deps)
    lighttype/src/ttf_parse.c
    lighttype/src/rasterize.c
    lighttype/src/font.c
    src/font_bridge.c
    src/label.c
    src/text_input.c
    src/text_edit.c
    # Image comparison framework
    src/image_cmp.c
)

if(LUI_BUILD_MCP)
    list(APPEND LIGHTUI_CORE_SOURCES
        src/mcp/lui_json.c
        src/mcp/lui_net.c
        src/mcp/lui_http.c
        src/mcp/lui_image_enc.c
        src/mcp/mcp_server.c
        src/mcp/mcp_tools.c
    )
endif()

# ---- Platform detection & sources --------------------------------------------
set(LIGHTUI_PLATFORM_SOURCES "")
set(LIGHTUI_PLATFORM_DEFINES "")
set(LIGHTUI_PLATFORM_LIBS    "")

if(EMSCRIPTEN)
    list(APPEND LIGHTUI_PLATFORM_SOURCES
        src/platform/wasm/platform_wasm.c)
    list(APPEND LIGHTUI_PLATFORM_DEFINES LUI_PLATFORM_WASM)

elseif(WIN32)
    list(APPEND LIGHTUI_PLATFORM_SOURCES
        src/platform/win32/platform_win32.c)
    list(APPEND LIGHTUI_PLATFORM_DEFINES LUI_PLATFORM_WIN32)

elseif(APPLE)
    enable_language(OBJC)
    list(APPEND LIGHTUI_PLATFORM_SOURCES
        src/platform/cocoa/platform_cocoa.m)
    list(APPEND LIGHTUI_PLATFORM_DEFINES LUI_PLATFORM_COCOA)
    list(APPEND LIGHTUI_PLATFORM_LIBS
        "-framework Cocoa"
        "-framework CoreGraphics")

else()
    # Linux / BSD — prefer Wayland if available, fall back to X11
    option(LUI_PREFER_WAYLAND "Prefer Wayland over X11 when both are present" OFF)

    find_package(PkgConfig QUIET)

    set(_WAYLAND_FOUND FALSE)

    if(PKG_CONFIG_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
        pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
        if(WAYLAND_CLIENT_FOUND)
            set(_WAYLAND_FOUND TRUE)
        endif()
    endif()

    if(LUI_PREFER_WAYLAND AND _WAYLAND_FOUND)
        set(_USE_WAYLAND TRUE)
    else()
        set(_USE_WAYLAND FALSE)
    endif()

    if(_USE_WAYLAND)
        # Generate xdg-shell protocol glue
        find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
        pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

        set(XDG_SHELL_XML
            "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")

        set(XDG_SHELL_HEADER
            "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
        set(XDG_SHELL_SOURCE
            "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.c")

        add_custom_command(
            OUTPUT "${XDG_SHELL_HEADER}"
            COMMAND "${WAYLAND_SCANNER}" client-header
                    "${XDG_SHELL_XML}" "${XDG_SHELL_HEADER}"
            DEPENDS "${XDG_SHELL_XML}"
            COMMENT "Generating xdg-shell client header"
        )
        add_custom_command(
            OUTPUT "${XDG_SHELL_SOURCE}"
            COMMAND "${WAYLAND_SCANNER}" private-code
                    "${XDG_SHELL_XML}" "${XDG_SHELL_SOURCE}"
            DEPENDS "${XDG_SHELL_XML}"
            COMMENT "Generating xdg-shell client source"
        )

        list(APPEND LIGHTUI_PLATFORM_SOURCES
            src/platform/wayland/platform_wayland.c
            "${XDG_SHELL_SOURCE}")
        list(APPEND LIGHTUI_PLATFORM_DEFINES
            LUI_PLATFORM_WAYLAND)
        list(APPEND LIGHTUI_PLATFORM_LIBS
            ${WAYLAND_CLIENT_LIBRARIES})
        set(LIGHTUI_WAYLAND_INC "${CMAKE_CURRENT_BINARY_DIR}")
    else()
        list(APPEND LIGHTUI_PLATFORM_SOURCES
            src/platform/x11/platform_x11.c)
        list(APPEND LIGHTUI_PLATFORM_DEFINES
            LUI_PLATFORM_X11)
        list(APPEND LIGHTUI_PLATFORM_LIBS
            ${CMAKE_DL_LIBS})
    endif()
endif()

# ---- LightVG library target --------------------------------------------------
set(LIGHTVG_SOURCES
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    lightvg/src/dirty.c
    lightvg/src/layer.c
    lightvg/src/scene.c
)

add_library(lightvg STATIC ${LIGHTVG_SOURCES})
target_include_directories(lightvg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lightvg/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lightvg/src
)

# ---- Library target ----------------------------------------------------------
add_library(lightui STATIC
    ${LIGHTUI_CORE_SOURCES}
    ${LIGHTUI_PLATFORM_SOURCES}
)

target_include_directories(lightui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lightvg/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lighttype/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp
        ${CMAKE_CURRENT_SOURCE_DIR}/lighttype/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lighttype/src
        $<$<BOOL:${LIGHTUI_WAYLAND_INC}>:${LIGHTUI_WAYLAND_INC}>
)

# Optional SIMD for the VG software rasterizer (default off -> portable
# scalar). Pick at most one.
option(LVG_USE_SSE2 "Enable SSE2 fast paths in the VG software rasterizer (x86-64 / x86 with -msse2)" OFF)
option(LVG_USE_NEON "Enable NEON fast paths in the VG software rasterizer (ARM/AArch64)"              OFF)
if(LVG_USE_SSE2 AND LVG_USE_NEON)
    message(FATAL_ERROR "LVG_USE_SSE2 and LVG_USE_NEON are mutually exclusive")
endif()
set(LVG_SIMD_DEFS "")
if(LVG_USE_SSE2)
    list(APPEND LVG_SIMD_DEFS LVG_USE_SSE2=1)
endif()
if(LVG_USE_NEON)
    list(APPEND LVG_SIMD_DEFS LVG_USE_NEON=1)
endif()

target_compile_definitions(lightvg PRIVATE ${LVG_SIMD_DEFS})

target_compile_definitions(lightui
    PRIVATE
        ${LIGHTUI_PLATFORM_DEFINES}
        LUI_HAVE_FONTS
)

target_link_libraries(lightui
    PRIVATE
        ${LIGHTUI_PLATFORM_LIBS}
    PUBLIC
        lightvg
)

if(UNIX AND NOT APPLE)
    target_link_libraries(lightvg PUBLIC m)
    target_link_libraries(lightui PRIVATE m)
    set(LIGHTUI_MATH_LIBS m)
else()
    set(LIGHTUI_MATH_LIBS "")
endif()

# clock_gettime is in librt on glibc < 2.17 (Linux only)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(lightui PRIVATE rt)
endif()

# Winsock2 for MCP HTTP transport
if(WIN32 AND LUI_BUILD_MCP)
    target_link_libraries(lightui PRIVATE ws2_32)
endif()

# pthreads for parallel strip rasterization in the AGG fill path.
if(NOT WIN32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads QUIET)
    if(Threads_FOUND)
        target_link_libraries(lightvg PUBLIC Threads::Threads)
        target_compile_definitions(lightvg PRIVATE LVG_HAVE_PTHREADS=1)
    endif()
endif()

# ---- FreeType/HarfBuzz from ref/ ---------------------------------------------
if(LUI_BUILD_FREETYPE)
    enable_language(CXX)

    # Build FreeType from ref/freetype (disable optional deps for a lean build)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL    ON CACHE BOOL "" FORCE)
    add_subdirectory(ref/freetype EXCLUDE_FROM_ALL)

    # Build HarfBuzz from ref/harfbuzz (auto-detects freetype target)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_UTILS  OFF CACHE BOOL "" FORCE)
    add_subdirectory(ref/harfbuzz EXCLUDE_FROM_ALL)

    target_sources(lightui PRIVATE lighttype/src/font_freetype.c)
    target_compile_definitions(lightui PRIVATE LUI_HAVE_FREETYPE)
    target_link_libraries(lightui PUBLIC freetype harfbuzz)
endif()

# ---- Example -----------------------------------------------------------------
add_executable(hello_lightui
    examples/hello/main.c
)
target_link_libraries(hello_lightui PRIVATE lightui)

# ---- Widget demo example -----------------------------------------------------
set(WIDGET_DEMO_HACK_FONT "${CMAKE_CURRENT_SOURCE_DIR}/fonts/hack/Hack-Regular.ttf")
set(WIDGET_DEMO_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/widget-demo-embedded")
set(WIDGET_DEMO_EMBED_HEADER "${WIDGET_DEMO_EMBED_DIR}/widget_demo_hack_font.h")
set(WIDGET_DEMO_EMBED_C "${WIDGET_DEMO_EMBED_DIR}/widget_demo_hack_font.c")
set(WIDGET_DEMO_EMBED_S "${WIDGET_DEMO_EMBED_DIR}/widget_demo_hack_font.S")

if(WIN32 OR EMSCRIPTEN)
    set(WIDGET_DEMO_EMBED_MODE array)
    set(WIDGET_DEMO_EMBED_OUTPUTS
        "${WIDGET_DEMO_EMBED_HEADER}"
        "${WIDGET_DEMO_EMBED_C}"
    )
else()
    set(WIDGET_DEMO_EMBED_MODE incbin)
    set(WIDGET_DEMO_EMBED_OUTPUTS
        "${WIDGET_DEMO_EMBED_HEADER}"
        "${WIDGET_DEMO_EMBED_C}"
        "${WIDGET_DEMO_EMBED_S}"
    )
    enable_language(ASM)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(
    OUTPUT ${WIDGET_DEMO_EMBED_OUTPUTS}
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_binary_resource.py"
        --input "${WIDGET_DEMO_HACK_FONT}"
        --out-dir "${WIDGET_DEMO_EMBED_DIR}"
        --base widget_demo_hack_font
        --api widget_demo_hack_font
        --mode "${WIDGET_DEMO_EMBED_MODE}"
    DEPENDS
        "${WIDGET_DEMO_HACK_FONT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_binary_resource.py"
    COMMENT "Embedding Hack-Regular.ttf for widget_demo"
    VERBATIM
)

add_executable(widget_demo
    examples/widget-demo/main.c
    ${WIDGET_DEMO_EMBED_OUTPUTS}
)
target_include_directories(widget_demo PRIVATE "${WIDGET_DEMO_EMBED_DIR}")
target_compile_definitions(widget_demo PRIVATE LUI_WIDGET_DEMO_EMBED_HACK_FONT)
target_link_libraries(widget_demo PRIVATE lightui)

# ---- Terminal demo example ----------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/terminal/main.c")
    add_executable(terminal_demo
        examples/terminal/main.c
    )
    target_link_libraries(terminal_demo PRIVATE lightui)
endif()

# ---- X11 server example (Linux/BSD only) -------------------------------------
if(UNIX AND NOT APPLE)
    add_executable(x11_server
        examples/x11-server/main.c
    )
    target_include_directories(x11_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp
    )
    target_link_libraries(x11_server PRIVATE lightui)
endif()

# ---- PTY terminal example (Linux/BSD/macOS) ----------------------------------
if(UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/xterminal/main.c")
    add_executable(xterminal
        examples/xterminal/main.c
    )
    target_link_libraries(xterminal PRIVATE lightui)
    if(NOT APPLE)
        target_link_libraries(xterminal PRIVATE util)
    endif()
endif()

# ---- Map viewer example ------------------------------------------------------
add_executable(map_viewer
    examples/map-viewer/main.c
)
target_link_libraries(map_viewer PRIVATE lightui)

# ---- Image retouch example ---------------------------------------------------
add_executable(image_retouch
    examples/image-retouch/main.c
)
target_link_libraries(image_retouch PRIVATE lightui)

# ---- NLA editor example ------------------------------------------------------
add_executable(nla_editor
    examples/nla-editor/main.c
)
target_link_libraries(nla_editor PRIVATE lightui)

# ---- Image browser example ---------------------------------------------------
add_executable(image_browser
    examples/image-browser/main.c
)
target_link_libraries(image_browser PRIVATE lightui)

# ---- Chat demo example -------------------------------------------------------
add_executable(chat_demo
    examples/chat/main.c
)
target_link_libraries(chat_demo PRIVATE lightui)

# ---- Text CJK demo example --------------------------------------------------
add_executable(text_cjk_demo
    examples/text-cjk-demo/main.c
)
target_link_libraries(text_cjk_demo PRIVATE lightui)

# ---- Text Standard 14 demo example ------------------------------------------
add_executable(text_std14_demo
    examples/text-std14-demo/main.c
)
target_link_libraries(text_std14_demo PRIVATE lightui)

# ---- Text all demo example --------------------------------------------------
add_executable(text_all_demo
    examples/text-all-demo/main.c
)
target_link_libraries(text_all_demo PRIVATE lightui)

# ---- Text kerning demo example ----------------------------------------------
add_executable(text_kerning_demo
    examples/text-kerning-demo/main.c
)
target_include_directories(text_kerning_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lighttype/src
    ${CMAKE_CURRENT_SOURCE_DIR}/lighttype/include
)
target_link_libraries(text_kerning_demo PRIVATE lightui)

# ---- Text-render example (standalone, uses font directly) --------------------
add_executable(text_render
    examples/text-render/main.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/frame_clock.c
    src/font_bridge.c
    lighttype/src/ttf_parse.c
    lighttype/src/rasterize.c
    lighttype/src/font.c
)
target_include_directories(text_render PRIVATE include src lighttype/include lighttype/src)
target_compile_definitions(text_render PRIVATE LUI_HAVE_FONTS)
target_compile_options(text_render PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
target_link_libraries(text_render PRIVATE ${LIGHTUI_MATH_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(text_render PRIVATE rt)
endif()

# Windowed variant — uses the platform backend via the lightui library
add_executable(text_render_window
    examples/text-render/main_window.c
)
target_include_directories(text_render_window PRIVATE include src lighttype/include lighttype/src)
target_compile_definitions(text_render_window PRIVATE LUI_HAVE_FONTS)
target_compile_options(text_render_window PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
target_link_libraries(text_render_window PRIVATE lightui)

# ---- Pluggable canvas backends (optional) ------------------------------------
#
# When LVG_ENABLE_BLEND2D=ON, build lightvg/src/backends/blend2d/blend2d_backend.cpp
# as a small static library, and propagate -DLVG_HAVE_BLEND2D to lightvg core
# so that canvas.c picks up the lvg_canvas_blend2d_init() extern and the
# BLEND2D arm of lvg_canvas_init_backend().
#
# The drawing_demo targets below link against the backend lib so they can
# switch backends at runtime (press 'b' to cycle, or --backend blend2d).
option(LVG_ENABLE_BLEND2D "Enable pluggable blend2d canvas backend (requires ref/blend2d)" OFF)

if(LVG_ENABLE_BLEND2D)
    set(BLEND2D_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ref/blend2d")
    if(NOT EXISTS "${BLEND2D_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "LVG_ENABLE_BLEND2D=ON but blend2d source not found at ${BLEND2D_DIR}. "
            "Run: bash scripts/fetch-verify-deps.sh")
    endif()
    enable_language(CXX)
    set(BLEND2D_STATIC TRUE CACHE BOOL "Build blend2d as static library" FORCE)
    if(NOT TARGET blend2d::blend2d)
        add_subdirectory("${BLEND2D_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/blend2d" EXCLUDE_FROM_ALL)
    endif()

    add_library(lightvg_backend_blend2d STATIC
        lightvg/src/backends/blend2d/blend2d_backend.cpp
    )
    target_include_directories(lightvg_backend_blend2d PRIVATE lightvg/include lightvg/src)
    target_link_libraries(lightvg_backend_blend2d PUBLIC blend2d::blend2d)
    target_compile_features(lightvg_backend_blend2d PRIVATE cxx_std_17)

    # Propagate define to lightvg core so canvas.c wires in the backend.
    target_compile_definitions(lightvg PUBLIC LVG_HAVE_BLEND2D)
    target_link_libraries(lightvg PUBLIC lightvg_backend_blend2d)
endif()

# LVG_ENABLE_THORVG: links lightvg/src/backends/thorvg/thorvg_backend.cpp against
# the pre-built thorvg static lib at ref/thorvg/build/src/libthorvg-1.a.
# Thorvg itself is a meson project; build it first with
#   meson setup ref/thorvg/build ref/thorvg && ninja -C ref/thorvg/build
# (or keep the checked-in prebuilt artefact).
option(LVG_ENABLE_THORVG "Enable pluggable thorvg canvas backend (requires prebuilt ref/thorvg/build/src/libthorvg-1.a)" OFF)

if(LVG_ENABLE_THORVG)
    set(THORVG_LIB "${CMAKE_CURRENT_SOURCE_DIR}/ref/thorvg/build/src/libthorvg-1.a")
    set(THORVG_INC "${CMAKE_CURRENT_SOURCE_DIR}/ref/thorvg/inc")
    if(NOT EXISTS "${THORVG_LIB}")
        message(FATAL_ERROR
            "LVG_ENABLE_THORVG=ON but prebuilt thorvg not found at ${THORVG_LIB}. "
            "Build it first with:\n"
            "  meson setup ${CMAKE_CURRENT_SOURCE_DIR}/ref/thorvg/build ${CMAKE_CURRENT_SOURCE_DIR}/ref/thorvg\n"
            "  ninja -C ${CMAKE_CURRENT_SOURCE_DIR}/ref/thorvg/build")
    endif()
    enable_language(CXX)

    add_library(lightvg_backend_thorvg STATIC
        lightvg/src/backends/thorvg/thorvg_backend.cpp
    )
    target_include_directories(lightvg_backend_thorvg PRIVATE lightvg/include lightvg/src "${THORVG_INC}")
    target_compile_definitions(lightvg_backend_thorvg PRIVATE TVG_STATIC)
    target_link_libraries(lightvg_backend_thorvg PUBLIC "${THORVG_LIB}")
    target_compile_features(lightvg_backend_thorvg PRIVATE cxx_std_17)

    target_compile_definitions(lightvg PUBLIC LVG_HAVE_THORVG)
    target_link_libraries(lightvg PUBLIC lightvg_backend_thorvg)
endif()

# ---- Drawing demo (all-in-one, windowed + offline) ---------------------------

# Optional SIMD for the demo's box-downsample (default off → portable scalar).
# Pick at most one.
option(LUI_DEMO_USE_SSE2 "Enable SSE2 fast path in drawing_demo downsample (x86-64 / x86 with -msse2)" OFF)
option(LUI_DEMO_USE_NEON "Enable NEON fast path in drawing_demo downsample (ARM/AArch64)" OFF)
if(LUI_DEMO_USE_SSE2 AND LUI_DEMO_USE_NEON)
    message(FATAL_ERROR "LUI_DEMO_USE_SSE2 and LUI_DEMO_USE_NEON are mutually exclusive")
endif()
set(LUI_DEMO_SIMD_DEFS "")
if(LUI_DEMO_USE_SSE2)
    list(APPEND LUI_DEMO_SIMD_DEFS LUI_DEMO_USE_SSE2=1)
endif()
if(LUI_DEMO_USE_NEON)
    list(APPEND LUI_DEMO_SIMD_DEFS LUI_DEMO_USE_NEON=1)
endif()

# Windowed variant — links against lightui for window/event/font
add_executable(drawing_demo
    examples/drawing-demo/main.c
)
target_include_directories(drawing_demo PRIVATE include src
    ${CMAKE_CURRENT_SOURCE_DIR}/lighttype/include)
target_compile_definitions(drawing_demo PRIVATE LUI_HAVE_FONTS ${LUI_DEMO_SIMD_DEFS})
target_compile_options(drawing_demo PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
target_link_libraries(drawing_demo PRIVATE lightui)
if(LVG_ENABLE_BLEND2D)
    target_link_libraries(drawing_demo PRIVATE lightvg_backend_blend2d)
endif()
if(LVG_ENABLE_THORVG)
    target_link_libraries(drawing_demo PRIVATE lightvg_backend_thorvg)
endif()

# Headless offline variant — no platform backend, no fonts
add_executable(drawing_demo_offline
    examples/drawing-demo/main.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/internal/lui_log.c
    src/frame_clock.c
    src/font_bridge.c
    lighttype/src/ttf_parse.c
    lighttype/src/rasterize.c
    lighttype/src/font.c
)
target_include_directories(drawing_demo_offline PRIVATE
    include
    src
    lighttype/include
    lighttype/src
)
target_compile_definitions(drawing_demo_offline PRIVATE
    DRAWING_DEMO_HEADLESS_ONLY
    LUI_HAVE_FONTS
    ${LUI_DEMO_SIMD_DEFS}
    ${LVG_SIMD_DEFS}
)
target_compile_options(drawing_demo_offline PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
target_link_libraries(drawing_demo_offline PRIVATE ${LIGHTUI_MATH_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(drawing_demo_offline PRIVATE rt)
endif()
if(LVG_ENABLE_BLEND2D)
    target_compile_definitions(drawing_demo_offline PRIVATE LVG_HAVE_BLEND2D)
    target_link_libraries(drawing_demo_offline PRIVATE lightvg_backend_blend2d)
endif()
if(LVG_ENABLE_THORVG)
    target_compile_definitions(drawing_demo_offline PRIVATE LVG_HAVE_THORVG)
    target_link_libraries(drawing_demo_offline PRIVATE lightvg_backend_thorvg)
endif()

# ---- Shape & stroke demo (standalone, headless) -------------------------------
add_executable(shape_stroke_demo
    examples/shape-and-stroke-demo/main.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
)
target_include_directories(shape_stroke_demo PRIVATE include src)
target_compile_options(shape_stroke_demo PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
target_link_libraries(shape_stroke_demo PRIVATE ${LIGHTUI_MATH_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(shape_stroke_demo PRIVATE rt)
endif()

# Advanced primitives demo (standalone, headless)
add_executable(shape_stroke_advanced
    examples/shape-and-stroke-demo/main_advanced.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
)
target_include_directories(shape_stroke_advanced PRIVATE include src)
target_compile_options(shape_stroke_advanced PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
target_link_libraries(shape_stroke_advanced PRIVATE ${LIGHTUI_MATH_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(shape_stroke_advanced PRIVATE rt)
endif()

# Windowed variant — uses the platform backend via the lightui library
add_executable(shape_stroke_window
    examples/shape-and-stroke-demo/main_window.c
)
target_include_directories(shape_stroke_window PRIVATE include src)
target_compile_definitions(shape_stroke_window PRIVATE LUI_HAVE_FONTS)
target_compile_options(shape_stroke_window PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
target_link_libraries(shape_stroke_window PRIVATE lightui)

# ---- SVG-tiny parser (subset, opt-in) -----------------------------------------
# Compact subset of SVG sufficient for the Ghostscript tiger test asset.
# OFF by default so the no-deps build path is unchanged. The tiger demo
# below force-flips it ON via a CACHE FORCE write.
option(LUI_BUILD_SVG_TINY
       "Build the minimal SVG-subset parser (tiger.svg subset)" OFF)

# ---- Tiger demo (windowed + offline, requires SVG-tiny) -----------------------
option(LUI_BUILD_TIGER_DEMO
       "Build the SVG tiger demo (forces LUI_BUILD_SVG_TINY=ON)" ON)
if(LUI_BUILD_TIGER_DEMO)
    set(LUI_BUILD_SVG_TINY ON CACHE BOOL "" FORCE)
endif()

if(LUI_BUILD_SVG_TINY)
    add_library(lightui_svg_tiny STATIC src/svg/svg_tiny.c)
    target_include_directories(lightui_svg_tiny
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(lightui_svg_tiny PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    target_link_libraries(lightui_svg_tiny PRIVATE lightui)
    if(UNIX AND NOT APPLE)
        target_link_libraries(lightui_svg_tiny PRIVATE m)
    endif()
endif()

if(LUI_BUILD_TIGER_DEMO)
    add_executable(tiger_demo examples/tiger-demo/main.c)
    target_include_directories(tiger_demo PRIVATE include src)
    target_compile_options(tiger_demo PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
    target_link_libraries(tiger_demo PRIVATE lightui lightui_svg_tiny)
    # Mirror drawing_demo: when backends are compiled in, link them
    # explicitly (the PUBLIC propagation from `lightui` already pulls them
    # in, but being explicit makes the dependency visible in this target
    # and matches the pattern used by drawing_demo / shape demos).
    if(LVG_ENABLE_BLEND2D)
        target_link_libraries(tiger_demo PRIVATE lightvg_backend_blend2d)
    endif()
    if(LVG_ENABLE_THORVG)
        target_link_libraries(tiger_demo PRIVATE lightvg_backend_thorvg)
    endif()
    if(UNIX AND NOT APPLE)
        target_link_libraries(tiger_demo PRIVATE m)
    endif()
endif()

# ---- MVT (Mapbox Vector Tile) parser + 2.5D map demo -------------------------
option(LUI_BUILD_GEOMAP_TINY
       "Build the minimal MVT/PBF parser library (lightui_geomap_tiny)" OFF)

option(LUI_BUILD_GEOMAP_DEMO
       "Build the 2.5D vector-map demo (forces LUI_BUILD_GEOMAP_TINY=ON)" ON)
if(LUI_BUILD_GEOMAP_DEMO)
    set(LUI_BUILD_GEOMAP_TINY ON CACHE BOOL "" FORCE)
endif()

if(LUI_BUILD_GEOMAP_TINY)
    add_library(lightui_geomap_tiny STATIC src/geomap/geomap_tiny.c)
    target_include_directories(lightui_geomap_tiny
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(lightui_geomap_tiny PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
    target_link_libraries(lightui_geomap_tiny PRIVATE lightui)
    if(UNIX AND NOT APPLE)
        target_link_libraries(lightui_geomap_tiny PRIVATE m)
    endif()
endif()

if(LUI_BUILD_GEOMAP_DEMO)
    add_executable(geomap_demo examples/geomap-demo/main.c)
    target_include_directories(geomap_demo PRIVATE include src)
    target_compile_options(geomap_demo PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
    target_link_libraries(geomap_demo PRIVATE lightui lightui_geomap_tiny)
    if(UNIX AND NOT APPLE)
        target_link_libraries(geomap_demo PRIVATE m)
    endif()
endif()

# ---- Verification against reference renderers (blend2d, thorvg) ---------------
option(LUI_BUILD_VERIFY "Build verification tests against blend2d and thorvg" OFF)

if(LUI_BUILD_VERIFY)
    # ---- Image comparison tool (headless, no platform needed) ----
    add_executable(verify_compare
        tests/verify/verify_compare.c
        lightvg/src/surface.c
        lightvg/src/canvas.c
        lightvg/src/backend_registry.c
        src/image_cmp.c
        src/image.c
    )
    target_include_directories(verify_compare PRIVATE include src)
    target_compile_options(verify_compare PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    )
    target_link_libraries(verify_compare PRIVATE m)

    # ---- Advanced primitives verifier (standalone, no deps) ----
    add_executable(verify_advanced
        tests/verify/verify_advanced.c
    )
    target_compile_options(verify_advanced PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    )
    target_link_libraries(verify_advanced PRIVATE m)

    # ---- Per-region comparison tool (standalone PPM loader, no deps) ----
    add_executable(verify_region_cmp
        tests/verify/verify_region_cmp.c
    )
    target_include_directories(verify_region_cmp PRIVATE include src)
    target_compile_options(verify_region_cmp PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    )
    target_link_libraries(verify_region_cmp PRIVATE m)

    # ---- blend2d (has CMakeLists.txt, can be added directly) ----
    set(BLEND2D_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ref/blend2d")
    if(EXISTS "${BLEND2D_DIR}/CMakeLists.txt")
        # blend2d builds as C++; we need to enable CXX for it
        enable_language(CXX)
        set(BLEND2D_STATIC TRUE CACHE BOOL "Build blend2d as static library" FORCE)
        # Guard against the pluggable-backend block (above) already adding it.
        if(NOT TARGET blend2d::blend2d)
            add_subdirectory("${BLEND2D_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/blend2d" EXCLUDE_FROM_ALL)
        endif()

        add_executable(verify_blend2d tests/verify/verify_blend2d.c)
        target_link_libraries(verify_blend2d PRIVATE blend2d::blend2d)
        target_compile_options(verify_blend2d PRIVATE
            $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
        )
    else()
        message(WARNING "blend2d not found at ${BLEND2D_DIR}. "
                        "Run: bash scripts/fetch-verify-deps.sh")
    endif()

    # ---- Independent software renderer (replicates lightui algorithms) ----
    # This is a standalone C program that reimplements lightui's rasterization
    # algorithms from scratch, verifying correctness by producing identical output.
    # No external library dependency — pure C99.
    add_executable(verify_thorvg tests/verify/verify_thorvg.c)
    target_compile_options(verify_thorvg PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    )
    target_link_libraries(verify_thorvg PRIVATE m)
endif()

# ---- Tests (headless — no platform backend) ----------------------------------
enable_testing()

# The test binary links only surface.c + canvas.c (no platform needed)
add_executable(test_canvas
    tests/test_canvas.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
)
target_include_directories(test_canvas PRIVATE
    include
    src
)
target_compile_options(test_canvas PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_canvas PRIVATE m)
endif()
add_test(NAME canvas COMMAND test_canvas)

# Overflow / degenerate-input regression test.  Links the size-arithmetic
# guards in surface.c and mcp/lui_image_enc.c without pulling the full UI.
add_executable(test_overflow
    tests/test_overflow.c
    lightvg/src/surface.c
    src/mcp/lui_image_enc.c
)
target_include_directories(test_overflow PRIVATE
    include
    src
    src/mcp
)
target_compile_options(test_overflow PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_overflow PRIVATE m)
endif()
add_test(NAME overflow COMMAND test_overflow)

# Pluggable logger smoke test. Links only the logger TU — no platform.
add_executable(test_log
    tests/test_log.c
    src/internal/lui_log.c
)
target_include_directories(test_log PRIVATE
    include
    src
)
target_compile_options(test_log PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
add_test(NAME log COMMAND test_log)

# Custom backend registry test. Exercises lvg_canvas_register_custom_backend()
# and the dispatch path in lvg_canvas_init_backend().
add_executable(test_custom_backend
    tests/test_custom_backend.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
)
target_include_directories(test_custom_backend PRIVATE
    include
    src
)
target_compile_options(test_custom_backend PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_custom_backend PRIVATE m)
endif()
add_test(NAME custom_backend COMMAND test_custom_backend)

# SVG-tiny parser test. Only built when LUI_BUILD_SVG_TINY=ON because
# it links against the optional lightui_svg_tiny library.
if(LUI_BUILD_SVG_TINY)
    add_executable(test_svg_tiny
        tests/test_svg_tiny.c
        lightvg/src/surface.c
        lightvg/src/canvas.c
        lightvg/src/backend_registry.c
    )
    target_include_directories(test_svg_tiny PRIVATE include src)
    target_compile_options(test_svg_tiny PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
    target_link_libraries(test_svg_tiny PRIVATE lightui_svg_tiny lightui)
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_svg_tiny PRIVATE m)
    endif()
    add_test(NAME svg_tiny COMMAND test_svg_tiny)
endif()

# MVT-tiny parser test.
if(LUI_BUILD_GEOMAP_TINY)
    add_executable(test_geomap_tiny
        tests/test_geomap_tiny.c
        lightvg/src/surface.c
        lightvg/src/canvas.c
        lightvg/src/backend_registry.c
    )
    target_include_directories(test_geomap_tiny PRIVATE include src)
    target_compile_options(test_geomap_tiny PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
    target_link_libraries(test_geomap_tiny PRIVATE lightui_geomap_tiny lightui)
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_geomap_tiny PRIVATE m)
    endif()
    add_test(NAME geomap_tiny COMMAND test_geomap_tiny)
endif()

add_executable(test_button
    tests/test_button.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/button.c
)
target_include_directories(test_button PRIVATE
    include
    src
)
target_compile_options(test_button PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_button PRIVATE m)
endif()
add_test(NAME button COMMAND test_button)

add_executable(test_layout
    tests/test_layout.c
    src/layout.c
    src/widget_cache.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    lightvg/src/dirty.c
    src/frame_clock.c
)
target_include_directories(test_layout PRIVATE
    include
    src
)
target_compile_options(test_layout PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
add_test(NAME layout COMMAND test_layout)

add_executable(test_widgets
    tests/test_widgets.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/layout.c
    src/widget_cache.c
    lightvg/src/dirty.c
    src/frame_clock.c
    src/scroll.c
    src/slider.c
    src/checkbox.c
    src/colorwheel.c
    src/curves.c
    src/timeline.c
    src/nodegraph.c
    src/combo.c
    src/chat.c
    src/treeview.c
    src/table.c
    src/plot.c
    src/menu.c
    src/histogram.c
    src/toolbar.c
    src/progress.c
    src/splitter.c
    src/viewport.c
    src/console.c
    src/dopesheet.c
    src/tabs.c
    src/heatmap.c
    src/gradient.c
    src/tooltip.c
    src/minimap.c
    src/spinner.c
    src/toggle.c
    src/radio.c
    src/knob.c
    src/numentry.c
    src/dialog.c
    src/toast.c
    src/statusbar.c
    src/accordion.c
    src/breadcrumb.c
    src/palette.c
    src/draglist.c
    src/badge.c
    src/propgrid.c
    src/layerstack.c
    src/rangeslider.c
    src/gauge.c
    src/filebrowser.c
    src/taginput.c
    src/searchbar.c
    src/popover.c
    src/ruler.c
    src/stepper.c
    src/rating.c
    src/imagecrop.c
    src/waveform.c
    src/pagination.c
    src/card.c
    src/sparkline.c
    src/theme.c
)
target_include_directories(test_widgets PRIVATE
    include
    src
    lighttype/include
)
target_compile_options(test_widgets PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_widgets PRIVATE m)
endif()
add_test(NAME widgets COMMAND test_widgets)

add_executable(test_markdown
    tests/test_markdown.c
    src/markdown.c
    src/markdown_render.c
    src/markdown_widget.c
    src/text_layout.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/layout.c
    src/widget_cache.c
    src/frame_clock.c
    lightvg/src/dirty.c
)
target_include_directories(test_markdown PRIVATE include src lighttype/include)
target_compile_options(test_markdown PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_markdown PRIVATE m)
endif()
add_test(NAME markdown COMMAND test_markdown)

add_executable(test_html
    tests/test_html.c
    src/html.c
    src/html_render.c
    src/html_widget.c
    src/text_layout.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/layout.c
    src/widget_cache.c
    src/frame_clock.c
    lightvg/src/dirty.c
)
target_include_directories(test_html PRIVATE include src lighttype/include)
target_compile_options(test_html PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_html PRIVATE m)
endif()
add_test(NAME html COMMAND test_html)

add_executable(test_export
    tests/test_export.c
    src/export_record.c
    src/export_svg.c
    src/export_pdf.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
)
target_include_directories(test_export PRIVATE include src)
target_compile_options(test_export PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_export PRIVATE m)
endif()
add_test(NAME export COMMAND test_export)

add_executable(test_image
    tests/test_image.c
    src/image.c
    lightvg/src/surface.c
)
target_include_directories(test_image PRIVATE include src)
target_compile_options(test_image PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_image PRIVATE m)
endif()
add_test(NAME image COMMAND test_image)

# MCP / JSON tests (requires LUI_BUILD_MCP)
# NOTE: test_mcp is temporarily disabled — its source list is missing
# lui_recorder_*, lui_image_cmp, lui_export_svg_string and fails to link.
# Re-enable once the dependency list is fixed.
if(FALSE AND LUI_BUILD_MCP)
    add_executable(test_mcp
        tests/test_mcp.c
        src/mcp/lui_json.c
        src/mcp/lui_net.c
        src/mcp/lui_http.c
        src/mcp/lui_image_enc.c
        src/mcp/mcp_server.c
        src/mcp/mcp_tools.c
        lightvg/src/surface.c
        lightvg/src/canvas.c
        lightvg/src/backend_registry.c
        src/image_cmp.c
        src/export_record.c
        src/export_svg.c
        src/layout.c
        src/widget_cache.c
        src/frame_clock.c
        src/scroll.c
        src/slider.c
        src/checkbox.c
        src/colorwheel.c
        src/curves.c
        src/timeline.c
        src/nodegraph.c
        src/combo.c
        src/chat.c
        src/treeview.c
        src/table.c
        src/plot.c
        src/menu.c
        src/histogram.c
        src/toolbar.c
        src/progress.c
        src/splitter.c
        src/viewport.c
        src/console.c
        src/dopesheet.c
        src/tabs.c
        src/heatmap.c
        src/gradient.c
        src/tooltip.c
        src/minimap.c
        src/spinner.c
        src/toggle.c
        src/radio.c
        src/knob.c
        src/numentry.c
        src/dialog.c
        src/toast.c
        src/statusbar.c
        src/accordion.c
        src/breadcrumb.c
        src/palette.c
        src/draglist.c
        src/badge.c
        src/propgrid.c
        src/layerstack.c
        src/rangeslider.c
        src/gauge.c
        src/filebrowser.c
        src/taginput.c
        src/searchbar.c
        src/popover.c
        src/ruler.c
        src/stepper.c
        src/rating.c
        src/imagecrop.c
        src/waveform.c
        src/pagination.c
        src/card.c
        src/sparkline.c
        src/theme.c
    )
    target_include_directories(test_mcp PRIVATE
        include
        src
        src/mcp
        ${CMAKE_CURRENT_SOURCE_DIR}/lighttype/include
    )
    target_compile_options(test_mcp PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(test_mcp PRIVATE m)
    endif()
    if(WIN32)
        target_link_libraries(test_mcp PRIVATE ws2_32)
    endif()
    add_test(NAME mcp COMMAND test_mcp)
endif()

# Font rendering test
add_executable(test_font
    tests/test_font.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/frame_clock.c
    src/font_bridge.c
    lighttype/src/ttf_parse.c
    lighttype/src/rasterize.c
    lighttype/src/font.c
)
target_include_directories(test_font PRIVATE include src lighttype/include lighttype/src)
target_compile_definitions(test_font PRIVATE LUI_HAVE_FONTS)
target_compile_options(test_font PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_font PRIVATE m)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(test_font PRIVATE rt)
endif()
# test_font is not added to CTest by default (needs a font file at runtime)

# ---- Pixel-level render tests ------------------------------------------------
#
# test_render renders scenes to PPM/PGM and diffs against golden references.
# Run:
#   ./build/test_render tests/golden tests/output [/path/to/font.ttf]
# Then (optional visual diff via Node.js):
#   node tests/diff_images.mjs --golden tests/golden --actual tests/output/actual
#
set(TEST_RENDER_SOURCES
    tests/test_render.c
    tests/image_test.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    lightvg/src/dirty.c
    src/frame_clock.c
    src/font_bridge.c
    lightvg/src/layer.c
    lightvg/src/scene.c
    lighttype/src/ttf_parse.c
    lighttype/src/rasterize.c
    lighttype/src/font.c
    src/text_layout.c
)

add_executable(test_render ${TEST_RENDER_SOURCES})

target_include_directories(test_render PRIVATE include src tests lighttype/include lighttype/src)
target_compile_options(test_render PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
target_compile_definitions(test_render PRIVATE LUI_HAVE_FONTS)

if(LUI_BUILD_FREETYPE)
    target_sources(test_render PRIVATE lighttype/src/font_freetype.c src/image_cmp.c)
    target_compile_definitions(test_render PRIVATE LUI_HAVE_FREETYPE)
    target_link_libraries(test_render PRIVATE
        freetype
        harfbuzz
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(test_render PRIVATE m)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(test_render PRIVATE rt)
endif()

# Add to CTest with default golden/output dirs (no font tests)
add_test(NAME render
    COMMAND test_render
        "${CMAKE_SOURCE_DIR}/tests/golden"
        "${CMAKE_BINARY_DIR}/tests/output"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# ---- Image comparison test ----------------------------------------------------
add_executable(test_image_cmp
    tests/test_image_cmp.c
    src/image_cmp.c
    lightvg/src/surface.c
)
target_include_directories(test_image_cmp PRIVATE include src)
target_compile_options(test_image_cmp PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_image_cmp PRIVATE m)
endif()
add_test(NAME image_cmp COMMAND test_image_cmp)

# ---- Text editor test --------------------------------------------------------
add_executable(test_text_edit
    tests/test_text_edit.c
    src/text_edit.c
    src/text_layout.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/frame_clock.c
    src/font_bridge.c
    lightvg/src/dirty.c
    lighttype/src/ttf_parse.c
    lighttype/src/rasterize.c
    lighttype/src/font.c
)
target_include_directories(test_text_edit PRIVATE include src lighttype/include lighttype/src)
target_compile_definitions(test_text_edit PRIVATE LUI_HAVE_FONTS)
target_compile_options(test_text_edit PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
if(UNIX AND NOT APPLE)
    target_link_libraries(test_text_edit PRIVATE m)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(test_text_edit PRIVATE rt)
endif()
add_test(NAME text_edit COMMAND test_text_edit)

# ---- Backend comparison test (standalone, needs a font file) -----------------
set(TEST_BACKEND_CMP_SOURCES
    tests/test_backend_cmp.c
    lightvg/src/surface.c
    lightvg/src/canvas.c
    lightvg/src/backend_registry.c
    src/frame_clock.c
    src/font_bridge.c
    src/image_cmp.c
    lighttype/src/ttf_parse.c
    lighttype/src/rasterize.c
    lighttype/src/font.c
)
if(LUI_BUILD_FREETYPE)
    list(APPEND TEST_BACKEND_CMP_SOURCES lighttype/src/font_freetype.c)
endif()

add_executable(test_backend_cmp ${TEST_BACKEND_CMP_SOURCES})
target_include_directories(test_backend_cmp PRIVATE include src lighttype/include lighttype/src)
target_compile_definitions(test_backend_cmp PRIVATE LUI_HAVE_FONTS)
target_compile_options(test_backend_cmp PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)
if(LUI_BUILD_FREETYPE)
    target_compile_definitions(test_backend_cmp PRIVATE LUI_HAVE_FREETYPE)
    target_link_libraries(test_backend_cmp PRIVATE
        freetype
        harfbuzz
    )
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(test_backend_cmp PRIVATE m)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(test_backend_cmp PRIVATE rt)
endif()
# Not added to CTest (needs a font file at runtime)

# ---- Installation ------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS lightvg lightui
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY lightvg/include/lightvg
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/lightui
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
