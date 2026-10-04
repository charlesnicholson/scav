# The per-compiler warning set, pinned in one place and PRIVATE to scav's own
# targets so a consumer inherits none of it.

include_guard(GLOBAL)

set(SCAV_WARNINGS_GNU_LIKE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wsign-conversion
  -Wshadow
  -Wold-style-cast
  -Wcast-align
  -Wcast-qual
  -Wdouble-promotion
  -Wfloat-equal
  -Wformat=2
  -Wimplicit-fallthrough
  -Wmissing-declarations  # catches a SCAV_INTERNAL that lost its prototype
  -Wnon-virtual-dtor
  -Woverloaded-virtual
  -Wredundant-decls
  -Wswitch-enum
  -Wundef
  -Wunused
  -Wwrite-strings
  -Wextra-semi
)

set(SCAV_WARNINGS_CLANG
  # Allows omitting designated fields, which value-initialize (e.g. scav_spaces);
  # newer clang enables this warning in -Wextra.
  -Wno-missing-designated-field-initializers
  -Wcomma
  -Wconditional-uninitialized
  -Wheader-hygiene
  -Winconsistent-missing-destructor-override
  -Wloop-analysis
  -Wnewline-eof
  -Wshadow-all
  -Wshift-sign-overflow
  -Wtautological-compare
  -Wthread-safety
  -Wunreachable-code-aggressive
  -Wunused-member-function
)

set(SCAV_WARNINGS_GCC
  # Disables gcc's missing-field warning for every initializer, designated included;
  # clang rows still check positional initializers.
  -Wno-missing-field-initializers
  -Warith-conversion
  -Wduplicated-branches
  -Wduplicated-cond
  -Wlogical-op
  -Wsuggest-override
  -Wuseless-cast
)

# /w1<n>: warning C<n> at level 1. /we<n>: as an error. /wd<n>: disabled.
set(SCAV_WARNINGS_MSVC
  /W4
  /permissive-           # without it MSVC accepts non-conforming C++
  /Zc:__cplusplus        # __cplusplus reports the standard in use
  /Zc:preprocessor       # conforming preprocessor
  /Zc:inline
  /volatile:iso
  /utf-8
  /w14242  # conversion, possible loss of data
  /w14254  # bitfield-to-smaller-type conversion
  /w14263  # member function does not override any base class virtual
  /w14265  # class has virtual functions but non-virtual destructor
  /w14287  # unsigned/negative constant mismatch
  /we4289  # loop control variable used outside the loop
  /w14296  # expression is always true/false
  /w14311  # pointer truncation
  /w14545  # expression before comma evaluates to a function missing an argument
  /w14546  # function call before comma missing argument list
  /w14547  # operator before comma has no effect
  /w14549  # operator before comma has no effect; did you mean operator?
  /w14555  # expression has no effect
  /wd4619  # #pragma warning: there is no warning number
  /w14640  # thread-unsafe static member initialization
  /w14826  # conversion is sign-extended, may cause unexpected behaviour
  /w14905  # wide string literal cast to LPSTR
  /w14906  # string literal cast to LPWSTR
  /w14928  # illegal copy-initialization; more than one user-defined conversion
)

function(scav_warnings_init)
  add_library(scav_warnings INTERFACE)

  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # clang-cl takes /W4 plus the clang warnings MSVC's headers pass.
      target_compile_options(scav_warnings INTERFACE
        /W4
        /utf-8
        -Wno-missing-designated-field-initializers  # /W4 enables it; see above
        -Wshift-sign-overflow
        -Wtautological-compare
        -Wthread-safety
      )
    else()
      target_compile_options(scav_warnings INTERFACE ${SCAV_WARNINGS_MSVC})
    endif()
    # Silences the CRT's deprecation warnings on standard <cstdio> functions.
    target_compile_definitions(scav_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(SCAV_WARNINGS_AS_ERRORS)
      target_compile_options(scav_warnings INTERFACE /WX)
    endif()
  else()
    target_compile_options(scav_warnings INTERFACE ${SCAV_WARNINGS_GNU_LIKE})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_compile_options(scav_warnings INTERFACE ${SCAV_WARNINGS_CLANG})
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      target_compile_options(scav_warnings INTERFACE ${SCAV_WARNINGS_GCC})
    endif()
    if(SCAV_WARNINGS_AS_ERRORS)
      target_compile_options(scav_warnings INTERFACE -Werror)
      if(APPLE)
        # Makes every Apple linker warning an error, e.g. a repeated archive.
        target_link_options(scav_warnings INTERFACE -Wl,-fatal_warnings)
      endif()
    endif()
  endif()

  # Turns off RTTI, and exceptions on GNU-style drivers; the code uses neither.
  add_library(scav_lang_rules INTERFACE)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(scav_lang_rules INTERFACE /GR-)
  else()
    target_compile_options(scav_lang_rules INTERFACE -fno-exceptions -fno-rtti)
  endif()
endfunction()
