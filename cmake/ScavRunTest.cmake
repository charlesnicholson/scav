# Runs one test with its output captured, echoed only on a failure -- including
# whatever it shelled out to. `cmake -P` takes no list, so argv is `|`-separated.

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
