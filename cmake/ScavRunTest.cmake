# Runs one test with its output captured, and prints nothing unless it fails.
#
# A passing test has nothing to say. Everything it would have said is exactly
# what you want on the failure, so it is held and echoed then -- including the
# output of anything it shells out to, which is where most of the noise came
# from: nested cmake configures, a negative test whose expected result is a
# CMake error, and doctest's own banner.
#
# `cmake -P` cannot take a list argument, so the argv arrives `|`-separated.

if(NOT DEFINED SCAV_CMD)
  message(FATAL_ERROR "ScavRunTest.cmake needs -DSCAV_CMD")
endif()

string(REPLACE "|" ";" argv "${SCAV_CMD}")

execute_process(
  COMMAND ${argv}
  RESULT_VARIABLE code
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

if(NOT code EQUAL 0)
  message("${out}")
  message("${err}")
  message(FATAL_ERROR "${SCAV_LABEL} failed (exit ${code})")
endif()
