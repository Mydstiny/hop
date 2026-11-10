cmake_minimum_required(VERSION 3.16)
project(hop_linux_tls LANGUAGES C CXX)
# Development-only shared C ABI. The final package must pin and audit its
# dependency and place the library at a verified, non-writable product path.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
if(NOT HOP_MBEDTLS_SOURCE_DIR)
  message(FATAL_ERROR "Set HOP_MBEDTLS_SOURCE_DIR to the independently verified mbedTLS 3.6.7 source")
endif()
add_subdirectory("${HOP_MBEDTLS_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls")
add_library(hop_tls_runtime SHARED
  ../../../core/secure/secure_channel.cpp
  ../../../core/secure/secure_channel_c.cpp
  ../../../core/secure/tls_identity.cpp)
target_include_directories(hop_tls_runtime PUBLIC ../../../core/include)
target_compile_features(hop_tls_runtime PUBLIC cxx_std_17)
target_link_libraries(hop_tls_runtime PRIVATE mbedtls mbedx509 mbedcrypto)

# Stateless policy plus stateful ordered key translator, exposed through the
# shared C ABI. Keep the runtime in a trusted installation path in releases.
add_library(hop_shortcut_runtime SHARED
  ../../../core/src/input_policy.cpp
  ../../../core/src/shortcut_translator.cpp
  ../../../core/src/shortcut_translator_c.cpp)
target_include_directories(hop_shortcut_runtime PUBLIC ../../../core/include)
target_compile_features(hop_shortcut_runtime PUBLIC cxx_std_17)

add_library(hop_clipboard_runtime SHARED
  ../../../core/src/clipboard_transfer.cpp
  ../../../core/src/clipboard_transfer_c.cpp)
target_include_directories(hop_clipboard_runtime PUBLIC ../../../core/include)
target_compile_features(hop_clipboard_runtime PUBLIC cxx_std_17)
