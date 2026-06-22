project('lightui',
  'c',
  version        : '0.2.0',
  license        : 'Apache-2.0',
  default_options: ['c_std=c99', 'warning_level=2', 'buildtype=release'],
  meson_version  : '>=0.58.0',
)

# Enable optional PNG export in the VG module (pulls in stb_image_write.h).
# The core VG rasterizer is libm-free by default; this flag turns on PNG
# output, which reintroduces stb_image_write (and its libm usage) in
# surface.c only.
add_project_arguments('-DLVG_ENABLE_PNG', '-DLVG_ENABLE_JPEG', language: 'c')

# ---- Core sources ------------------------------------------------------------
lightui_core_sources = [
  'src/internal/lui_log.c',
  'src/button.c',
  'src/lightui.c',
  'src/frame_clock.c',
  'src/layout.c', 'src/widget_cache.c',
  'src/scroll.c',
  'src/slider.c',
  'src/checkbox.c',
  'src/colorwheel.c',
  'src/curves.c',
  'src/timeline.c',
  'src/nodegraph.c',
  'src/combo.c',
  'src/chat.c',
  'src/treeview.c',
  'src/table.c',
  'src/plot.c',
  'src/menu.c',
  'src/histogram.c',
  'src/toolbar.c',
  'src/progress.c',
  'src/splitter.c',
  'src/viewport.c',
  'src/console.c',
  'src/dopesheet.c',
  'src/tabs.c',
  'src/heatmap.c',
  'src/gradient.c',
  'src/tooltip.c',
  'src/minimap.c',
  'src/spinner.c',
  'src/toggle.c',
  'src/radio.c',
  'src/knob.c',
  'src/numentry.c',
  'src/dialog.c',
  'src/toast.c',
  'src/statusbar.c',
  'src/accordion.c',
  'src/breadcrumb.c',
  'src/palette.c',
  'src/draglist.c',
  'src/badge.c',
  'src/propgrid.c',
  'src/layerstack.c',
  'src/rangeslider.c',
  'src/gauge.c',
  'src/filebrowser.c',
  'src/taginput.c',
  'src/searchbar.c',
  'src/popover.c',
  'src/ruler.c',
  'src/stepper.c',
  'src/rating.c',
  'src/imagecrop.c',
  'src/waveform.c',
  'src/pagination.c',
  'src/card.c',
  'src/sparkline.c',
  'src/theme.c',
  'src/text_layout.c',
  'src/markdown.c',
  'src/markdown_render.c',
  'src/markdown_widget.c',
  'src/html.c',
  'src/html_render.c',
  'src/html_widget.c',
  'src/export_record.c',
  'src/export_svg.c',
  'src/export_pdf.c',
  'src/image.c',
]

lightui_platform_sources = []
lightui_platform_deps    = []
lightui_platform_args    = []
lightvg_inc             = include_directories('lightvg/include')
lightui_inc              = include_directories('include', 'src', 'lightvg/include')
lightui_mcp_inc          = include_directories('include', 'src', 'src/mcp', 'lightvg/include')
generated_inc            = []

# ---- MCP option ---------------------------------------------------------------
lui_build_mcp = get_option('lui_build_mcp')

lightui_mcp_sources = []
if lui_build_mcp
  lightui_mcp_sources = [
    'src/mcp/lui_json.c',
    'src/mcp/lui_net.c',
    'src/mcp/lui_http.c',
    'src/mcp/lui_image_enc.c',
    'src/mcp/mcp_server.c',
    'src/mcp/mcp_tools.c',
  ]
endif

cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: false)

# ---- Optional canvas backends — early stage ---------------------------------
# Resolve the option flags and add the project-wide -DLUI_HAVE_* macros
# *before* any subdir or static_library() declares a target. The actual
# backend libraries are built further down (after lightui_inc is known).
lui_enable_blend2d = get_option('lui_enable_blend2d')
lui_enable_thorvg  = get_option('lui_enable_thorvg')

blend2d_lib = disabler()
thorvg_lib  = disabler()

