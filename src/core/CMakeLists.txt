# Three subsystems: `lang` turns bytes into statements, `model` holds the entity
# arrays, and `load` composes the two.
scav_static_library(scavcore
  # src/-root primitives belonging to no library; core compiles them, so anything
  # linking core reaches them.
  ../scav_xxhash.cpp
  # Core-wide sources outside the three subsystems: the C API and the diagnostics table.
  c_api.cpp
  diagnostics.cpp
  lang/lexer.cpp
  lang/source_text.cpp
  lang/syntax.cpp
  lang/parser.cpp
  lang/printer.cpp
  lang/unicode_nfc.cpp
  load/load.cpp
  load/load_file.cpp
  load/path.cpp
  model/builder.cpp
  model/chart.cpp
  model/columns.cpp
  model/hash.cpp
  model/lower.cpp
  model/resolve.cpp
  model/validate.cpp
)
scav_install_library(scavcore core)
scav_optimize_for_size(scavcore)

# The redistributable shared library, `scav`. Each later library adds its c_api.cpp
# here and links in; scavcore comes transitively through their PUBLIC links.
add_library(scav_shared SHARED c_api.cpp)
scav_settings(scav_shared)
target_include_directories(scav_shared PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
set_target_properties(scav_shared PROPERTIES
  OUTPUT_NAME scav
  # The PDB keeps the target name, distinct from the scav executable's PDB.
  PDB_NAME scav_shared
)
scav_export_c_abi(scav_shared "${PROJECT_SOURCE_DIR}/abi/scav_abi.json")
scav_dead_code_strip(scav_shared)
# The build tree keeps its symbols; `cmake --install --strip` removes them.
install(TARGETS scav_shared)

# Manual target: regenerates the committed NFC .inc tables, downloading the UCD.
find_package(Python3 3.14 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_custom_target(scav_core_unicode_tables
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/lang/gen_unicode_tables.py" --download
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lang"
    COMMENT "regenerating the Unicode NFC tables from the UCD"
    USES_TERMINAL
    VERBATIM
  )
endif()

if(SCAV_BUILD_TESTS)
  # Unit tests sit beside their subject; suite-level tests live in tests/, prefixed
  # by class (functional, fuzz, perf).
  scav_tests(scav_core_tests
    TIMING "perf:*"
    c_api_tests.cpp
    diagnostics_tests.cpp
    lang/lexer_tests.cpp
    lang/parser_tests.cpp
    lang/printer_tests.cpp
    lang/source_text_tests.cpp
    lang/syntax_tests.cpp
    lang/unicode_nfc_tests.cpp
    load/load_file_tests.cpp
    load/load_tests.cpp
    load/path_tests.cpp
    model/builder_tests.cpp
    model/chart_tests.cpp
    model/columns_tests.cpp
    model/hash_tests.cpp
    model/lower_tests.cpp
    model/resolve_tests.cpp
    model/validate_tests.cpp
    tests/functional_corpus_tests.cpp
    tests/functional_exit_gate_tests.cpp
    tests/functional_printer_tests.cpp
    tests/fuzz_lang_tests.cpp
    tests/fuzz_load_tests.cpp
    tests/fuzz_model_tests.cpp
    tests/perf_lang_tests.cpp
    tests/perf_load_tests.cpp
    tests/perf_model_tests.cpp
    tests/test_synth.cpp
    scav_core_tests.cpp
    # Tests for the src/-root primitives that replace standard-library facilities.
    ../scav_int_tests.cpp
    ../scav_hash_map_tests.cpp
    ../scav_inflate.cpp  # also compiled into the inflate driver and the CLI
    ../scav_inflate_tests.cpp
    ../scav_stable_sort_tests.cpp
    ../scav_xxhash_tests.cpp
  )
  target_link_libraries(scav_core_tests PRIVATE scavcore_testable)

  # Decodes reference-encoder streams for functional_tests/test_inflate.py.
  add_executable(scav_inflate_driver ../scav_inflate_driver.cpp ../scav_inflate.cpp)
  scav_settings(scav_inflate_driver)
  target_link_libraries(scav_inflate_driver PRIVATE scavcore)
  # Checks that the test binary reports a deliberately failing test as a failure.
  scav_expect_test_failure(scav_core_tests "*deliberate failure*")

  # Sanitizer and coverage builds are light: a smaller perf input and no MiB/s floors.
  set(scav_perf_light OFF)
  if(NOT SCAV_SANITIZER STREQUAL "NONE" OR SCAV_COVERAGE)
    set(scav_perf_light ON)
  endif()
  # Debug is light too, on single- and multi-config generators.
  set(scav_light "$<OR:$<BOOL:${scav_perf_light}>,$<CONFIG:Debug>>")
  set(scav_floors "$<AND:$<BOOL:${SCAV_PERF_FLOORS}>,$<NOT:${scav_light}>>")
  # Input bytes: 4 MiB light, 100 MiB with timing floors, 8 MiB otherwise.
  target_compile_definitions(scav_core_tests PRIVATE
    "SCAV_PERF_INPUT_BYTES=$<IF:${scav_light},4194304,$<IF:${scav_floors},104857600,8388608>>"
    "SCAV_PERF_ASSERT_FLOOR=$<IF:${scav_floors},1,0>"
    "SCAV_PERF_ASSERT_SCALING=$<IF:${scav_light},0,1>"
  )
endif()
