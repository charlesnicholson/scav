# A standalone project that finds scav through CMAKE_PREFIX_PATH only.

cmake_minimum_required(VERSION 3.28)

project(scav_consumer LANGUAGES CXX)

# The project uses no C++20 modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(scav REQUIRED)

add_executable(consumer main.cpp)
# Compiles every public header with nothing on the include path but the
# installed prefix.
target_link_libraries(consumer PRIVATE
  scav::core scav::layout scav::draw scav::svg)

# The consumer picks its own language level; the exported target asks only for
# cxx_std_20.
set_target_properties(consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
