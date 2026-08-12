# A separate project on purpose. What is absent is the test: no envy, no toolchain
# file, no scav-specific cache variables, nothing but CMAKE_PREFIX_PATH.

cmake_minimum_required(VERSION 3.28)

project(scav_consumer LANGUAGES CXX)

# This project uses no modules, so do not make clang-scan-deps a prerequisite of
# building it. Nothing to do with scav; any C++20 project wants this.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_package(scav REQUIRED)

add_executable(consumer main.cpp)
# Both libraries, so the consumer compiles every public header of each with
# nothing on the include path but the installed prefix.
target_link_libraries(consumer PRIVATE scav::core scav::toy)

# The consumer picks its own language level; the exported target asks only for
# cxx_std_20.
set_target_properties(consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
