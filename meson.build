project('nanopdf', 'c', 'cpp',
  version: '1.0.0',
  license: 'Apache-2.0',
  meson_version: '>=1.1.0',
  default_options: [
    'cpp_std=c++17',
    'c_std=c11',
    'warning_level=2',
  ],
)

# ── Options ──────────────────────────────────────────────────────────

opt_stb_truetype   = get_option('use_stb_truetype')
opt_nanostl        = get_option('use_nanostl')
opt_thorvg         = get_option('use_thorvg')
opt_blend2d        = get_option('use_blend2d')
opt_libtiff        = get_option('use_libtiff')
opt_cpp_image_bridge = get_option('use_cpp_image_bridge')
opt_builtin_ccitt  = get_option('use_builtin_ccitt')
opt_build_tests    = get_option('build_tests')
opt_build_mcp      = get_option('build_mcp')
opt_log_level      = get_option('log_level')
cpp = meson.get_compiler('cpp')

# ── Core library sources ─────────────────────────────────────────────

nanopdf_sources = files(
  'src/nanopdf.cc',
  'src/jpx-decoder.cc',
  'src/type1-parser.cc',
  'src/cff-parser.cc',
  'src/canvas-exporter.cc',
  'src/crypto.cc',
  'src/pdf-security.cc',
  'src/pdf-attachments.cc',
  'src/color-transform.cc',
  'src/tiff-decoder.cc',
  'src/text-layout.cc',
  'src/table-extraction.cc',
  'src/form-manipulation.cc',
  'src/document-structure.cc',
  'src/pdf-writer.cc',
  'src/font-provider.cc',
  # Embedded third-party (stb, miniz)
  'src/third_party/stb_image_impl.cc',
  'src/third_party/stb_image_write_impl.cc',
  'src/third_party/stb_truetype_impl.cc',
  'src/third_party/tiny_dng_impl.cc',
  'src/third_party/miniz.c',
  # JBIG2 decoder
  'src/jbig2/JBig2_BitStream.cc',
  'src/jbig2/JBig2_Image.cc',
  'src/jbig2/JBig2_Segment.cc',
  'src/jbig2/JBig2_SymbolDict.cc',
  'src/jbig2/JBig2_PatternDict.cc',
  'src/jbig2/JBig2_HuffmanTable.cc',
  'src/jbig2/JBig2_ArithDecoder.cc',
  'src/jbig2/JBig2_ArithIntDecoder.cc',
  'src/jbig2/JBig2_GrdProc.cc',
  'src/jbig2/JBig2_GrrdProc.cc',
  'src/jbig2/JBig2_Context.cc',
  'src/jbig2/JBig2_SddProc.cc',
  'src/jbig2/JBig2_TrdProc.cc',
)

nanopdf_inc = include_directories('src', 'src/third_party')

nanopdf_compile_args = [
  '-DNANOPDF_LOG_LEVEL=' + opt_log_level.to_string(),
  '-DNANOPDF_USE_MINIZ',
]

if opt_stb_truetype
  nanopdf_compile_args += '-DNANOPDF_USE_STB_TRUETYPE'
endif

if opt_nanostl
  nanopdf_compile_args += '-DNANOPDF_USE_NANOSTL'
endif

if opt_builtin_ccitt
  nanopdf_compile_args += '-DNANOPDF_USE_BUILTIN_CCITT'
endif

nanopdf_deps = []

# ── Optional: ThorVG backend ─────────────────────────────────────────

if opt_thorvg
  thorvg_dep = dependency('thorvg', required: true)
  nanopdf_deps += thorvg_dep
  nanopdf_sources += files('src/thorvg-backend.cc')
  nanopdf_compile_args += '-DNANOPDF_USE_THORVG'
endif

# ── Optional: libtiff ────────────────────────────────────────────────

if opt_libtiff
  libtiff_dep = dependency('libtiff-4', required: false)
  if not libtiff_dep.found()
    libtiff_dep = dependency('libtiff', required: true)
  endif
  nanopdf_deps += libtiff_dep
  nanopdf_compile_args += '-DNANOPDF_USE_LIBTIFF'
