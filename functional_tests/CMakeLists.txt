# Python, standard library only: a functional test that needs `pip install` is one
# more thing between a clean checkout and a green run.
#
# These are stamped build steps like the unit tests, so `cmake --build` runs them
# and a second build skips them. Each one declares the files it actually reads,
# which is what makes the skip correct rather than merely fast: edit ci.yml and
# only func.ci_anchor re-runs.

find_package(Python3 3.14 COMPONENTS Interpreter REQUIRED)

# Handed to the Python side as data rather than rediscovered there: a test that
# guesses the compiler is testing its guess.
#
# Forward slashes throughout: a Windows path drops raw into the JSON, where every
# backslash is an escape and `D:\a` is a parse error.
foreach(pair
  "SOURCE_DIR;${PROJECT_SOURCE_DIR}"
  "BINARY_DIR;${PROJECT_BINARY_DIR}"
  "CMAKE;${CMAKE_COMMAND}"
  "MAKE_PROGRAM;${CMAKE_MAKE_PROGRAM}"
  "CXX_COMPILER;${CMAKE_CXX_COMPILER}"
  "DOCTEST_DIR;${SCAV_DOCTEST_INCLUDE_DIR}"
  "PYTHON;${Python3_EXECUTABLE}"
)
  list(GET pair 0 name)
  list(GET pair 1 value)
  file(TO_CMAKE_PATH "${value}" SCAV_FT_${name})
endforeach()

set(SCAV_FT_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/scav_build_config.json")
configure_file(scav_build_config.json.in "${SCAV_FT_CONFIG}" @ONLY)

# scav_add_functional_test(<name> [DEPENDS <files>] [TARGETS <targets>])
function(scav_add_functional_test name)
  cmake_parse_arguments(arg "" "" "DEPENDS;TARGETS" ${ARGN})
  scav_stamped_test(func.${name}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.py" -v
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/scavtest.py"
            "${SCAV_FT_CONFIG}"
            ${arg_DEPENDS}
    TARGETS ${arg_TARGETS}
    # The one thing the Python side cannot derive for itself.
    ENV "SCAV_BUILD_CONFIG=${SCAV_FT_CONFIG}"
    COMMENT "functional ${name}"
  )
endfunction()

# install_consumer installs the whole project and configures a separate CMake
# project against it, so it has to run after every archive and header exists.
get_property(scav_all_libraries GLOBAL PROPERTY SCAV_LIBRARIES)
get_property(scav_all_test_exes GLOBAL PROPERTY SCAV_TEST_EXECUTABLES)
set(scav_installed_files "")
foreach(library IN LISTS scav_all_libraries)
  list(APPEND scav_installed_files "$<TARGET_FILE:${library}>")
endforeach()

scav_add_functional_test(install_consumer
  DEPENDS ${scav_installed_files}
          "${CMAKE_CURRENT_SOURCE_DIR}/consumer/CMakeLists.txt"
          "${CMAKE_CURRENT_SOURCE_DIR}/consumer/main.cpp"
  TARGETS ${scav_all_libraries} ${scav_all_test_exes}
)
scav_add_functional_test(dump
  DEPENDS "${PROJECT_SOURCE_DIR}/test_data/charts/brew.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/vac.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/dock.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/led.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/mill.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/axis.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/toolchanger.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/estop.scav"
          "${PROJECT_SOURCE_DIR}/test_data/golden/dump/brew.txt"
          "${PROJECT_SOURCE_DIR}/test_data/golden/dump/vac.txt"
          "${PROJECT_SOURCE_DIR}/test_data/golden/dump/mill.txt"
          "$<TARGET_FILE:scav>"
  TARGETS scav
)
# P2's exit gate: the load session driven from Python over ctypes, faking a
# network fetch. Needs the shared library, not an archive.
scav_add_functional_test(load
  DEPENDS "${PROJECT_SOURCE_DIR}/test_data/charts/vac.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/dock.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/led.scav"
          "$<TARGET_FILE:scav_shared>"
          "$<TARGET_FILE:scav>"
  TARGETS scav_shared scav
)
scav_add_functional_test(presets
  DEPENDS "${PROJECT_SOURCE_DIR}/CMakePresets.json"
          "${PROJECT_SOURCE_DIR}/tools/gen_presets.py"
)
scav_add_functional_test(provisioning
  DEPENDS "${PROJECT_SOURCE_DIR}/envy.lua"
)
scav_add_functional_test(ci_anchor
  DEPENDS "${PROJECT_SOURCE_DIR}/.github/workflows/ci.yml"
)
