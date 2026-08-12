# Runs one doctest case written to fail, and succeeds only if it did -- which a
# custom command cannot express, since a build step succeeds by exiting zero.
#
# Its output is captured, not printed: a build log full of a deliberate failure
# trains people to ignore the real ones. Echoed only on an unexpected pass.

if(NOT DEFINED SCAV_EXE OR NOT DEFINED SCAV_FILTER)
  message(FATAL_ERROR "ScavExpectFailure.cmake needs -DSCAV_EXE and -DSCAV_FILTER")
endif()

execute_process(
  COMMAND "${SCAV_EXE}" --no-skip "--test-case=${SCAV_FILTER}"
  RESULT_VARIABLE code
  OUTPUT_VARIABLE captured
  ERROR_VARIABLE captured_err
)

if(code EQUAL 0)
  message("${captured}")
  message("${captured_err}")
  message(FATAL_ERROR
    "'${SCAV_FILTER}' exited 0. That case is written to fail; a harness that "
    "reports nothing looks identical to one where everything passes, and this "
    "is the check that tells them apart.")
endif()