endif

# ── Windows: M_PI ────────────────────────────────────────────────────

if host_machine.system() == 'windows'
  nanopdf_compile_args += '-D_USE_MATH_DEFINES'
endif

# ── Library target ───────────────────────────────────────────────────

nanopdf_lib = static_library('nanopdf',
  nanopdf_sources,
  include_directories: nanopdf_inc,
  dependencies: nanopdf_deps,
  c_args: nanopdf_compile_args,
  cpp_args: nanopdf_compile_args,
  install: true,
)

nanopdf_dep = declare_dependency(
  include_directories: nanopdf_inc,
  link_with: nanopdf_lib,
)

install_headers('src/nanopdf.hh')

nanopdf_c_inc = include_directories('src/c')
nanopdf_c_sources = files(
  'src/c/nanopdf_alloc.c',
  'src/c/nanopdf_context.c',
  'src/c/nanopdf_crypto.c',
  'src/c/nanopdf_image_decoder.c',
  'src/c/nanopdf_jbig2.c',
  'src/c/nanopdf_jpx.c',
  'src/c/nanopdf_object.c',
  'src/c/nanopdf_basic_document.c',
  'src/c/nanopdf_basic_layout.c',
  'src/c/nanopdf_c_api.c',
  'src/third_party/miniz.c',
)
nanopdf_c_bridge_cpp_args = []

if opt_cpp_image_bridge
  nanopdf_c_bridge_cpp_args = cpp.get_supported_arguments(
    '-Wno-unused-function',
    '-Wno-unused-parameter',
    '-Wno-unused-variable',
  )
  nanopdf_c_sources += files(
    'src/c/nanopdf_image_decoder_bridge.cc',
    'src/jpx-decoder.cc',
    'src/jbig2/JBig2_BitStream.cc',
    'src/jbig2/JBig2_Image.cc',
    'src/jbig2/JBig2_Segment.cc',
    'src/jbig2/JBig2_SymbolDict.cc',
    'src/jbig2/JBig2_PatternDict.cc',
    'src/jbig2/JBig2_HuffmanTable.cc',
    'src/jbig2/JBig2_ArithDecoder.cc',
    'src/jbig2/JBig2_ArithIntDecoder.cc',
    'src/jbig2/JBig2_GrdProc.cc',
    'src/jbig2/JBig2_GrrdProc.cc',
    'src/jbig2/JBig2_Context.cc',
    'src/jbig2/JBig2_SddProc.cc',
    'src/jbig2/JBig2_TrdProc.cc',
  )
endif

nanopdf_c_lib = static_library('nanopdf-c',
  nanopdf_c_sources,
  include_directories: nanopdf_c_inc,
  c_args: opt_cpp_image_bridge ? ['-DNANOPDF_C_USE_CPP_IMAGE_BRIDGE=1'] : [],
  cpp_args: opt_cpp_image_bridge ? ['-DNANOPDF_C_USE_CPP_IMAGE_BRIDGE=1'] + nanopdf_c_bridge_cpp_args : [],
  install: true,
)

nanopdf_c_dep = declare_dependency(
  include_directories: include_directories('src/c'),
  link_with: nanopdf_c_lib,
)

install_headers(
  'src/c/nanopdf_types.h',
  'src/c/nanopdf_c.h',
  'src/c/nanopdf_parse.h',
  'src/c/nanopdf_text.h',
  'src/c/nanopdf_forms.h',
  subdir: 'nanopdf-c',
)

# ── MCP server ───────────────────────────────────────────────────────

