cmake_minimum_required(VERSION 3.16)
project(ncrypto LANGUAGES C)

# Pure C11 crypto library — an OpenSSL-free stack for PDF signing / TLS:
# hashing, HMAC/HKDF/PBKDF2, AES-GCM/CBC, X25519, BigInt, RSA(+PSS),
# ECDSA P-256/P-384, ASN.1 DER, X.509, CMS, RFC 3161, PKCS#12, TLS 1.3 client.
# No third-party dependencies (libc only).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

file(GLOB NCRYPTO_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_library(ncrypto STATIC ${NCRYPTO_SOURCES})
target_include_directories(ncrypto PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(NOT MSVC)
  target_compile_options(ncrypto PRIVATE -Wall -Wextra)
endif()

# Tests: each tests/test_*.c is a standalone executable with its own main().
option(NCRYPTO_BUILD_TESTS "Build ncrypto tests" ON)
if(NCRYPTO_BUILD_TESTS)
  enable_testing()
  file(GLOB NCRYPTO_TESTS CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c)
  foreach(test_src ${NCRYPTO_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE ncrypto)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
