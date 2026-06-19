-- xmake.lua — xmake build script for lightui
--
-- Minimum xmake version: 2.6.0
-- SPDX-License-Identifier: Apache-2.0

set_project("lightui")
set_version("0.2.0")
set_xmakever("2.6.0")

set_languages("c11")  -- c11 is the minimum MSVC supports; c99 causes -TP (C++ mode) on MSVC
set_warnings("allextra")  -- /W4 on MSVC, -Wall -Wextra on GCC/Clang

-- Enable optional PNG export in the VG module. Without this flag
-- lightvg/src/surface.c is libm-free (PPM only); with it, PNG output is
-- compiled in via the vendored stb_image_write.h.
add_defines("LVG_ENABLE_PNG", "LVG_ENABLE_JPEG")
add_includedirs("lightvg/include")
if is_plat("windows", "mingw") then
    add_cflags("/utf-8", "/wd4819", {tools = "cl"})  -- UTF-8 source/exec charset; suppress codepage warning
    add_defines("_CRT_SECURE_NO_WARNINGS")  -- allow standard C functions (snprintf, strncpy, etc.)
end

-- ---- Options -----------------------------------------------------------------
option("prefer_wayland",
    {description = "Prefer Wayland over X11 on Linux", default = false,
     showmenu = true})

option("lui_build_mcp",
    {description = "Build MCP server for LLM debugging", default = true,
     showmenu = true})

-- Pluggable canvas backends. Both OFF by default so the common build path
-- has no external deps; enable via `xmake f --lui_enable_blend2d=y`.
-- Actual integration stubs are not wired up here (the CMake build is the
-- tested path for those), but the flags propagate LVG_HAVE_BLEND2D /
-- LVG_HAVE_THORVG to the library for code-path parity.
option("lui_enable_blend2d",
    {description = "Enable the blend2d canvas backend (requires prebuilt ref/blend2d)",
     default = false, showmenu = true})

option("lui_enable_thorvg",
    {description = "Enable the thorvg canvas backend (requires prebuilt ref/thorvg/build)",
     default = false, showmenu = true})

option("lui_build_svg_tiny",
    {description = "Build the minimal SVG-subset parser library (lightui_svg_tiny)",
     default = true, showmenu = true})

option("lui_build_tiger_demo",
    {description = "Build the tiger SVG demo (forces lui_build_svg_tiny=y)",
     default = true, showmenu = true})

option("lui_build_geomap_tiny",
    {description = "Build the minimal MVT/PBF parser library (lightui_geomap_tiny)",
     default = true, showmenu = true})

option("lui_build_geomap_demo",
    {description = "Build the 2.5D vector-map demo (forces lui_build_geomap_tiny=y)",
     default = true, showmenu = true})

-- ---- LightVG library ---------------------------------------------------------
target("lightvg")
    set_kind("static")
    add_includedirs("lightvg/include", {public = true})
    add_includedirs("lightvg/src", {public = false})
    if not is_plat("windows", "mingw") then
        add_syslinks("m")
    end
    add_files(
        "lightvg/src/surface.c",
        "lightvg/src/canvas.c",
        "lightvg/src/backend_registry.c",
        "lightvg/src/dirty.c",
        "lightvg/src/layer.c",
        "lightvg/src/scene.c"
    )

