# Python, standard library only. Stamped build steps like the unit tests, each
# declaring the files it reads, so a second build skips what did not change.

find_package(Python3 3.14 COMPONENTS Interpreter REQUIRED)

# Data rather than rediscovered: a test that guesses the compiler tests its
# guess. Forward slashes, since every backslash in the JSON is an escape.
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

# The whole corpus, for the gates that read all of it rather than one root.
set(scav_corpus_charts
  "${PROJECT_SOURCE_DIR}/test_data/charts/axis.scav"
  "${PROJECT_SOURCE_DIR}/test_data/charts/brew.scav"
  "${PROJECT_SOURCE_DIR}/test_data/charts/dock.scav"
  "${PROJECT_SOURCE_DIR}/test_data/charts/estop.scav"
  "${PROJECT_SOURCE_DIR}/test_data/charts/led.scav"
  "${PROJECT_SOURCE_DIR}/test_data/charts/mill.scav"
  "${PROJECT_SOURCE_DIR}/test_data/charts/toolchanger.scav"
  "${PROJECT_SOURCE_DIR}/test_data/charts/vac.scav"
)

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
  DEPENDS ${scav_corpus_charts}
          "${PROJECT_SOURCE_DIR}/test_data/golden/dump/brew.txt"
          "${PROJECT_SOURCE_DIR}/test_data/golden/dump/vac.txt"
          "${PROJECT_SOURCE_DIR}/test_data/golden/dump/mill.txt"
          "${PROJECT_SOURCE_DIR}/test_data/golden/dump/vac.json"
          "$<TARGET_FILE:scav>"
  TARGETS scav
)
# The loader driven from Python over ctypes, faking a network fetch. Needs the
# shared library, not an archive.
scav_add_functional_test(load
  DEPENDS "${PROJECT_SOURCE_DIR}/test_data/charts/vac.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/dock.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/led.scav"
          "$<TARGET_FILE:scav_shared>"
          "$<TARGET_FILE:scav>"
  TARGETS scav_shared scav
)
scav_add_functional_test(fmt
  DEPENDS ${scav_corpus_charts}
          "$<TARGET_FILE:scav>"
  TARGETS scav
)
scav_add_functional_test(check
  DEPENDS ${scav_corpus_charts}
          "$<TARGET_FILE:scav>"
  TARGETS scav
)
scav_add_functional_test(deps
  DEPENDS "${PROJECT_SOURCE_DIR}/test_data/charts/vac.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/dock.scav"
          "${PROJECT_SOURCE_DIR}/test_data/charts/led.scav"
          "$<TARGET_FILE:scav>"
  TARGETS scav
)
scav_add_functional_test(include_subset
  DEPENDS "${PROJECT_SOURCE_DIR}/src/layout/c_api.cpp"
          "${PROJECT_SOURCE_DIR}/src/layout/include/scav/scav_layout.h"
          "${PROJECT_SOURCE_DIR}/src/layout/include/scav/scav_layout_c.h"
          "${PROJECT_SOURCE_DIR}/src/layout/profile.cpp"
          "${PROJECT_SOURCE_DIR}/src/layout/profile_tests.cpp"
          "${PROJECT_SOURCE_DIR}/src/layout/router.cpp"
          "${PROJECT_SOURCE_DIR}/src/layout/router_tests.cpp"
          "${PROJECT_SOURCE_DIR}/src/layout/spaces.cpp"
          "${PROJECT_SOURCE_DIR}/src/layout/spaces_tests.cpp"
          "${PROJECT_SOURCE_DIR}/src/layout/split.cpp"
          "${PROJECT_SOURCE_DIR}/src/layout/split.h"
          "${PROJECT_SOURCE_DIR}/src/layout/split_tests.cpp"
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
