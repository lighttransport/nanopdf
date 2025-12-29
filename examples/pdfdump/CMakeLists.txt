cmake_minimum_required(VERSION 3.5)

project(nanopdf_pdfdump_example)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nanopdf library (assumes it's built in the parent build directory)
set(NANOPDF_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(NANOPDF_BUILD_DIR ${NANOPDF_ROOT}/build)

# Include nanopdf headers
include_directories(${NANOPDF_ROOT}/src)

# Create the pdfdump executable
add_executable(pdfdump pdfdump.cc)

# Link with nanopdf library
if(EXISTS ${NANOPDF_BUILD_DIR}/libnanopdf.a)
  target_link_libraries(pdfdump ${NANOPDF_BUILD_DIR}/libnanopdf.a)
else()
  message(FATAL_ERROR "nanopdf library not found. Please build nanopdf first.")
endif()

# Link with zlib (required by nanopdf)
find_package(ZLIB REQUIRED)
target_link_libraries(pdfdump ZLIB::ZLIB)

# Platform-specific libraries
if(UNIX AND NOT APPLE)
  target_link_libraries(pdfdump pthread)
endif()

# Installation
install(TARGETS pdfdump DESTINATION bin)

# Print configuration summary
message(STATUS "")
message(STATUS "PDFDump Example Configuration:")
message(STATUS "  nanopdf library: ${NANOPDF_BUILD_DIR}/libnanopdf.a")
message(STATUS "")
