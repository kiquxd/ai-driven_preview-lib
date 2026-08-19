cmake_minimum_required(VERSION 3.24)
project(preview VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(PREVIEW_BUILD_TESTS "Build preview library tests" ON)
option(PREVIEW_BUILD_EXAMPLES "Build manual terminal examples" OFF)
option(PREVIEW_ENABLE_SANITIZERS "Enable the configured sanitizer set" OFF)
set(PREVIEW_SANITIZER_SET "address,undefined" CACHE STRING
    "Comma-separated Clang/GCC sanitizer set")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "preview supports Linux x86-64 only")
  endif()
elseif(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  if(CMAKE_OSX_ARCHITECTURES)
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
      message(FATAL_ERROR "preview supports macOS arm64 only")
    endif()
  elseif(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "preview supports macOS arm64 only")
  endif()
else()
  message(FATAL_ERROR
    "preview supports only Linux x86-64 and macOS arm64")
endif()

add_library(preview SHARED
  src/detection.cpp
  src/engine.cpp
  src/hex_provider.cpp
  src/image_provider.cpp
  src/io.cpp
  src/local_file_source.cpp
  src/syntax_highlighter.cpp
  src/stb_impl.cpp
  src/text_provider.cpp)
add_library(preview::preview ALIAS preview)

target_compile_features(preview PUBLIC cxx_std_20)
target_compile_definitions(preview PRIVATE PREVIEW_BUILDING_LIBRARY=1)
target_include_directories(preview
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
set_target_properties(preview PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0)

target_compile_options(preview PRIVATE
  $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
set_source_files_properties(src/stb_impl.cpp PROPERTIES
  COMPILE_OPTIONS
    "-Wno-conversion;-Wno-sign-conversion;-Wno-unused-function;-Wno-unused-but-set-variable")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(preview PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/preview.map")
  if(NOT PREVIEW_ENABLE_SANITIZERS)
    target_link_options(preview PRIVATE "-Wl,--no-undefined")
  endif()
  set_target_properties(preview PROPERTIES LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/preview.map")
endif()

if(PREVIEW_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(preview PRIVATE "-fsanitize=${PREVIEW_SANITIZER_SET}"
                                           -fno-omit-frame-pointer)
    target_link_options(preview PRIVATE "-fsanitize=${PREVIEW_SANITIZER_SET}")
  else()
    message(FATAL_ERROR "PREVIEW_ENABLE_SANITIZERS requires Clang or GCC")
  endif()
endif()

if(PREVIEW_BUILD_TESTS)
  find_package(Threads REQUIRED)
  enable_testing()
  add_executable(preview_tests tests/test_preview.cpp)
  target_compile_features(preview_tests PRIVATE cxx_std_20)
  target_compile_definitions(preview_tests PRIVATE
    PREVIEW_TEST_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
  target_link_libraries(preview_tests PRIVATE preview::preview Threads::Threads)
  if(PREVIEW_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(preview_tests PRIVATE
                                                 "-fsanitize=${PREVIEW_SANITIZER_SET}"
                                                 -fno-omit-frame-pointer)
    target_link_options(preview_tests PRIVATE
                                              "-fsanitize=${PREVIEW_SANITIZER_SET}")
  endif()
  add_test(NAME preview_tests COMMAND preview_tests)
endif()

if(PREVIEW_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

configure_package_config_file(
  cmake/previewConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/previewConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/preview")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/previewConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(TARGETS preview EXPORT previewTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT previewTargets
  FILE previewTargets.cmake
  NAMESPACE preview::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/preview")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/previewConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/previewConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/preview")
install(FILES LICENSE THIRD_PARTY_NOTICES DESTINATION ${CMAKE_INSTALL_DATADIR}/preview)
install(FILES third_party/stb/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/preview/licenses
  RENAME stb-LICENSE)
