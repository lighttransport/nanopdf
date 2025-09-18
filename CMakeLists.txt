cmake_minimum_required(VERSION 3.16)

set(BUILD_TARGET "nanopdf")
set(TEST_TARGET "test_nanopdf")
project(${BUILD_TARGET} LANGUAGES C CXX)

# options
option(NANOPDF_USE_CCACHE "Use ccache for faster recompile." ON)
option(NANOPDF_USE_NANOSTL "Use nanostl." OFF)
option(NANOPDF_USE_STB_TRUETYPE "Use stb_truetype to load fonts." ON)
option(NANOPDF_BUILD_TESTS "Build test executables" ON)
option(NANOPDF_BUILD_WASM "Build for WebAssembly with Emscripten" OFF)

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

add_library(${BUILD_TARGET} src/nanopdf.cc src/jbig2-decoder.cc src/canvas-exporter.cc src/crypto.cc src/pdf-security.cc)
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

# Only add sanitizers for non-WASM builds
if(NOT NANOPDF_BUILD_WASM)
  add_sanitizers(${BUILD_TARGET})
endif()

# Build test executables only if requested and not building for WASM
if(NANOPDF_BUILD_TESTS AND NOT NANOPDF_BUILD_WASM)
  add_executable(${TEST_TARGET} src/test-nanopdf.cc)
  target_link_libraries(${TEST_TARGET} ${BUILD_TARGET})

  # Phase 1 features test
  add_executable(test_phase1 src/test-phase1-features.cc)
  target_link_libraries(test_phase1 ${BUILD_TARGET})

  # Phase 1 simple test (without Value objects)
  add_executable(test_phase1_simple src/test-phase1-simple.cc)
  target_link_libraries(test_phase1_simple ${BUILD_TARGET})

  # Phase 2 features test
  add_executable(test_phase2 src/test-phase2-features.cc)
  target_link_libraries(test_phase2 ${BUILD_TARGET})

  # Phase 3 features test
  add_executable(test_phase3 src/test-phase3-features.cc)
  target_link_libraries(test_phase3 ${BUILD_TARGET})

  # Phase 4 features test
  add_executable(test_phase4 src/test-phase4-features.cc)
  target_link_libraries(test_phase4 ${BUILD_TARGET})

  # Phase 5 features test
  add_executable(test_phase5 src/test-phase5-features.cc)
  target_link_libraries(test_phase5 ${BUILD_TARGET})

  # Phase 6 features test (CCITTFaxDecode, transparency, patterns, tagged PDF)
  add_executable(test_phase6 src/test-phase6-features.cc)
  target_link_libraries(test_phase6 ${BUILD_TARGET})
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

