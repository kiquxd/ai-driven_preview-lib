cmake_minimum_required(VERSION 3.24)
project(preview_consumer LANGUAGES CXX)

find_package(preview CONFIG REQUIRED)
add_executable(preview_consumer main.cpp)
target_compile_features(preview_consumer PRIVATE cxx_std_20)
target_link_libraries(preview_consumer PRIVATE preview::preview)
