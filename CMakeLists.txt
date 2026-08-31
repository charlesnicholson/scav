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

# Measured at 20% off a full load at the 2k-state target. Off by default: a
# toolchain without it is still supported, and the optimized presets set it.
option(SCAV_LTO "Link-time optimization" OFF)

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

if(SCAV_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT scav_ipo_ok OUTPUT scav_ipo_why LANGUAGES CXX)
  if(scav_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(WARNING "SCAV_LTO is on and this toolchain cannot: ${scav_ipo_why}")
  endif()
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(scav_tests_default ON)
else()
  set(scav_tests_default OFF)
endif()
option(SCAV_BUILD_TESTS "Build the unit tests" ${scav_tests_default})

# Tests are build steps, so a green build cannot hide a red test. Off keeps the
# executables and drops them from ALL, leaving `--target run.<name>`.
option(SCAV_RUN_TESTS "Run the tests as part of the build" ON)

# Off: a shared runner stalling reports a regression that is not there. The
# counted bounds always assert; this adds the wall-clock ones.
option(SCAV_PERF_FLOORS "Assert the performance timing floors" OFF)

# Language baseline

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Modules are unavailable, so scanning for them buys nothing and makes
# clang-scan-deps a build prerequisite.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
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
include(ScavEmbed)
include(ScavLibrary)
include(ScavTesting)

scav_warnings_init()
scav_sanitizers_init()
scav_coverage_init()
scav_testing_init()

if(SCAV_CLANG_TIDY)
  # envy's copy, whose version envy.lua pins. It sits behind SCAV_LINT, so
  # without that the query fails and PATH answers instead.
  if(WIN32)
    set(scav_envy "${PROJECT_SOURCE_DIR}/bin/envy.bat")
  else()
    set(scav_envy "${PROJECT_SOURCE_DIR}/bin/envy")
  endif()
  execute_process(COMMAND "${scav_envy}" product clang-tidy
    RESULT_VARIABLE scav_tidy_found
    OUTPUT_VARIABLE scav_tidy OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
  )
  if(scav_tidy_found EQUAL 0 AND EXISTS "${scav_tidy}")
    set(SCAV_CLANG_TIDY_EXE "${scav_tidy}")
  else()
    find_program(SCAV_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  endif()
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

add_subdirectory(src)

# The applications: consumers of the libraries, never privileged layers.
add_subdirectory(apps)

scav_check_layering()
if(SCAV_BUILD_TESTS)
  scav_check_tests()
  add_subdirectory(functional_tests)
endif()

# The coverage gate's list of what was supposed to be tested. Written here because
# every library has declared itself by now.
get_property(scav_production_sources GLOBAL PROPERTY SCAV_PRODUCTION_SOURCES)
# A generated source has no author and nothing a test could exercise, so it is
# not something the gate can ask about.
list(FILTER scav_production_sources EXCLUDE REGEX "^${PROJECT_BINARY_DIR}/")
list(JOIN scav_production_sources "\n" scav_production_sources_text)
file(WRITE "${PROJECT_BINARY_DIR}/scav_sources.txt" "${scav_production_sources_text}\n")

# install() + an export config package

include(CMakePackageConfigHelpers)

# The cross-library vocabulary only; scav_install_library installs each
# library's own API from its src/<lib>/include.
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