if opt_build_mcp
  mcp_inc = include_directories('src', 'src/mcp')

  nanopdf_mcp_lib = library('nanopdf_mcp',
    files(
      'src/mcp/mcp-json.cc',
      'src/mcp/mcp-protocol.cc',
      'src/mcp/mcp-transport.cc',
      'src/mcp/mcp-tools.cc',
    ),
    include_directories: mcp_inc,
    link_with: nanopdf_lib,
    install: true,
  )

  nanopdf_mcp_dep = declare_dependency(
    include_directories: mcp_inc,
    link_with: [nanopdf_lib, nanopdf_mcp_lib],
  )

  executable('nanopdf-mcp',
    files('src/mcp/mcp-server.cc'),
    include_directories: mcp_inc,
    link_with: [nanopdf_lib, nanopdf_mcp_lib],
    install: true,
  )
endif

# ── Tools ────────────────────────────────────────────────────────────

executable('nanopdf_benchmark',
  files('src/benchmark.cc'),
  include_directories: nanopdf_inc,
  link_with: nanopdf_lib,
)

executable('ccitt_compare',
  files('tools/ccitt_compare.cc'),
  include_directories: nanopdf_inc,
  link_with: nanopdf_lib,
)

executable('visual_compare',
  files('tools/visual_compare.cc'),
  include_directories: nanopdf_inc,
)

# ── Tests ────────────────────────────────────────────────────────────

