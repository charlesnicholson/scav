scav_static_library(scavcore
  lang/diagnostics.cpp
  lang/lexer.cpp
  lang/source_text.cpp
  lang/syntax.cpp
  lang/parser.cpp
  lang/unicode_nfc.cpp
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
  # synth_document.cpp is harness-only: scav ships no chart generator.
  scav_tests(scav_core_tests
    lang/corpus_tests.cpp
    lang/diagnostics_tests.cpp
    lang/fuzz_tests.cpp
    lang/lexer_tests.cpp
    lang/source_text_tests.cpp
    lang/syntax_tests.cpp
    lang/parser_tests.cpp
    lang/perf_tests.cpp
    lang/synth_document.cpp
    lang/unicode_nfc_tests.cpp
    scav_core_tests.cpp
  )
  target_link_libraries(scav_core_tests PRIVATE scavcore_testable)

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
  target_compile_definitions(scav_core_tests PRIVATE
    "SCAV_PERF_INPUT_BYTES=$<IF:${scav_light},4194304,104857600>"
    "SCAV_PERF_ASSERT_FLOOR=$<IF:${scav_light},0,1>"
  )
endif()
