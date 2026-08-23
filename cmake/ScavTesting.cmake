# Each test is a custom command whose output is a stamp, wired into ALL, so a
# second build is a no-op -- which is why there is no CTest.

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

  # `rm -rf out/<preset>/stamp` re-runs the suite without rebuilding anything.
  file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/stamp")
# Tests write scratch under here; a fresh tree must not depend on which test
# happens to create it first.
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/test")
  set(SCAV_STAMP_DIR "${PROJECT_BINARY_DIR}/stamp" PARENT_SCOPE)
endfunction()

# One environment for every test, so a local run and CI's suppress the same
# findings. Yields `NAME=value` pairs for `cmake -E env`.
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

# Delete the stamp, run, touch it -- so a crashed run leaves behind no stamp a
# later build would trust.
function(scav_stamped_test stamp_name)
  cmake_parse_arguments(arg "" "COMMENT" "DEPENDS;COMMAND;TARGETS;ENV" ${ARGN})

  set(stamp "${SCAV_STAMP_DIR}/${stamp_name}.passed")
  scav_test_environment(env "${stamp_name}")
  list(APPEND env ${arg_ENV})

  # `cmake -P` takes no list argument, so the argv is joined for the runner to
  # split. `|` cannot appear in a path or a doctest filter here.
  set(argv "${CMAKE_COMMAND}|-E|env")
  foreach(pair IN LISTS env)
    string(APPEND argv "|${pair}")
  endforeach()
  foreach(word IN LISTS arg_COMMAND)
    string(APPEND argv "|${word}")
  endforeach()

  add_custom_command(
    OUTPUT "${stamp}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${stamp}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSCAV_CMD=${argv}"
            "-DSCAV_LABEL=${arg_COMMENT}"
            -P "${PROJECT_SOURCE_DIR}/cmake/ScavRunTest.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
    DEPENDS ${arg_DEPENDS} "${PROJECT_SOURCE_DIR}/cmake/ScavRunTest.cmake"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "${arg_COMMENT}"
    USES_TERMINAL
    VERBATIM
  )

  # Out of ALL when SCAV_RUN_TESTS is off; `--target run.<name>` still works.
  set(all_arg "ALL")
  if(NOT SCAV_RUN_TESTS)
    set(all_arg "")
  endif()
  add_custom_target(run.${stamp_name} ${all_arg} DEPENDS "${stamp}")
  if(arg_TARGETS)
    add_dependencies(run.${stamp_name} ${arg_TARGETS})
  endif()
endfunction()

# A doctest executable that runs as part of the build. Link what it tests
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

# One case is written to fail. Inverting needs a script: a build step succeeds by
# exiting zero, and this succeeds by exiting non-zero.
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

# Every library is exercised by some test executable.
function(scav_check_tests)
  get_property(libraries GLOBAL PROPERTY SCAV_LIBRARIES)
  get_property(executables GLOBAL PROPERTY SCAV_TEST_EXECUTABLES)

  # ctest's noTestsAction=error, as a configure-time check that fires earlier.
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
