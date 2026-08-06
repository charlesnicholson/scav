# Tests are build steps, not a second command.
#
# Every test -- unit and functional -- is an add_custom_command whose OUTPUT is a
# stamp file, wired into ALL. So `cmake --build` builds *and* verifies, a build
# cannot report success with a failing test, and a second build back to back is
# an immediate no-op because every stamp is newer than its inputs.
#
# That last property is why CTest is gone rather than wrapped: ctest re-runs
# everything on every invocation. It has no notion of a test being up to date, so
# "build, then test" can never be incremental and the second `./build.sh` always
# pays for the first one again.
#
# Mocks and interface seams are rejected (PRD 5), so a unit test is an ordinary
# translation unit compiled against the testable archive, reaching internals via
# SCAV_INTERNAL.

include_guard(GLOBAL)

function(scav_testing_init)
  if(NOT SCAV_BUILD_TESTS)
    return()
  endif()

  # find_path returns the directory *containing* the header, so the amalgamated
  # layout and an upstream install both resolve `#include "doctest.h"`.
  find_path(SCAV_DOCTEST_INCLUDE_DIR doctest.h
    HINTS "${SCAV_DOCTEST_DIR}"
    PATH_SUFFIXES doctest include/doctest
    DOC "Directory containing doctest.h"
  )

  if(NOT SCAV_DOCTEST_INCLUDE_DIR)
    message(FATAL_ERROR
      "doctest.h not found. Either run `./bin/envy sync` and configure through "
      "build.sh (which passes -DSCAV_DOCTEST_DIR from `envy product "
      "doctest_cpp_dir`), point -DSCAV_DOCTEST_DIR at your own copy, or "
      "configure with -DSCAV_BUILD_TESTS=OFF.")
  endif()

  add_library(scav_doctest INTERFACE)
  # SYSTEM: doctest.h is not ours to keep clean under the pinned warning set.
  target_include_directories(scav_doctest SYSTEM INTERFACE
    "${SCAV_DOCTEST_INCLUDE_DIR}")
  # Without exceptions doctest drops its REQUIRE family unless told to keep them,
  # implemented as an abort instead of a throw.
  target_compile_definitions(scav_doctest INTERFACE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
  )

  # One place for every stamp, so `rm -rf out/<preset>/stamp` re-runs the whole
  # suite without rebuilding anything.
  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/stamp")
  set(SCAV_STAMP_DIR "${PROJECT_BINARY_DIR}/stamp" PARENT_SCOPE)
endfunction()

# scav_test_environment(<out var> <stamp name>)
#
# One environment for every test, so a developer's run and CI's run suppress the
# same findings and fail on the same conditions. Yields `NAME=value` pairs for
# `cmake -E env`.
function(scav_test_environment out_var stamp_name)
  # Bare filenames: these strings are colon-separated, so `D:/a/scav` would split
  # at the drive letter. Every test runs from the source directory already.
  set(env "")

  if(SCAV_SANITIZER STREQUAL "ASAN")
    set(opts "abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1")
    # MSVC's AddressSanitizer accepts a fixed set of suppression kinds and rejects
    # anything else in the file, comments included. Nothing to suppress there.
    if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
      string(APPEND opts ":suppressions=asan.supp")
    endif()
    # LeakSanitizer is a Linux-only companion to ASan in the pinned toolchains.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      string(APPEND opts ":detect_leaks=1")
      list(APPEND env "LSAN_OPTIONS=suppressions=lsan.supp")
    endif()
    list(APPEND env "ASAN_OPTIONS=${opts}")
  elseif(SCAV_SANITIZER STREQUAL "UBSAN")
    list(APPEND env
      "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:suppressions=ubsan.supp")
  elseif(SCAV_SANITIZER STREQUAL "TSAN")
    list(APPEND env
      "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=tsan.supp")
  elseif(SCAV_SANITIZER STREQUAL "MSAN")
    list(APPEND env "MSAN_OPTIONS=halt_on_error=1:poison_in_dtor=1")
  endif()

  if(SCAV_COVERAGE)
    list(APPEND env
      "LLVM_PROFILE_FILE=${PROJECT_BINARY_DIR}/coverage/${stamp_name}-%p.profraw")
  endif()

  set(${out_var} "${env}" PARENT_SCOPE)
endfunction()

