# A separate project on purpose. What is absent is the test: no envy, no toolchain
# file, no scav-specific cache variables, nothing but CMAKE_PREFIX_PATH.

cmake_minimum_required(VERSION 3.28)

project(scav_consumer LANGUAGES CXX)

find_package(scav REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE scav::toy)

# The consumer picks its own language level; the exported target asks only for
# cxx_std_20.
set_target_properties(consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
