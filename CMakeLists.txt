# scav -- statechart authoring, layout, rendering. Nothing here assumes envy: any
# conforming C++20 toolchain configures this tree.

cmake_minimum_required(VERSION 3.28)

project(scav
  VERSION 0.1.0
  DESCRIPTION "Statechart authoring, layout, and rendering"
  LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

include(GNUInstallDirs)
include(CMakeDependentOption)

# Options

# Puts SCAV_TESTING on the *shipping* libraries too. All layout arithmetic is
# integer, so this must produce byte-identical output to release.
option(SCAV_TESTING "Define SCAV_TESTING for the shipping libraries too" OFF)

option(SCAV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(SCAV_CLANG_TIDY "Run clang-tidy as part of compilation" OFF)
option(SCAV_COVERAGE "Instrument for branch coverage" OFF)

# One enum, not booleans: ASan and TSan cannot coexist.
set(SCAV_SANITIZER "NONE" CACHE STRING "Sanitizer: NONE|ASAN|UBSAN|TSAN|MSAN")
set_property(CACHE SCAV_SANITIZER PROPERTY STRINGS NONE ASAN UBSAN TSAN MSAN)

# MSan reports false positives against any uninstrumented standard library, so it
# needs a libc++ of its own.
set(SCAV_MSAN_LIBCXX_DIR "" CACHE PATH "Prefix of a libc++ built with MSan")

# Set by the build driver from what envy provisioned; a consumer without envy
# points it at their own copy or turns tests off.
set(SCAV_DOCTEST_DIR "" CACHE PATH "Directory containing doctest.h")

if(PROJECT_IS_TOP_LEVEL)
  set(scav_tests_default ON)
else()
  set(scav_tests_default OFF)
endif()
option(SCAV_BUILD_TESTS "Build the unit tests" ${scav_tests_default})

# Language baseline

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keeps artifacts from scattering across per-subsystem subdirectories of the build
# tree.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(ScavWarnings)
include(ScavSanitizers)
include(ScavCoverage)
include(ScavLibrary)
include(ScavTesting)

scav_warnings_init()
scav_sanitizers_init()
scav_coverage_init()
scav_testing_init()

if(SCAV_CLANG_TIDY)
  find_program(SCAV_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  set(CMAKE_CXX_CLANG_TIDY "${SCAV_CLANG_TIDY_EXE}" "--warnings-as-errors=*")
  # An upstream clang-tidy has no default macOS sysroot, so every diagnostic
  # arrives as a missing standard header instead of the finding it looked for.
  if(APPLE)
    # Empty under Command Line Tools, where the compiler finds the SDK implicitly.
    set(scav_sysroot "${CMAKE_OSX_SYSROOT}")
    if(NOT scav_sysroot)
      execute_process(COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE scav_sysroot OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
    endif()
    if(scav_sysroot)
      list(APPEND CMAKE_CXX_CLANG_TIDY "--extra-arg=-isysroot${scav_sysroot}")
    else()
      message(WARNING
        "SCAV_CLANG_TIDY on macOS with no SDK path: the pinned clang-tidy is an "
        "upstream LLVM build with no default sysroot, so every file will report "
        "a missing standard header instead of a finding.")
    endif()
  endif()
endif()

# The libraries

# Before add_subdirectory(src), which registers tests: add_test() needs testing
# enabled in an enclosing scope first.
if(SCAV_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

scav_check_layering()
if(SCAV_BUILD_TESTS)
  scav_check_tests()
  add_subdirectory(functional_tests)
endif()

# The coverage gate's list of what was supposed to be tested. Written here because
# every library has declared itself by now.
get_property(scav_production_sources GLOBAL PROPERTY SCAV_PRODUCTION_SOURCES)
list(JOIN scav_production_sources "\n" scav_production_sources_text)
file(WRITE "${PROJECT_BINARY_DIR}/scav_sources.txt" "${scav_production_sources_text}\n")

# install() + an export config package

include(CMakePackageConfigHelpers)

install(DIRECTORY include/scav
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h"
)

install(EXPORT scavTargets
  FILE scav-targets.cmake
  NAMESPACE scav::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scav"
)

configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/cmake/scav-config.cmake.in"
  "${PROJECT_BINARY_DIR}/scav-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scav"
)

# The C ABI is additive within a minor version.
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/scav-config-version.cmake"
  COMPATIBILITY SameMinorVersion
)

install(FILES
  "${PROJECT_BINARY_DIR}/scav-config.cmake"
  "${PROJECT_BINARY_DIR}/scav-config-version.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/scav"
)