# scav_stamped_test(<stamp name> COMMAND <argv> DEPENDS <files> [TARGETS <t>] [COMMENT <s>])
#
# The shape every test shares: delete the stamp, run, touch the stamp. Deleting
# first means an interrupted or crashed run never leaves behind a stamp that a
# later build would trust.
function(scav_stamped_test stamp_name)
  cmake_parse_arguments(arg "" "COMMENT" "DEPENDS;COMMAND;TARGETS;ENV" ${ARGN})

  set(stamp "${SCAV_STAMP_DIR}/${stamp_name}.passed")
  scav_test_environment(env "${stamp_name}")
  list(APPEND env ${arg_ENV})

  add_custom_command(
    OUTPUT "${stamp}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
    COMMAND "${CMAKE_COMMAND}" -E env ${env} ${arg_COMMAND}
    COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
    DEPENDS ${arg_DEPENDS}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "${arg_COMMENT}"
    # The console pool: test output reaches the terminal in order, rather than
    # being buffered and interleaved with compile lines.
    USES_TERMINAL
    VERBATIM
  )

  # Not in ALL when SCAV_RUN_TESTS is off, so `--target run.<name>` still works
  # for someone who wants one test without turning the option back on.
  set(all_arg "ALL")
  if(NOT SCAV_RUN_TESTS)
    set(all_arg "")
  endif()
  add_custom_target(run.${stamp_name} ${all_arg} DEPENDS "${stamp}")
  if(arg_TARGETS)
    add_dependencies(run.${stamp_name} ${arg_TARGETS})
  endif()
endfunction()

# scav_tests(<name> <source>...)
#
# A doctest executable that runs as part of the build. Link whatever it tests
# yourself -- usually a library's _testable archive.
function(scav_tests name)
  add_executable(${name} ${ARGN} "${PROJECT_SOURCE_DIR}/src/doctest_main.cpp")
  scav_settings(${name})
  target_link_libraries(${name} PRIVATE scav_doctest)
  target_compile_definitions(${name} PRIVATE
    SCAV_TESTING
    "SCAV_TEST_DATA_DIR=\"${PROJECT_SOURCE_DIR}/test_data\""
    "SCAV_TEST_OUT_DIR=\"${PROJECT_BINARY_DIR}/test\""
  )

  scav_stamped_test(${name}
    COMMAND "$<TARGET_FILE:${name}>" --order-by=name
    DEPENDS "$<TARGET_FILE:${name}>"
    TARGETS ${name}
    COMMENT "unit ${name}"
  )
  set_property(GLOBAL APPEND PROPERTY SCAV_TEST_EXECUTABLES ${name})
endfunction()

# scav_expect_test_failure(<name> <doctest case filter>)
#
# A harness that silently passes everything looks identical to a working one, so a
# case is written to fail and this asserts the failure is reported. The inversion
# needs a script: a build step succeeds by exiting zero, and this one has to
# succeed by exiting non-zero.
function(scav_expect_test_failure name filter)
  scav_stamped_test(${name}_reports_failures
    COMMAND "${CMAKE_COMMAND}"
            "-DSCAV_EXE=$<TARGET_FILE:${name}>"
            "-DSCAV_FILTER=${filter}"
            -P "${PROJECT_SOURCE_DIR}/cmake/ScavExpectFailure.cmake"
    DEPENDS "$<TARGET_FILE:${name}>"
            "${PROJECT_SOURCE_DIR}/cmake/ScavExpectFailure.cmake"
    TARGETS ${name}
    COMMENT "meta ${name} reports failures"
  )
endfunction()

# scav_check_tests()
#
# Every library is exercised by some test executable. A phase is not done until its
# tests are, and an untested file fails the build.
function(scav_check_tests)
  get_property(libraries GLOBAL PROPERTY SCAV_LIBRARIES)
  get_property(executables GLOBAL PROPERTY SCAV_TEST_EXECUTABLES)

  # ctest had noTestsAction=error for this; with tests as build steps the
  # equivalent is a configure-time check, and it fires earlier.
  if(NOT executables)
    message(FATAL_ERROR
      "SCAV_BUILD_TESTS is on but no test executable was declared. A build that "
      "verifies nothing reports the same green as one that verifies everything.")
  endif()

  set(tested "")
  foreach(executable IN LISTS executables)
    get_target_property(links ${executable} LINK_LIBRARIES)
    list(APPEND tested ${links})
  endforeach()

  foreach(library IN LISTS libraries)
    if(NOT ${library}_testable IN_LIST tested)
      message(FATAL_ERROR
        "${library} has no test executable linking ${library}_testable. Declare one "
        "with scav_tests().")
    endif()
  endforeach()
endfunction()
