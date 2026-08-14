scav_static_library(scavcore
  lang/diagnostics.cpp
  lang/lexer.cpp
  lang/source_text.cpp
  lang/syntax.cpp
  lang/parser.cpp
  lang/unicode_nfc.cpp
  model/builder.cpp
  model/chart.cpp
  model/columns.cpp
  model/lower.cpp
  model/resolve.cpp
  model/validate.cpp
)
scav_install_library(scavcore core)

# The generator lives with the code it generates. Not in ALL and not a dependency
# of unicode_nfc.cpp: the .inc files are committed, so a build needs neither the
# network nor Python, and the Unicode version is not a build's to move.
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
  # Unit tests pair with their subject -- foo_tests.cpp beside foo.cpp (or
  # foo.h). Everything suite-level lives in tests/, named by its class:
  # functional_*, fuzz_*, perf_*, beside the shared fixtures they drive.
  scav_tests(scav_core_tests
    lang/diagnostics_tests.cpp
    lang/lexer_tests.cpp
    lang/parser_tests.cpp
    lang/source_text_tests.cpp
    lang/syntax_tests.cpp
    lang/unicode_nfc_tests.cpp
    model/builder_tests.cpp
    model/chart_tests.cpp
    model/columns_tests.cpp
    model/lower_tests.cpp
    model/resolve_tests.cpp
    model/validate_tests.cpp
    tests/functional_corpus_tests.cpp
    tests/functional_exit_gate_tests.cpp
    tests/fuzz_lang_tests.cpp
    tests/fuzz_model_tests.cpp
    tests/perf_lang_tests.cpp
    tests/perf_model_tests.cpp
    tests/test_synth_document.cpp
    scav_core_tests.cpp
    ../scav_sort_tests.cpp
  )
  target_link_libraries(scav_core_tests PRIVATE scavcore_testable)
  # The deliberately-failing case (PB): a harness that reports nothing looks
  # identical to one where everything passes.
  scav_expect_test_failure(scav_core_tests "*deliberate failure*")

  # Performance tests assert floors, not times. Under instrumentation the numbers
  # describe the instrumentation, so those configs shrink the input and skip the
  # floor.
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
  # Input sizes follow what the run can prove. The 100 MB corpus exists for the
  # timing floors, which only assert on a named machine (SCAV_PERF_FLOORS); a
  # run without them keeps the scaling and memory assertions, and 8 MB is ample
  # to expose a quadratic to those while keeping the suite seconds long.
  target_compile_definitions(scav_core_tests PRIVATE
    "SCAV_PERF_INPUT_BYTES=$<IF:${scav_light},4194304,$<IF:${scav_floors},104857600,8388608>>"
    "SCAV_PERF_ASSERT_FLOOR=$<IF:${scav_floors},1,0>"
  )
endif()
