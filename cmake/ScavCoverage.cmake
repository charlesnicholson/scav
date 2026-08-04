# An untested file fails the build. That is not a percentage target: zero executed
# lines means nobody wrote a test, whatever the aggregate says.

include_guard(GLOBAL)

function(scav_coverage_init)
  add_library(scav_coverage INTERFACE)

  if(NOT SCAV_COVERAGE)
    return()
  endif()

  # Source-based instrumentation, not gcov: gcov reports no branch regions for the
  # per-file summary the gate reads.
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "SCAV_COVERAGE needs clang: the gate reads llvm-cov's per-file branch "
      "summary, and gcov reports no branch regions for it.")
  endif()

  target_compile_options(scav_coverage INTERFACE
    -fprofile-instr-generate
    -fcoverage-mapping
  )
  target_link_options(scav_coverage INTERFACE -fprofile-instr-generate)
endfunction()
