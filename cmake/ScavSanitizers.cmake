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

  # Both Windows toolchains take the MSVC driver's spellings: /Zi for the debug
  # info a report needs to symbolize, /Oy- for the frame pointers, and no
  # incremental linking. The GNU spellings below are ignored there, and an ignored
  # argument is itself a warning.
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(flags /Zi /Oy-)
    target_link_options(scav_sanitizer INTERFACE /INCREMENTAL:NO)
    if(san STREQUAL "ASAN")
      list(APPEND flags /fsanitize=address)
      if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # cl embeds a defaultlib directive in the objects; clang-cl does not, so
        # the runtime has to be named at link or every __asan_* is undefined.
        target_link_options(scav_sanitizer INTERFACE /fsanitize=address)
      endif()
    elseif(san STREQUAL "UBSAN")
      # Trap rather than diagnose: the diagnosing runtime is built against one CRT
      # and mismatches ours at link. A trap needs no runtime, and a test that dies
      # on undefined behaviour is what the row is for.
      list(APPEND flags -fsanitize=undefined -fsanitize-trap=undefined)
    endif()
    target_compile_options(scav_sanitizer INTERFACE ${flags})
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
