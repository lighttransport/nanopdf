# lighttype/meson.build — LightType font library

lighttype_sources = [
  'src/ttf_parse.c',
  'src/rasterize.c',
  'src/font.c',
]

lighttype_args = []
lighttype_deps = [math_dep]

# Optional FreeType/HarfBuzz backend (controlled by parent's lui_build_freetype)
if lui_build_freetype
  lighttype_sources += 'src/font_freetype.c'
  lighttype_args += '-DLTT_HAVE_FREETYPE'
  lighttype_deps += [ft_dep, hb_dep]
endif

lighttype_inc = include_directories('include', 'src')

lighttype_lib = static_library('lighttype',
  lighttype_sources,
  include_directories : lighttype_inc,
  dependencies : lighttype_deps,
  c_args : lighttype_args,
)

lighttype_dep = declare_dependency(
  link_with : lighttype_lib,
  include_directories : include_directories('include'),
  compile_args : lighttype_args,
)