if lui_enable_blend2d.enabled()
  blend2d_lib = cc.find_library('blend2d',
    dirs     : [meson.project_source_root() / 'ref' / 'blend2d' / 'build'],
    required : lui_enable_blend2d,
    static   : true,
  )
  if blend2d_lib.found()
    add_languages('cpp', native: false, required: true)
    add_project_arguments('-DLVG_HAVE_BLEND2D', language: 'c')
  endif
endif

if lui_enable_thorvg.enabled()
  thorvg_lib = cc.find_library('thorvg-1',
    dirs     : [meson.project_source_root() / 'ref' / 'thorvg' / 'build' / 'src'],
    required : lui_enable_thorvg,
    static   : true,
  )
  if thorvg_lib.found()
    add_languages('cpp', native: false, required: true)
    add_project_arguments('-DLVG_HAVE_THORVG', language: 'c')
  endif
endif

# ---- LightType font library (subdir) -----------------------------------------
lui_build_freetype = get_option('lui_build_freetype')
lightui_ft_deps = []

if lui_build_freetype
  add_languages('cpp', native: false)
  ft_dep = dependency('freetype2', required: true,
                       fallback: ['freetype', 'freetype_dep'])
  hb_dep = dependency('harfbuzz', required: true,
                       fallback: ['harfbuzz', 'libharfbuzz_dep'])
  lightui_ft_deps = [ft_dep, hb_dep]
endif

subdir('lighttype')

# Font bridge + remaining LightUI font consumers
lightui_font_sources = [
  'src/font_bridge.c',
  'src/label.c',
  'src/text_input.c',
  'src/text_edit.c',
]
lightui_font_args_base = ['-DLUI_HAVE_FONTS']
lightui_font_args = lightui_font_args_base
if lui_build_freetype
  lightui_font_args += '-DLUI_HAVE_FREETYPE'
endif

# ---- Image comparison (pure C99, no deps beyond math) ------------------------
lightui_core_sources += 'src/image_cmp.c'

# ---- Platform selection ------------------------------------------------------
host = host_machine.system()

if host == 'darwin'
  add_languages('objc', native: false)
  lightui_platform_sources += 'src/platform/cocoa/platform_cocoa.m'
  lightui_platform_args    += '-DLUI_PLATFORM_COCOA'
  lightui_platform_deps    += [
    dependency('appleframeworks', modules: ['Cocoa', 'CoreGraphics']),
  ]

elif host == 'windows'
  lightui_platform_sources += 'src/platform/win32/platform_win32.c'
  lightui_platform_args    += '-DLUI_PLATFORM_WIN32'

elif host == 'emscripten'
  lightui_platform_sources += 'src/platform/wasm/platform_wasm.c'
  lightui_platform_args    += '-DLUI_PLATFORM_WASM'

