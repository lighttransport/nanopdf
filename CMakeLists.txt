cmake_minimum_required(VERSION 3.16)

set(BUILD_TARGET "nanopdf")
set(TEST_TARGET "test_nanopdf")
project(${BUILD_TARGET} LANGUAGES C CXX)

# options
option(NANOPDF_USE_CCACHE "Use ccache for faster recompile." ON)
option(NANOPDF_USE_NANOSTL "Use nanostl." OFF)
option(NANOPDF_USE_STB_TRUETYPE "Use stb_truetype to load fonts." ON)

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

# [ccache]
if(NANOPDF_USE_CCACHE)
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
target_link_libraries(${BUILD_TARGET} PUBLIC z)

if (NANOPDF_USE_NANOSTL)
  # nanostl
  target_include_directories(${BUILD_TARGET} PRIVATE src/nanostl/include)
  target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_NANOSTL")
endif ()

if (NANOPDF_USE_STB_TRUETYPE)
  target_compile_definitions(${BUILD_TARGET} PRIVATE "NANOPDF_USE_STB_TRUETYPE")
endif ()

add_sanitizers(${BUILD_TARGET})

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