-- ---- Library -----------------------------------------------------------------
target("lightui")
    set_kind("static")

    add_deps("lightvg")
    add_includedirs("include", {public = true})
    add_includedirs("lightvg/include", {public = true})
    add_includedirs("lighttype/include", {public = true})
    add_includedirs("src", "lighttype/src", {public = false})
    add_defines("LUI_HAVE_FONTS")
    if not is_plat("windows", "mingw") then
        add_syslinks("m")
    end

    -- LightType font library sources
    add_files(
        "lighttype/src/ttf_parse.c",
        "lighttype/src/rasterize.c",
        "lighttype/src/font.c"
    )

    add_files(
        "src/internal/lui_log.c",
        "src/button.c",
        "src/lightui.c",
        "src/frame_clock.c",
        "src/layout.c",
        "src/widget_cache.c",
        "src/scroll.c",
        "src/slider.c",
        "src/checkbox.c",
        "src/colorwheel.c",
        "src/curves.c",
        "src/timeline.c",
        "src/nodegraph.c",
        "src/combo.c",
        "src/chat.c",
        "src/treeview.c",
        "src/table.c",
        "src/plot.c",
        "src/menu.c",
        "src/histogram.c",
        "src/toolbar.c",
        "src/progress.c",
        "src/splitter.c",
        "src/viewport.c",
        "src/console.c",
        "src/dopesheet.c",
        "src/tabs.c",
        "src/heatmap.c",
        "src/gradient.c",
        "src/tooltip.c",
        "src/minimap.c",
        "src/spinner.c",
        "src/toggle.c",
        "src/radio.c",
        "src/knob.c",
        "src/numentry.c",
        "src/dialog.c",
        "src/toast.c",
        "src/statusbar.c",
        "src/accordion.c",
        "src/breadcrumb.c",
        "src/palette.c",
        "src/draglist.c",
        "src/badge.c",
        "src/propgrid.c",
        "src/layerstack.c",
        "src/rangeslider.c",
        "src/gauge.c",
        "src/filebrowser.c",
        "src/taginput.c",
        "src/searchbar.c",
        "src/popover.c",
        "src/ruler.c",
        "src/stepper.c",
        "src/rating.c",
        "src/imagecrop.c",
        "src/waveform.c",
        "src/pagination.c",
        "src/card.c",
        "src/sparkline.c",
        "src/theme.c",
        "src/text_layout.c",
        "src/markdown.c",
        "src/markdown_render.c",
        "src/markdown_widget.c",
        "src/html.c",
        "src/html_render.c",
        "src/html_widget.c",
        "src/export_record.c",
        "src/export_svg.c",
        "src/export_pdf.c",
        "src/image.c",
        "src/image_cmp.c",
        "src/font_bridge.c",
        "src/label.c",
        "src/text_input.c",
        "src/text_edit.c"
    )

    if has_config("lui_build_mcp") then
        add_includedirs("src/mcp", {public = false})
        add_files(
            "src/mcp/lui_json.c",
            "src/mcp/lui_net.c",
            "src/mcp/lui_http.c",
            "src/mcp/lui_image_enc.c",
            "src/mcp/mcp_server.c",
            "src/mcp/mcp_tools.c"
        )
        if is_plat("windows", "mingw") then
            add_syslinks("ws2_32")
            add_defines("_WINSOCK_DEPRECATED_NO_WARNINGS")
        end
    end

    -- macOS
    if is_plat("macosx") then
        add_languages("objc")
        add_files("src/platform/cocoa/platform_cocoa.m")
        add_defines("LUI_PLATFORM_COCOA")
        add_frameworks("Cocoa", "CoreGraphics")

    -- Windows
    elseif is_plat("windows", "mingw") then
        add_files("src/platform/win32/platform_win32.c")
        add_defines("LUI_PLATFORM_WIN32")
        add_syslinks("user32", "gdi32")

    -- WebAssembly
    elseif is_plat("wasm") then
        add_files("src/platform/wasm/platform_wasm.c")
        add_defines("LUI_PLATFORM_WASM")

    -- Linux / BSD
    else
        -- xmake does not support `find_package` at parse time in this form.
        -- Keep backend selection explicit and deterministic:
        -- default to X11, switch to Wayland when prefer_wayland is enabled.
        local use_wayland = has_config("prefer_wayland")

        if use_wayland then
            -- Generate xdg-shell protocol glue
            local scanner = find_program("wayland-scanner")
            if scanner then
                local xml = "/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml"
                local gen_dir = "$(buildir)/gen"
                add_includedirs(gen_dir)
                before_build(function(target)
                    os.mkdir(gen_dir)
                    local ok, err
                    ok, err = os.execv(scanner, {"client-header", xml,
                        gen_dir .. "/xdg-shell-client-protocol.h"})
                    if not ok then
                        raise("wayland-scanner failed (client-header): " .. tostring(err))
                    end
                    ok, err = os.execv(scanner, {"private-code", xml,
                        gen_dir .. "/xdg-shell-client-protocol.c"})
                    if not ok then
                        raise("wayland-scanner failed (private-code): " .. tostring(err))
                    end
                end)
                add_files(gen_dir .. "/xdg-shell-client-protocol.c")
            end
            add_files("src/platform/wayland/platform_wayland.c")
            add_defines("LUI_PLATFORM_WAYLAND")
            add_links("wayland-client")
        else
            add_files("src/platform/x11/platform_x11.c")
            add_defines("LUI_PLATFORM_X11")
            add_links("dl")
        end
    end

-- ---- Example -----------------------------------------------------------------
target("hello_lightui")
    set_kind("binary")
    add_deps("lightui")
    add_files("examples/hello/main.c")

-- ---- Tests (headless — no platform backend) ----------------------------------
target("test_canvas")
    set_kind("binary")
    set_default(false)
    add_includedirs("include", "src")
    if not is_plat("windows", "mingw") then
        add_syslinks("m")
    end
    add_files(
        "tests/test_canvas.c",
        "lightvg/src/surface.c",
        "lightvg/src/canvas.c",
        "lightvg/src/backend_registry.c"
    )
    after_build(function(target)
        os.exec(target:targetfile())
    end)