if opt_build_tests

  nanotest_inc = include_directories('tests', 'tests/fixtures')

  test_fixtures_lib = static_library('test_fixtures',
    files('tests/fixtures/test_helpers.cc'),
    include_directories: [nanopdf_inc, nanotest_inc],
    link_with: nanopdf_lib,
    cpp_args: [
      '-DNANOPDF_TEST_DATA_DIR="' + meson.current_source_dir() / 'tests' / 'data' + '"',
    ],
  )

  # ── Unit test suite ──────────────────────────────────────────────

  unit_test_sources = files(
    'tests/unit/core/test_value.cc',
    'tests/unit/core/test_xref.cc',
    'tests/unit/core/test_pdf_parsing.cc',
    'tests/unit/annotations/test_annotation_types.cc',
    'tests/unit/annotations/test_freetext_annotation.cc',
    'tests/unit/annotations/test_link_annotation.cc',
    'tests/unit/annotations/test_markup_annotation.cc',
    'tests/unit/annotations/test_text_annotation.cc',
    'tests/unit/document/test_document_catalog.cc',
    'tests/unit/document/test_document_info.cc',
    'tests/unit/document/test_named_destinations.cc',
    'tests/unit/document/test_outline.cc',
    'tests/unit/document/test_page_labels.cc',
    'tests/unit/document/test_attachments.cc',
    'tests/unit/document/test_document_structure.cc',
    'tests/unit/document/test_pdfa_validation.cc',
    'tests/unit/filters/test_ascii85_decode.cc',
    'tests/unit/filters/test_asciihex_decode.cc',
    'tests/unit/filters/test_ccitt_decode.cc',
    'tests/unit/filters/test_dct_decode.cc',
    'tests/unit/filters/test_filter_chains.cc',
    'tests/unit/filters/test_flate_decode.cc',
    'tests/unit/filters/test_lzw_decode.cc',
    'tests/unit/filters/test_png_predictors.cc',
    'tests/unit/filters/test_runlength_decode.cc',
    'tests/unit/filters/test_tiff_decode.cc',
    'tests/unit/fonts/test_cmap.cc',
    'tests/unit/fonts/test_cff_wrapper.cc',
    'tests/unit/fonts/test_font_substitution.cc',
    'tests/unit/fonts/test_type0_font.cc',
    'tests/unit/fonts/test_type3_font.cc',
    'tests/unit/forms/test_button_field.cc',
    'tests/unit/forms/test_choice_field.cc',
    'tests/unit/forms/test_form_field_types.cc',
    'tests/unit/forms/test_form_manipulation.cc',
    'tests/unit/forms/test_text_field.cc',
    'tests/unit/security/test_aes.cc',
    'tests/unit/security/test_hash.cc',
    'tests/unit/security/test_rc4.cc',
    'tests/unit/text/test_text_extraction.cc',
    'tests/unit/text/test_text_state.cc',
    'tests/unit/text/test_text_layout.cc',
    'tests/unit/text/test_table_extraction.cc',
    'tests/unit/graphics/test_color_transform.cc',
    'tests/unit/test_suite_main.cc',
  )

  unit_test_args = [
    '-DNANOPDF_TEST_DATA_DIR="' + meson.current_source_dir() / 'tests' / 'data' + '"',
    '-DNANOPDF_TEST_SUITE_NO_MAIN=1',
  ]

  # Include MCP JSON test if MCP is built
  if opt_build_mcp
    unit_test_sources += files('examples/mcp/test-mcp-json.cc')
    unit_suite_link = [nanopdf_lib, nanopdf_mcp_lib, test_fixtures_lib]
  else
    unit_suite_link = [nanopdf_lib, test_fixtures_lib]
  endif

  nanopdf_unit_suite = executable('nanopdf_unit_suite',
    unit_test_sources,
    include_directories: [nanopdf_inc, nanotest_inc,
                          include_directories('.')],
    link_with: unit_suite_link,
    cpp_args: unit_test_args,
  )

  test('nanopdf_unit_suite', nanopdf_unit_suite)

  nanopdf_c_api_smoke = executable('nanopdf_c_api_smoke',
    files('tests/c/test_nanopdf_c.c'),
    include_directories: [include_directories('src/c'),
                          include_directories('tests')],
    c_args: opt_cpp_image_bridge ? ['-DNANOPDF_C_USE_CPP_IMAGE_BRIDGE=1'] : [],
    link_with: nanopdf_c_lib,
  )

  test('nanopdf_c_api_smoke', nanopdf_c_api_smoke)

  # ── Integration tests ────────────────────────────────────────────

  integration_args = [
    '-DNANOPDF_TEST_DATA_DIR="' + meson.current_source_dir() / 'tests' / 'data' + '"',
    '-DNANOPDF_PROJECT_DIR="' + meson.current_source_dir() + '"',
  ]

  test_parse_real = executable('test_parse_real_pdfs',
    files('tests/integration/test_parse_real_pdfs.cc'),
    include_directories: [nanopdf_inc, nanotest_inc],
    link_with: [nanopdf_lib, test_fixtures_lib],
    cpp_args: integration_args,
  )

  test_text_extraction = executable('test_text_extraction',
    files('tests/integration/test_text_extraction.cc'),
    include_directories: [nanopdf_inc, nanotest_inc],
    link_with: [nanopdf_lib, test_fixtures_lib],
    cpp_args: integration_args,
  )

  test('test_parse_real_pdfs', test_parse_real,
    timeout: 300,
  )

  test('test_text_extraction', test_text_extraction,
    timeout: 300,
  )

  # ── Validation tests ─────────────────────────────────────────────

  validation_inc = include_directories('tests/validation')

  nanopdf_validation_lib = static_library('nanopdf_validation',
    files(
      'tests/validation/arlington_parser.cc',
      'tests/validation/arlington_validator.cc',
    ),
    include_directories: [nanopdf_inc, nanotest_inc, validation_inc],
    link_with: nanopdf_lib,
  )

  validation_link = [nanopdf_lib, nanopdf_validation_lib, test_fixtures_lib]

  foreach t : [
    ['test_arlington_parser', 'tests/validation/test_arlington_parser.cc'],
    ['test_arlington_validate', 'tests/validation/test_arlington_validate.cc'],
    ['test_safedocs', 'tests/validation/test_safedocs.cc'],
    ['test_pdf_differences', 'tests/validation/test_pdf_differences.cc'],
    ['test_corpus_parsing', 'tests/validation/test_corpus_parsing.cc'],
  ]
    exe = executable(t[0],
      files(t[1]),
      include_directories: [nanopdf_inc, nanotest_inc, validation_inc],
      link_with: validation_link,
      cpp_args: [
        '-DNANOPDF_TEST_DATA_DIR="' + meson.current_source_dir() / 'tests' / 'data' + '"',
        '-DNANOPDF_PROJECT_DIR="' + meson.current_source_dir() + '"',
      ],
    )
    test(t[0], exe, timeout: 600)
  endforeach

endif
