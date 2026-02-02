cmake_minimum_required(VERSION 3.5)

project(nanopdf_img2pdf_example)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nanopdf library (assumes it's built in the parent build directory)
set(NANOPDF_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(NANOPDF_BUILD_DIR ${NANOPDF_ROOT}/build)

# Include nanopdf headers
include_directories(${NANOPDF_ROOT}/src)

# Create the img2pdf executable
add_executable(img2pdf img2pdf.cc)

# Create the test_advanced_features executable
add_executable(test_advanced_features build/test_advanced_features.cc)

# Link with nanopdf library
if(EXISTS ${NANOPDF_BUILD_DIR}/libnanopdf.a)
  target_link_libraries(img2pdf ${NANOPDF_BUILD_DIR}/libnanopdf.a)
  target_link_libraries(test_advanced_features ${NANOPDF_BUILD_DIR}/libnanopdf.a)
else()
  message(FATAL_ERROR "nanopdf library not found. Please build nanopdf first.")
endif()

# Link with zlib (required by nanopdf)
find_package(ZLIB REQUIRED)
target_link_libraries(img2pdf ZLIB::ZLIB)
target_link_libraries(test_advanced_features ZLIB::ZLIB)

# Platform-specific libraries
if(UNIX AND NOT APPLE)
  target_link_libraries(img2pdf pthread)
  target_link_libraries(test_advanced_features pthread)
endif()

# Installation
install(TARGETS img2pdf DESTINATION bin)

# Print configuration summary
message(STATUS "")
message(STATUS "img2pdf Example Configuration:")
message(STATUS "  nanopdf library: ${NANOPDF_BUILD_DIR}/libnanopdf.a")
message(STATUS "")
