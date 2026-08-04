# Mocks and interface seams are rejected, so a unit test is an ordinary translation
# unit compiled against the testable archive, reaching internals via SCAV_INTERNAL.

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
endfunction()

# One environment for every test, so a developer's run and CI's run suppress the
# same findings and fail on the same conditions.
function(scav_set_test_environment test_name)
  # Bare filenames: these strings are colon-separated, so `D:/a/scav` would split
  # at the drive letter. Every test runs from the source directory already.
  set(env "")

  if(SCAV_SANITIZER STREQUAL "ASAN")
    set(opts "abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1")
    string(APPEND opts ":suppressions=asan.supp")
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
      "LLVM_PROFILE_FILE=${PROJECT_BINARY_DIR}/coverage/${test_name}-%p.profraw")
  endif()

  if(env)
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${env}")
  endif()
endfunction()

# scav_tests(<name> <source>...)
#
# A doctest executable registered with ctest as unit.<name>. Link whatever it tests
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

  add_test(NAME unit.${name} COMMAND ${name} --order-by=name)
  set_tests_properties(unit.${name} PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  )
  scav_set_test_environment(unit.${name})
  set_property(GLOBAL APPEND PROPERTY SCAV_TEST_EXECUTABLES ${name})
endfunction()

# scav_expect_test_failure(<name> <doctest case filter>)
#
# A harness that silently passes everything looks identical to a working one, so a
# case is written to fail and this asserts that the failure is reported.
function(scav_expect_test_failure name filter)
  set(test "meta.${name}_reports_failures")
  add_test(NAME ${test} COMMAND ${name} --no-skip "--test-case=${filter}")
  set_tests_properties(${test} PROPERTIES
    WILL_FAIL TRUE
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  )
  # Also environment-set, because an instrumented binary with no LLVM_PROFILE_FILE
  # drops a default.profraw into its working directory, which is the source tree.
  scav_set_test_environment(${test})
endfunction()

# scav_check_tests()
#
# Every library is exercised by some test executable. A phase is not done until its
# tests are, and an untested file fails the build.
function(scav_check_tests)
  get_property(libraries GLOBAL PROPERTY SCAV_LIBRARIES)
  get_property(executables GLOBAL PROPERTY SCAV_TEST_EXECUTABLES)

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
