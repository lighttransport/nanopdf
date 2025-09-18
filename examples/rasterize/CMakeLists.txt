cmake_minimum_required(VERSION 3.5)

project(nanopdf_rasterize_example)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nanopdf library (assumes it's built in the parent build directory)
set(NANOPDF_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(NANOPDF_BUILD_DIR ${NANOPDF_ROOT}/build)

# Include nanopdf headers
include_directories(${NANOPDF_ROOT}/src)

# Find ThorVG if enabled
option(USE_THORVG "Enable ThorVG backend" ON)
if(USE_THORVG)
  find_package(PkgConfig)
  if(PkgConfig_FOUND)
    pkg_check_modules(THORVG thorvg)
  endif()

  if(NOT THORVG_FOUND)
    # Try to find ThorVG manually
    find_path(THORVG_INCLUDE_DIR thorvg.h
      PATHS /usr/local/include/thorvg /usr/include/thorvg
      PATH_SUFFIXES thorvg)

    find_library(THORVG_LIBRARY NAMES thorvg
      PATHS /usr/local/lib /usr/lib
      PATH_SUFFIXES thorvg)

    if(THORVG_INCLUDE_DIR AND THORVG_LIBRARY)
      set(THORVG_FOUND TRUE)
      set(THORVG_INCLUDE_DIRS ${THORVG_INCLUDE_DIR})
      set(THORVG_LIBRARIES ${THORVG_LIBRARY})
      message(STATUS "Found ThorVG: ${THORVG_LIBRARY}")
    else()
      message(WARNING "ThorVG not found. Rasterize example will not work without ThorVG.")
    endif()
  endif()

  if(THORVG_FOUND)
    add_compile_definitions(NANOPDF_USE_THORVG)
    include_directories(${THORVG_INCLUDE_DIRS})
  endif()
endif()

# Create the rasterize executable
add_executable(rasterize rasterize.cc)

# Link with nanopdf library
if(EXISTS ${NANOPDF_BUILD_DIR}/libnanopdf.a)
  target_link_libraries(rasterize ${NANOPDF_BUILD_DIR}/libnanopdf.a)
else()
  message(FATAL_ERROR "nanopdf library not found. Please build nanopdf first.")
endif()

# Link with ThorVG if found
if(THORVG_FOUND)
  target_link_libraries(rasterize ${THORVG_LIBRARIES})
endif()

# Link with zlib (required by nanopdf)
find_package(ZLIB REQUIRED)
target_link_libraries(rasterize ZLIB::ZLIB)

# Platform-specific libraries
if(UNIX AND NOT APPLE)
  target_link_libraries(rasterize pthread)
endif()

# Installation
install(TARGETS rasterize DESTINATION bin)

# Print configuration summary
message(STATUS "")
message(STATUS "Rasterize Example Configuration:")
message(STATUS "  ThorVG support: ${THORVG_FOUND}")
if(THORVG_FOUND)
  message(STATUS "  ThorVG include: ${THORVG_INCLUDE_DIRS}")
  message(STATUS "  ThorVG library: ${THORVG_LIBRARIES}")
endif()
message(STATUS "")