cmake_minimum_required(VERSION 3.24)
project(preview_examples LANGUAGES C CXX)

if(NOT TARGET preview::preview)
  set(PREVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(PREVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(PREVIEW_ENABLE_SANITIZERS OFF CACHE BOOL "" FORCE)
  add_subdirectory(.. preview-library)
endif()

add_executable(preview_demo preview_demo.cpp)
target_compile_features(preview_demo PRIVATE cxx_std_20)
target_link_libraries(preview_demo PRIVATE preview::preview)
