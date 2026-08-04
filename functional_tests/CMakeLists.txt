# Python, standard library only: a functional test that needs `pip install` is one
# more thing between a clean checkout and a green run.

find_package(Python3 3.14 COMPONENTS Interpreter REQUIRED)

# Handed to the Python side as data rather than rediscovered there: a test that
# guesses the compiler is testing its guess.
set(SCAV_FT_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/scav_build_config.json")
configure_file(scav_build_config.json.in "${SCAV_FT_CONFIG}" @ONLY)

function(scav_add_functional_test name)
  add_test(NAME func.${name}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.py" -v
  )
  # ENVIRONMENT adds rather than replaces, so a test can still read variables set
  # on the invocation.
  set_tests_properties(func.${name} PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    ENVIRONMENT "SCAV_BUILD_CONFIG=${SCAV_FT_CONFIG}"
  )
endfunction()

scav_add_functional_test(install_consumer)
scav_add_functional_test(presets)
scav_add_functional_test(provisioning)
