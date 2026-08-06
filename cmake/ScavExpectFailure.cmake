# Runs one doctest case that is written to fail, and succeeds only if it did.
#
# Invoked with `cmake -DSCAV_EXE=... -DSCAV_FILTER=... -P`. It exists because a
# build step reports success by exiting zero, and this one has to report success
# by exiting non-zero -- there is no way to say that in a custom command.
#
# The failing case's own output is captured rather than printed: it is expected,
# and a build log full of a deliberate failure trains people to ignore the real
# ones. It is echoed only when the case unexpectedly passes, which is the case
# someone actually has to read.

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
