# One mutually-exclusive enum rather than booleans: ASan and TSan cannot coexist,
# so a boolean pair would invite an unbuildable combination.

include_guard(GLOBAL)

# Availability is a toolchain fact, not a preference. Anything unavailable is a
# configure error rather than a build that appears to work and reports nothing.
function(scav_sanitizer_check name)
  set(msvc_frontend FALSE)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(msvc_frontend TRUE)
  endif()

  if(name STREQUAL "UBSAN" AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR
      "SCAV_SANITIZER=UBSAN: MSVC has no UndefinedBehaviorSanitizer. Use the "
      "clang triple for the UBSan row.")
  endif()

  if(name STREQUAL "TSAN" AND (WIN32 OR msvc_frontend))
    message(FATAL_ERROR
      "SCAV_SANITIZER=TSAN: no ThreadSanitizer exists for either Windows "
      "toolchain. TSan rows are macOS and Linux only.")
  endif()

  if(name STREQUAL "MSAN")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR
        "SCAV_SANITIZER=MSAN: MemorySanitizer needs an instrumented libc++ and "
        "is therefore Linux/clang only.")
    endif()
    if(SCAV_MSAN_LIBCXX_DIR STREQUAL "")
      message(FATAL_ERROR
        "SCAV_SANITIZER=MSAN needs SCAV_MSAN_LIBCXX_DIR. Without an "
        "instrumented libc++, MSan reports false positives from uninstrumented "
        "standard-library code for the life of the project. Build one with "
        "`tools/msan_libcxx.py`, which prints the prefix to pass here.")
    endif()
    if(NOT EXISTS "${SCAV_MSAN_LIBCXX_DIR}/include/c++/v1/vector")
      message(FATAL_ERROR
        "SCAV_MSAN_LIBCXX_DIR=${SCAV_MSAN_LIBCXX_DIR} has no "
        "include/c++/v1/vector, so it is not a libc++ install prefix.")
    endif()
  endif()
endfunction()

function(scav_sanitizers_init)
  add_library(scav_sanitizer INTERFACE)

  string(TOUPPER "${SCAV_SANITIZER}" san)
  if(NOT san MATCHES "^(NONE|ASAN|UBSAN|TSAN|MSAN)$")
    message(FATAL_ERROR
      "SCAV_SANITIZER=${SCAV_SANITIZER} is not one of NONE ASAN UBSAN TSAN MSAN")
  endif()

  if(san STREQUAL "NONE")
    return()
  endif()

  scav_sanitizer_check("${san}")

  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # MSVC's only sanitizer; it needs debug info to symbolize and is incompatible
    # with incremental linking.
    target_compile_options(scav_sanitizer INTERFACE /fsanitize=address /Zi)
    target_link_options(scav_sanitizer INTERFACE /INCREMENTAL:NO)
    return()
  endif()

  # Frame pointers, so a report has a readable stack.
  set(common -fno-omit-frame-pointer -fno-optimize-sibling-calls -g)

  if(san STREQUAL "ASAN")
    set(flags -fsanitize=address ${common})
  elseif(san STREQUAL "UBSAN")
    # Signed overflow stays undefined rather than reaching for -fwrapv, which MSVC
    # has no equivalent for; -fno-sanitize-recover makes a finding fail the test.
    set(flags
      -fsanitize=undefined
      -fsanitize=signed-integer-overflow
      -fsanitize=shift
      -fsanitize=integer-divide-by-zero
      -fno-sanitize-recover=all
      ${common}
    )
  elseif(san STREQUAL "TSAN")
    set(flags -fsanitize=thread ${common})
  elseif(san STREQUAL "MSAN")
    set(flags
      -fsanitize=memory
      -fsanitize-memory-track-origins=2
      -fno-sanitize-recover=all
      ${common}
    )
  endif()

  target_compile_options(scav_sanitizer INTERFACE ${flags})
  target_link_options(scav_sanitizer INTERFACE ${flags})

  if(san STREQUAL "MSAN")
    # An uninstrumented libc++ is precisely what produces the false positives, so
    # replace it wholesale.
    target_compile_options(scav_sanitizer INTERFACE
      -nostdinc++
      "-isystem${SCAV_MSAN_LIBCXX_DIR}/include/c++/v1"
    )
    target_link_options(scav_sanitizer INTERFACE
      -nostdlib++
      "-L${SCAV_MSAN_LIBCXX_DIR}/lib"
      "-Wl,-rpath,${SCAV_MSAN_LIBCXX_DIR}/lib"
    )
    target_link_libraries(scav_sanitizer INTERFACE c++ c++abi)
  endif()

  set(SCAV_SANITIZER_ACTIVE "${san}" CACHE INTERNAL "resolved sanitizer")
endfunction()
