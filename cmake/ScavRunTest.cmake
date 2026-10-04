# Runs SCAV_CMD, an argv joined with `|`, and prints its captured output only on
# failure.

if(NOT DEFINED SCAV_CMD)
  message(FATAL_ERROR "ScavRunTest.cmake needs -DSCAV_CMD")
endif()

string(REPLACE "|" ";" argv "${SCAV_CMD}")

string(TIMESTAMP t0 "%s" UTC)
execute_process(
  COMMAND ${argv}
  RESULT_VARIABLE code
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err
)

string(TIMESTAMP t1 "%s" UTC)
math(EXPR seconds "${t1} - ${t0}")

if(NOT code EQUAL 0)
  message("${out}")
  message("${err}")
  message(FATAL_ERROR "${SCAV_LABEL} failed (exit ${code}) after ${seconds} s")
endif()
message("${SCAV_LABEL}: ${seconds} s")
