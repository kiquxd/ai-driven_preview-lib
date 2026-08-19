cmake_minimum_required(VERSION 3.24)
project(preview_add_subdirectory_consumer LANGUAGES C CXX)

set(PREVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../.. preview-library)
add_executable(preview_add_subdirectory_consumer ../consumer/main.cpp)
target_compile_features(preview_add_subdirectory_consumer PRIVATE cxx_std_20)
target_link_libraries(preview_add_subdirectory_consumer PRIVATE preview::preview)