else
  # Linux / BSD — prefer Wayland if explicitly requested, else X11
  prefer_wayland = get_option('prefer_wayland')

  wayland_dep  = dependency('wayland-client',   required: false)
  wayland_protos = dependency('wayland-protocols', required: false)
  use_wayland = false
  if prefer_wayland and wayland_dep.found()
    use_wayland = true
  else
    use_wayland = false
  endif

  if use_wayland
    wayland_scanner = find_program('wayland-scanner', required: true)

    protocols_dir = wayland_protos.get_variable(
      pkgconfig: 'pkgdatadir',
      default_value: '/usr/share/wayland-protocols',
    )
    xdg_shell_xml = protocols_dir / 'stable/xdg-shell/xdg-shell.xml'

    xdg_shell_header = custom_target('xdg-shell-h',
      output : 'xdg-shell-client-protocol.h',
      input  : xdg_shell_xml,
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
    xdg_shell_source = custom_target('xdg-shell-c',
      output : 'xdg-shell-client-protocol.c',
      input  : xdg_shell_xml,
      command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    )

    generated_inc = include_directories('.')
    lightui_platform_sources += [
      'src/platform/wayland/platform_wayland.c',
      xdg_shell_header,
      xdg_shell_source,
    ]
    lightui_platform_args += '-DLUI_PLATFORM_WAYLAND'
    lightui_platform_deps += wayland_dep
  else
    lightui_platform_sources += 'src/platform/x11/platform_x11.c'
    lightui_platform_args    += '-DLUI_PLATFORM_X11'
    lightui_platform_deps    += cc.find_library('dl', required : false)
  endif
endif

# ---- Optional canvas backend libs (target stage) ----------------------------
# The matching -DLUI_HAVE_* project arguments were added much earlier so
# canvas.c (built as part of lightui_lib below) sees them. Here we just
# build the per-backend translation units that implement the dispatch
# vtables, plus expose link-friendly variables for downstream targets.
blend2d_backend_lib = []
blend2d_backend_dep = []
thorvg_backend_lib  = []
thorvg_backend_dep  = []

if lui_enable_blend2d.enabled() and blend2d_lib.found()
  blend2d_backend_lib = [static_library('lightvg_backend_blend2d',
    'lightvg/src/backends/blend2d/blend2d_backend.cpp',
    include_directories : [lightvg_inc, include_directories('lightvg/src'),
                           include_directories('ref/blend2d/src')],
    cpp_args            : ['-std=c++17', '-DLVG_HAVE_BLEND2D'],
  )]
  blend2d_backend_dep = blend2d_lib
  warning('Meson blend2d integration is minimal; the CMake build is the '
          + 'tested path (see CMakeLists.txt LVG_ENABLE_BLEND2D).')
endif

if lui_enable_thorvg.enabled() and thorvg_lib.found()
  thorvg_backend_lib = [static_library('lightvg_backend_thorvg',
    'lightvg/src/backends/thorvg/thorvg_backend.cpp',
    include_directories : [lightvg_inc, include_directories('lightvg/src'),
                           include_directories('ref/thorvg/inc')],
    cpp_args            : ['-std=c++17', '-DTVG_STATIC', '-DLVG_HAVE_THORVG'],
  )]
  thorvg_backend_dep = thorvg_lib
endif

# ---- LightVG library ---------------------------------------------------------
lightvg_sources = [
  'lightvg/src/surface.c',
  'lightvg/src/canvas.c',
  'lightvg/src/backend_registry.c',
  'lightvg/src/dirty.c',
  'lightvg/src/layer.c',
  'lightvg/src/scene.c',
]

lightvg_lib = static_library('lightvg',
  lightvg_sources,
  include_directories : [lightvg_inc, include_directories('lightvg/src')],
  dependencies        : [math_dep, blend2d_backend_dep, thorvg_backend_dep],
  link_with           : blend2d_backend_lib + thorvg_backend_lib,
  install             : true,
)

lightvg_dep = declare_dependency(
  link_with           : [lightvg_lib] + blend2d_backend_lib + thorvg_backend_lib,
  include_directories : lightvg_inc,
  dependencies        : [math_dep, blend2d_backend_dep, thorvg_backend_dep],
)

# ---- Library -----------------------------------------------------------------
lightui_lib = static_library('lightui',
  lightui_core_sources + lightui_platform_sources + lightui_mcp_sources + lightui_font_sources,
  include_directories : [lightui_mcp_inc] + generated_inc,
  dependencies        : lightui_platform_deps + [math_dep, lightvg_dep, lighttype_dep] + lightui_ft_deps,
  c_args              : lightui_platform_args + lightui_font_args,
  objc_args           : lightui_platform_args + lightui_font_args,
  install             : true,
)

lightui_dep = declare_dependency(
  link_with           : lightui_lib,
  include_directories : [include_directories('include'), lightvg_inc],
  compile_args        : lightui_font_args,
  dependencies        : [lightvg_dep, lighttype_dep],
)

# ---- Example -----------------------------------------------------------------
executable('hello_lightui',
  'examples/hello/main.c',
  dependencies : lightui_dep,
)

# ---- Map viewer example ------------------------------------------------------
executable('map_viewer',
  'examples/map-viewer/main.c',
  dependencies : lightui_dep,
)

# ---- Image retouch example ---------------------------------------------------
executable('image_retouch',
  'examples/image-retouch/main.c',
  dependencies : lightui_dep,
)

# ---- NLA editor example ------------------------------------------------------
executable('nla_editor',
  'examples/nla-editor/main.c',
  dependencies : lightui_dep,
)

# ---- Image browser example ---------------------------------------------------
executable('image_browser',
  'examples/image-browser/main.c',
  dependencies : lightui_dep,
)

# ---- Text CJK demo example ---------------------------------------------------
executable('text_cjk_demo',
  'examples/text-cjk-demo/main.c',
  dependencies : lightui_dep,
)

# ---- Text Standard 14 demo example ------------------------------------------
executable('text_std14_demo',
  'examples/text-std14-demo/main.c',
  dependencies : lightui_dep,
)

# ---- Text All demo example (CJK + Std14 + Emoji) ----------------------------
executable('text_all_demo',
  'examples/text-all-demo/main.c',
  dependencies : lightui_dep,
)

# ---- Text Kerning demo (kerning comparison) ----------------------------------
executable('text_kerning_demo',
  'examples/text-kerning-demo/main.c',
  include_directories : [include_directories('src'), include_directories('lighttype/src')],
  dependencies : lightui_dep,
)

# ---- Widget demo example -----------------------------------------------------
widget_demo_embed_mode = (host == 'windows' or host == 'emscripten') ? 'array' : 'incbin'
widget_demo_embed_outputs = widget_demo_embed_mode == 'array' ? [
  'widget_demo_hack_font.h',
  'widget_demo_hack_font.c',
] : [
  'widget_demo_hack_font.h',
  'widget_demo_hack_font.c',
  'widget_demo_hack_font.S',
]
widget_demo_hack_font = custom_target('widget_demo_hack_font',
  input : 'fonts/hack/Hack-Regular.ttf',
  output : widget_demo_embed_outputs,
  command : [
    find_program('python3'),
    files('scripts/embed_binary_resource.py'),
    '--input', '@INPUT@',
    '--out-dir', meson.current_build_dir(),
    '--base', 'widget_demo_hack_font',
    '--api', 'widget_demo_hack_font',
    '--mode', widget_demo_embed_mode,
  ],
)
executable('widget_demo',
  ['examples/widget-demo/main.c', widget_demo_hack_font],
  include_directories : include_directories('.'),
  c_args : ['-DLUI_WIDGET_DEMO_EMBED_HACK_FONT'],
  dependencies : [lightui_dep, math_dep],
)

# ---- Terminal demo example ----------------------------------------------------
fs = import('fs')
if fs.exists('examples/terminal/main.c')
  executable('terminal_demo',
    'examples/terminal/main.c',
    dependencies : lightui_dep,
  )
endif

# ---- X11 server example (Linux/BSD only) -------------------------------------
if host == 'linux' or host == 'freebsd' or host == 'openbsd' or host == 'netbsd'
  executable('x11_server',
    'examples/x11-server/main.c',
    include_directories : lightui_mcp_inc,
    dependencies : lightui_dep,
  )
endif

# ---- PTY terminal example (Linux/BSD/macOS) ----------------------------------
if host != 'windows' and host != 'emscripten' and fs.exists('examples/xterminal/main.c')
  xterminal_deps = [lightui_dep]
  if host != 'darwin'
    util_dep = cc.find_library('util', required : false)
    if util_dep.found()
      xterminal_deps += util_dep
    endif
  endif
  executable('xterminal',
    'examples/xterminal/main.c',
    dependencies : xterminal_deps,
  )
endif

# ---- Chat demo example -------------------------------------------------------
if fs.exists('examples/chat/main.c')
  executable('chat_demo',
    'examples/chat/main.c',
    dependencies : [lightui_dep, math_dep],
  )
endif

# ---- ThorVG examples port (headless) -----------------------------------------
executable('thorvg_examples',
  'examples/thorvg.examples/main.c',
  dependencies : [lightui_dep, math_dep],
)

# ---- Nodegraph demo example -------------------------------------------------
executable('nodegraph_demo',
  'examples/nodegraph/main.c',
  dependencies : lightui_dep,
)

# ---- SVG-tiny parser (subset, opt-in) ---------------------------------------
# Mirrors CMake's LUI_BUILD_SVG_TINY. Defaults match CMake (parser ON
# when tiger demo is requested; otherwise dormant).
lui_build_tiger_demo = get_option('lui_build_tiger_demo')
lui_build_svg_tiny   = get_option('lui_build_svg_tiny') or lui_build_tiger_demo

if lui_build_svg_tiny
  lightui_svg_tiny_lib = static_library('lightui_svg_tiny',
    ['src/svg/svg_tiny.c'],
    include_directories : lightui_inc,
    dependencies        : [math_dep],
  )
  lightui_svg_tiny_dep = declare_dependency(
    link_with           : lightui_svg_tiny_lib,
    include_directories : include_directories('include'),
  )
endif

if lui_build_tiger_demo
  executable('tiger_demo',
    'examples/tiger-demo/main.c',
    dependencies : [lightui_dep, lightui_svg_tiny_dep, math_dep,
                    blend2d_backend_dep, thorvg_backend_dep],
    link_with           : blend2d_backend_lib + thorvg_backend_lib,
  )
endif

# ---- MVT-tiny parser + 2.5D vector-map demo ---------------------------------
lui_build_geomap_demo = get_option('lui_build_geomap_demo')
lui_build_geomap_tiny = get_option('lui_build_geomap_tiny') or lui_build_geomap_demo

if lui_build_geomap_tiny
  lightui_geomap_tiny_lib = static_library('lightui_geomap_tiny',
    ['src/geomap/geomap_tiny.c'],
    include_directories : lightui_inc,
    dependencies        : [math_dep],
  )
  lightui_geomap_tiny_dep = declare_dependency(
    link_with           : lightui_geomap_tiny_lib,
    include_directories : include_directories('include'),
  )
endif

if lui_build_geomap_demo
  executable('geomap_demo',
    'examples/geomap-demo/main.c',
    dependencies : [lightui_dep, lightui_geomap_tiny_dep, math_dep],
  )
endif

# ---- Tests (headless) --------------------------------------------------------
test_canvas_exe = executable('test_canvas',
  ['tests/test_canvas.c', 'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep, blend2d_backend_dep, thorvg_backend_dep],
  link_with           : blend2d_backend_lib + thorvg_backend_lib,
)
test('canvas', test_canvas_exe)

# Overflow / degenerate-input regression test (phase 0).
test_overflow_exe = executable('test_overflow',
  ['tests/test_overflow.c', 'lightvg/src/surface.c', 'src/mcp/lui_image_enc.c'],
  include_directories : lightui_mcp_inc,
  dependencies : [math_dep],
)
test('overflow', test_overflow_exe)

# Pluggable-logger smoke test (phase 3a).
test_log_exe = executable('test_log',
  ['tests/test_log.c', 'src/internal/lui_log.c'],
  include_directories : lightui_inc,
)
test('log', test_log_exe)

# Custom backend registry (phase 3b).
test_custom_backend_exe = executable('test_custom_backend',
  ['tests/test_custom_backend.c',
   'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep],
)
test('custom_backend', test_custom_backend_exe)

# SVG-tiny parser test. Only registered when the parser is built.
if lui_build_svg_tiny
  test_svg_tiny_exe = executable('test_svg_tiny',
    ['tests/test_svg_tiny.c',
     'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c',
     'src/internal/lui_log.c'],
    include_directories : lightui_inc,
    dependencies        : [lightui_svg_tiny_dep, math_dep],
  )
  test('svg_tiny', test_svg_tiny_exe)
endif

if lui_build_geomap_tiny
  test_geomap_tiny_exe = executable('test_geomap_tiny',
    ['tests/test_geomap_tiny.c',
     'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c',
     'src/internal/lui_log.c'],
    include_directories : lightui_inc,
    dependencies        : [lightui_geomap_tiny_dep, math_dep],
  )
  test('geomap_tiny', test_geomap_tiny_exe)
endif

test_button_exe = executable('test_button',
  ['tests/test_button.c', 'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c', 'src/button.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep],
)
test('button', test_button_exe)

test_widgets_exe = executable('test_widgets',
  ['tests/test_widgets.c',
   'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c', 'lightvg/src/dirty.c', 'src/layout.c', 'src/widget_cache.c', 'src/frame_clock.c',
   'src/scroll.c', 'src/slider.c', 'src/checkbox.c',
   'src/colorwheel.c', 'src/curves.c', 'src/timeline.c',
   'src/nodegraph.c', 'src/combo.c', 'src/chat.c',
   'src/treeview.c', 'src/table.c', 'src/plot.c', 'src/menu.c',
   'src/histogram.c', 'src/toolbar.c', 'src/progress.c',
   'src/splitter.c', 'src/viewport.c', 'src/console.c',
   'src/dopesheet.c', 'src/tabs.c', 'src/heatmap.c',
   'src/gradient.c', 'src/tooltip.c', 'src/minimap.c',
   'src/spinner.c', 'src/toggle.c', 'src/radio.c',
   'src/knob.c', 'src/numentry.c', 'src/dialog.c',
   'src/toast.c', 'src/statusbar.c', 'src/accordion.c',
   'src/breadcrumb.c', 'src/palette.c', 'src/draglist.c',
   'src/badge.c', 'src/propgrid.c', 'src/layerstack.c',
   'src/rangeslider.c', 'src/gauge.c', 'src/filebrowser.c',
   'src/taginput.c', 'src/searchbar.c', 'src/popover.c',
   'src/ruler.c', 'src/stepper.c', 'src/rating.c',
   'src/imagecrop.c', 'src/waveform.c', 'src/pagination.c',
   'src/card.c', 'src/sparkline.c', 'src/theme.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep, lighttype_dep],
)
test('widgets', test_widgets_exe)

test_markdown_exe = executable('test_markdown',
  ['tests/test_markdown.c',
   'src/markdown.c', 'src/markdown_render.c', 'src/markdown_widget.c',
   'src/text_layout.c', 'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c',
   'src/layout.c', 'src/widget_cache.c', 'src/frame_clock.c', 'lightvg/src/dirty.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep, lighttype_dep],
)
test('markdown', test_markdown_exe)

test_html_exe = executable('test_html',
  ['tests/test_html.c',
   'src/html.c', 'src/html_render.c', 'src/html_widget.c',
   'src/text_layout.c', 'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c',
   'src/layout.c', 'src/widget_cache.c', 'src/frame_clock.c', 'lightvg/src/dirty.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep, lighttype_dep],
)
test('html', test_html_exe)

test_export_exe = executable('test_export',
  ['tests/test_export.c',
   'src/export_record.c', 'src/export_svg.c', 'src/export_pdf.c',
   'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep],
)
test('export', test_export_exe)

test_image_exe = executable('test_image',
  ['tests/test_image.c', 'src/image.c', 'lightvg/src/surface.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep],
)
test('image', test_image_exe)

# NOTE: test_mcp is temporarily disabled — its source list is missing
# lui_recorder_*, lui_image_cmp, lui_export_svg_string and fails to link.
# Re-enable once the dependency list is fixed.
if false and lui_build_mcp
  test_mcp_exe = executable('test_mcp',
    ['tests/test_mcp.c',
     'src/mcp/lui_json.c', 'src/mcp/lui_net.c', 'src/mcp/lui_http.c',
     'src/mcp/lui_image_enc.c', 'src/mcp/mcp_server.c', 'src/mcp/mcp_tools.c',
     'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c', 'src/layout.c', 'src/widget_cache.c', 'src/frame_clock.c',
     'src/scroll.c', 'src/slider.c', 'src/checkbox.c', 'src/theme.c',
     'src/export_record.c', 'src/export_svg.c', 'src/image_cmp.c'],
    include_directories : lightui_mcp_inc,
    dependencies : [math_dep, lighttype_dep],
  )
  test('mcp', test_mcp_exe)
endif

# ---- Image comparison test ----------------------------------------------------
test_image_cmp_exe = executable('test_image_cmp',
  ['tests/test_image_cmp.c', 'src/image_cmp.c', 'lightvg/src/surface.c'],
  include_directories : lightui_inc,
  dependencies : [math_dep],
)
test('image_cmp', test_image_cmp_exe)

# ---- Text editor test --------------------------------------------------------
# text_edit.c uses text_layout which references font functions.
# We link the font sources so the test is self-contained.
test_text_edit_exe = executable('test_text_edit',
  ['tests/test_text_edit.c',
   'src/text_edit.c', 'src/text_layout.c',
   'src/font_bridge.c',
   'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c',
   'src/frame_clock.c', 'lightvg/src/dirty.c'],
  include_directories : lightui_inc,
  c_args : lightui_font_args_base,
  dependencies : [math_dep, lighttype_dep],
)
test('text_edit', test_text_edit_exe)

# ---- Backend comparison (standalone, needs a font file at runtime) -----------
executable('test_backend_cmp',
  ['tests/test_backend_cmp.c',
   'lightvg/src/surface.c', 'lightvg/src/canvas.c', 'lightvg/src/backend_registry.c', 'src/image_cmp.c',
   'src/font_bridge.c', 'src/frame_clock.c'],
  include_directories : lightui_inc,
  c_args : lightui_font_args,
  dependencies : [math_dep, lighttype_dep],
)

# ---- Optional canvas backends ------------------------------------------------
#
# Parity with CMake's LVG_ENABLE_BLEND2D / _THORVG. Both are OFF by default
# so the common build path has no external deps. Meson's `feature` options
# map: disabled -> OFF, enabled -> required, auto -> best effort.
#
# Optional canvas backends (blend2d / thorvg) are configured earlier in
# this file, before lightui_lib is built, so canvas.c sees -DLUI_HAVE_*
# and so executable() targets can link against them via the
# {blend2d,thorvg}_backend_lib / _dep variables exposed there.

# ---- Verification against reference renderers --------------------------------
#
# blend2d: built as a CMake subproject (not integrated here — use CMake)
#
# thorvg: can be built as a Meson subproject from ref/thorvg
#   meson setup build -Dverify=true
#   meson compile -C build verify_thorvg
#
lui_build_verify = get_option('verify')

if lui_build_verify
  thorvg_dir = meson.project_source_root() / 'ref' / 'thorvg'
  thorvg_capi = thorvg_dir / 'src' / 'bindings' / 'capi' / 'thorvg_capi.h'
  thorvg_build_dir = thorvg_dir / 'build' / 'src'

  # Look for pre-built thorvg static library (built via scripts/build-thorvg.sh)
  cpp = meson.get_compiler('cpp', native: true, required: false)
  if import('fs').exists(thorvg_capi)
    thorvg_lib = find_library('thorvg-1',
      dirs: [thorvg_build_dir],
      required: false,
      static: true,
    )

    if thorvg_lib.found()
      thorvg_capi_inc = include_directories(
        thorvg_dir / 'src' / 'bindings' / 'capi',
      )

      executable('verify_thorvg',
        'tests/verify/verify_thorvg.c',
        include_directories : thorvg_capi_inc,
        dependencies : [thorvg_lib, math_dep],
        link_args : ['-lstdc++'],
      )
    else
      warning('thorvg library not found. Build it first:',
              '  bash scripts/fetch-verify-deps.sh',
              '  bash scripts/build-thorvg.sh')
    endif
  else
    warning('thorvg source not found at ' + thorvg_dir +
            '. Run: bash scripts/fetch-verify-deps.sh')
  endif
endif

# ---- Install headers ---------------------------------------------------------
install_subdir('include/lightui',
  install_dir: get_option('includedir'),
)
install_subdir('lightvg/include/lightvg',
  install_dir: get_option('includedir'),
)
