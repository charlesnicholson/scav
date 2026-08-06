scav_static_library(scavcore
  lang/diagnostics.cpp
  lang/lexer.cpp
  lang/source_text.cpp
  lang/syntax_tree.cpp
  lang/parser.cpp
  lang/unicode_nfc.cpp
  model/interner.cpp
  model/string_pool.cpp
)
scav_install_library(scavcore core)

# The NFC tables are generated from the Unicode UCD by a script that lives with
# the code it generates, because nothing outside this library has a use for it.
#
# Deliberately NOT in ALL, and deliberately not a dependency of unicode_nfc.cpp:
# the generated .inc files are committed, so an ordinary build needs neither the
# network nor Python, and the Unicode version they encode is a determinism input
# (PRD 6) rather than something a build is free to move underneath you.
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
  # synth.cpp is harness-only and is deliberately not in the library: PRD 3.2
  # rejects a `gen` verb, so the synthetic generator has no shipping home.
  scav_tests(scav_core_tests
    lang/corpus_tests.cpp
    lang/fuzz_tests.cpp
    lang/lexer_tests.cpp
    lang/source_text_tests.cpp
    lang/parser_tests.cpp
    lang/perf_tests.cpp
    lang/synth_document.cpp
    lang/unicode_nfc_tests.cpp
    model/model_tests.cpp
  )
  target_link_libraries(scav_core_tests PRIVATE scavcore_testable)

  # Performance tests assert floors, not times (PRD 5), so they are not a matrix
  # row and not a golden. Under a sanitizer, coverage instrumentation or -O0 the
  # numbers describe the instrumentation rather than the code, so those configs
  # run a smaller input for correctness and skip the floor entirely.
  set(scav_perf_light OFF)
  if(NOT SCAV_SANITIZER STREQUAL "NONE" OR SCAV_COVERAGE)
    set(scav_perf_light ON)
  endif()
  # $<CONFIG:Debug> as well, because CMAKE_BUILD_TYPE is empty on the
  # multi-config generators.
  set(scav_light "$<OR:$<BOOL:${scav_perf_light}>,$<CONFIG:Debug>>")
  target_compile_definitions(scav_core_tests PRIVATE
    "SCAV_PERF_INPUT_BYTES=$<IF:${scav_light},4194304,104857600>"
    "SCAV_PERF_ASSERT_FLOOR=$<IF:${scav_light},0,1>"
  )
endif()