target("test_button")
    set_kind("binary")
    set_default(false)
    add_includedirs("include", "src")
    if not is_plat("windows", "mingw") then
        add_syslinks("m")
    end
    add_files(
        "tests/test_button.c",
        "lightvg/src/surface.c",
        "lightvg/src/canvas.c",
        "lightvg/src/backend_registry.c",
        "src/button.c"
    )
    after_build(function(target)
        os.exec(target:targetfile())
    end)

-- Overflow / degenerate-input regression test (phase 0).
target("test_overflow")
    set_kind("binary")
    set_default(false)
    add_includedirs("include", "src", "src/mcp")
    if not is_plat("windows", "mingw") then
        add_syslinks("m")
    end
    add_files(
        "tests/test_overflow.c",
        "lightvg/src/surface.c",
        "src/mcp/lui_image_enc.c"
    )
    after_build(function(target)
        os.exec(target:targetfile())
    end)

-- Pluggable-logger smoke test (phase 3a).
target("test_log")
    set_kind("binary")
    set_default(false)
    add_includedirs("include", "src")
    add_files(
        "tests/test_log.c",
        "src/internal/lui_log.c"
    )
    after_build(function(target)
        os.exec(target:targetfile())
    end)

-- Custom canvas backend registry (phase 3b).
target("test_custom_backend")
    set_kind("binary")
    set_default(false)
    add_includedirs("include", "src")
    if not is_plat("windows", "mingw") then
        add_syslinks("m")
    end
    add_files(
        "tests/test_custom_backend.c",
        "lightvg/src/surface.c",
        "lightvg/src/canvas.c",
        "lightvg/src/backend_registry.c"
    )
    after_build(function(target)
        os.exec(target:targetfile())
    end)

-- ---- SVG-tiny parser + tiger demo --------------------------------------------
-- Mirrors CMake's LUI_BUILD_SVG_TINY / LUI_BUILD_TIGER_DEMO. Building
-- the tiger demo implies building the parser.
if has_config("lui_build_svg_tiny") or has_config("lui_build_tiger_demo") then
    target("lightui_svg_tiny")
        set_kind("static")
        add_includedirs("include", {public = true})
        add_includedirs("src",     {public = false})
        if not is_plat("windows", "mingw") then
            add_syslinks("m")
        end
        add_files("src/svg/svg_tiny.c")
        add_deps("lightui")
end

if has_config("lui_build_tiger_demo") then
    target("tiger_demo")
        set_kind("binary")
        add_deps("lightui", "lightui_svg_tiny")
        add_files("examples/tiger-demo/main.c")
        if not is_plat("windows", "mingw") then
            add_syslinks("m")
        end

    -- SVG-tiny smoke test (ctest equivalent runs after build).
    target("test_svg_tiny")
        set_kind("binary")
        set_default(false)
        add_includedirs("include", "src")
        add_deps("lightui_svg_tiny")
        if not is_plat("windows", "mingw") then
            add_syslinks("m")
        end
        add_files(
            "tests/test_svg_tiny.c",
            "lightvg/src/surface.c",
            "lightvg/src/canvas.c",
            "lightvg/src/backend_registry.c",
            "src/internal/lui_log.c"
        )
        after_build(function(target)
            os.exec(target:targetfile())
        end)
end

-- ---- MVT-tiny parser + 2.5D vector-map demo ---------------------------------
if has_config("lui_build_geomap_tiny") or has_config("lui_build_geomap_demo") then
    target("lightui_geomap_tiny")
        set_kind("static")
        add_includedirs("include", {public = true})
        add_includedirs("src",     {public = false})
        if not is_plat("windows", "mingw") then
            add_syslinks("m")
        end
        add_files("src/geomap/geomap_tiny.c")
        add_deps("lightui")
end

if has_config("lui_build_geomap_demo") then
    target("geomap_demo")
        set_kind("binary")
        add_deps("lightui", "lightui_geomap_tiny")
        add_files("examples/geomap-demo/main.c")
        if not is_plat("windows", "mingw") then
            add_syslinks("m")
        end

    target("test_geomap_tiny")
        set_kind("binary")
        set_default(false)
        add_includedirs("include", "src")
        add_deps("lightui_geomap_tiny")
        if not is_plat("windows", "mingw") then
            add_syslinks("m")
        end
        add_files(
            "tests/test_geomap_tiny.c",
            "lightvg/src/surface.c",
            "lightvg/src/canvas.c",
            "lightvg/src/backend_registry.c",
            "src/internal/lui_log.c"
        )
        after_build(function(target)
            os.exec(target:targetfile())
        end)
end
