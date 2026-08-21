# Three subsystems: `lang` is bytes to statements and knows nothing about
# entities, `model` is the entity arrays, `load` composes the two.
scav_static_library(scavcore
  # A src/-root primitive belonging to no library; core compiles it because core
  # is its only consumer, and anything linking core reaches it.
  ../scav_xxhash.cpp
  # Core-wide, owned by no subsystem: every layer below produces diagnostics,
  # and c_api.cpp is the C projection of the whole public header.
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

# The redistributable: one shared object, with the static archives a build-time
# decomposition. Each later library contributes its own c_api.cpp.
add_library(scav_shared SHARED c_api.cpp)
scav_settings(scav_shared)
target_link_libraries(scav_shared PRIVATE scavcore)
target_include_directories(scav_shared PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
set_target_properties(scav_shared PROPERTIES
  OUTPUT_NAME scav
  # OUTPUT_NAME would land this on the scav executable's own PDB, and the two
  # links race for the write. The PDB alone keeps the target name.
  PDB_NAME scav_shared
  # MSVC exports nothing without either a .def file or this.
  WINDOWS_EXPORT_ALL_SYMBOLS TRUE
)

# Not in ALL and not a dependency of unicode_nfc.cpp: the .inc files are
# committed, so a build needs neither the network nor Python.
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
  # Unit tests pair with their subject; everything suite-level lives in tests/,
  # named by its class beside the fixtures it drives.
  scav_tests(scav_core_tests
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
    # The src/-root primitives standing in for the standard library's: they
    # belong to no subsystem, so their tests sit beside them.
    ../scav_int_tests.cpp
    ../scav_lookup_map_tests.cpp
    ../scav_stable_sort_tests.cpp
    ../scav_xxhash_tests.cpp
  )
  target_link_libraries(scav_core_tests PRIVATE scavcore_testable)
  # A harness reporting nothing looks identical to one where all is well.
  scav_expect_test_failure(scav_core_tests "*deliberate failure*")

  # Floors, not times. Under instrumentation the numbers describe the
  # instrumentation, so those configs shrink the input and skip the floor.
  set(scav_perf_light OFF)
  if(NOT SCAV_SANITIZER STREQUAL "NONE" OR SCAV_COVERAGE)
    set(scav_perf_light ON)
  endif()
  # $<CONFIG:Debug> as well, because CMAKE_BUILD_TYPE is empty on the
  # multi-config generators.
  set(scav_light "$<OR:$<BOOL:${scav_perf_light}>,$<CONFIG:Debug>>")
  # Instrumentation makes a timing describe the instrumentation, so a light config
  # never asserts one even when floors are asked for.
  set(scav_floors "$<AND:$<BOOL:${SCAV_PERF_FLOORS}>,$<NOT:${scav_light}>>")
  # 100 MB is for the timing floors, which assert only on a named machine; 8 MB
  # exposes a quadratic to the scaling and memory bounds in seconds.
  # A rate needs a named machine; a ratio does not, and instrumentation scales
  # both of its sides. So scaling asserts wherever the numbers mean anything.
  target_compile_definitions(scav_core_tests PRIVATE
    "SCAV_PERF_INPUT_BYTES=$<IF:${scav_light},4194304,$<IF:${scav_floors},104857600,8388608>>"
    "SCAV_PERF_ASSERT_FLOOR=$<IF:${scav_floors},1,0>"
    "SCAV_PERF_ASSERT_SCALING=$<IF:${scav_light},0,1>"
  )
endif()
