# Coverage instrumentation. The gate fails the build on any production file with
# zero executed lines.

include_guard(GLOBAL)

function(scav_coverage_init)
  add_library(scav_coverage INTERFACE)

  if(NOT SCAV_COVERAGE)
    return()
  endif()

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
