include_guard(GLOBAL)

# SCAV_LIBRARY_DEPS_<lib>: the scav libraries <lib> may link.
set(SCAV_LIBRARY_DEPS_core "")
set(SCAV_LIBRARY_DEPS_layout core)
set(SCAV_LIBRARY_DEPS_draw core)
set(SCAV_LIBRARY_DEPS_svg draw)
set(SCAV_LIBRARY_DEPS_imgui draw)

# scav_settings(<target>) -- warning set, sanitizer, coverage and include paths.
function(scav_settings target)
  # BUILD_INTERFACE keeps these settings targets out of the exported link interface.
  target_link_libraries(${target} PRIVATE
    $<BUILD_INTERFACE:scav_warnings>
    $<BUILD_INTERFACE:scav_lang_rules>
    $<BUILD_INTERFACE:scav_sanitizer>
    $<BUILD_INTERFACE:scav_coverage>
  )
  # Internal headers, `src/<lib>/...`, visible to this target only.
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  # The cross-library vocabulary. A library's own API is added below.
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  )
  target_compile_features(${target} PUBLIC cxx_std_20)
  if(CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF")
    # One section per function and datum, which --gc-sections drops individually.
    target_compile_options(${target} PRIVATE
      "$<$<CONFIG:Release>:-ffunction-sections;-fdata-sections>")
  endif()
endfunction()

# scav_optimize_for_size(<target> [SOURCES <file>...]) -- Release -Oz for <target>
# and <target>_testable, or for SOURCES in its directory; GNU-style drivers only.
function(scav_optimize_for_size target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES")
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    return()
  endif()
  if(arg_SOURCES)
    set_source_files_properties(${arg_SOURCES} TARGET_DIRECTORY ${target} PROPERTIES
      COMPILE_OPTIONS "$<$<CONFIG:Release>:-Oz>")
    return()
  endif()
  foreach(variant ${target} ${target}_testable)
    if(TARGET ${variant})
      target_compile_options(${variant} PRIVATE "$<$<CONFIG:Release>:-Oz>")
    endif()
  endforeach()
endfunction()

# scav_dead_code_strip(<target>) -- a Release link drops unreferenced code and, for a
# Mach-O executable, exports nothing. MSVC links Release with /OPT:REF,ICF by default.
function(scav_dead_code_strip target)
  if(CMAKE_EXECUTABLE_FORMAT STREQUAL "MACHO")
    set(flags "-dead_strip")
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
      string(APPEND flags ",-no_exported_symbols")
    endif()
    target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:LINKER:${flags}>")
  elseif(CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF")
    target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:LINKER:--gc-sections>")
  endif()
endfunction()

# scav_export_c_abi(<shared library> <abi json>) -- exports exactly the golden's
# functions: a .def on Windows, an export list on Mach-O, a version script on ELF.
function(scav_export_c_abi target abi_json)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${abi_json}")
  file(READ "${abi_json}" abi)
  set(names "")
  string(JSON header_count LENGTH "${abi}" headers)
  math(EXPR header_last "${header_count} - 1")
  foreach(h RANGE ${header_last})
    string(JSON function_count LENGTH "${abi}" headers ${h} functions)
    if(function_count EQUAL 0)
      continue()
    endif()
    math(EXPR function_last "${function_count} - 1")
    foreach(f RANGE ${function_last})
      string(JSON name GET "${abi}" headers ${h} functions ${f} name)
      list(APPEND names "${name}")
    endforeach()
  endforeach()

  set(base "${CMAKE_CURRENT_BINARY_DIR}/${target}_exports")
  if(WIN32)
    list(JOIN names "\n  " body)
    file(CONFIGURE OUTPUT "${base}.def" CONTENT "EXPORTS\n  ${body}\n" @ONLY)
    target_sources(${target} PRIVATE "${base}.def")
  elseif(CMAKE_EXECUTABLE_FORMAT STREQUAL "MACHO")
    list(TRANSFORM names PREPEND "_")
    list(JOIN names "\n" body)
    file(CONFIGURE OUTPUT "${base}.txt" CONTENT "${body}\n" @ONLY)
    target_link_options(${target} PRIVATE "LINKER:-exported_symbols_list,${base}.txt")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${base}.txt")
  elseif(CMAKE_EXECUTABLE_FORMAT STREQUAL "ELF")
    list(JOIN names ";\n    " body)
    file(CONFIGURE OUTPUT "${base}.map"
      CONTENT "{\n  global:\n    ${body};\n  local: *;\n};\n" @ONLY)
    target_link_options(${target} PRIVATE "LINKER:--version-script=${base}.map")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${base}.map")
  else()
    message(FATAL_ERROR
      "scav_export_c_abi: no export list for ${CMAKE_EXECUTABLE_FORMAT}")
  endif()
endfunction()

# scav_static_library(<name> <source>...) -- two archives from one source list.
# <name>_testable carries SCAV_TESTING, dropping internal linkage.
function(scav_static_library name)
  if(NOT ARGN)
    message(FATAL_ERROR "scav_static_library(${name}): no sources")
  endif()

  # include/ is the public API, the only part of `src/<lib>/` that dependents reach.
  set(public_include "${CMAKE_CURRENT_SOURCE_DIR}/include")
  if(NOT IS_DIRECTORY "${public_include}")
    message(FATAL_ERROR
      "scav_static_library(${name}): no ${public_include}. Every library declares "
      "its API in include/scav/, or it has no way to be consumed.")
  endif()

  foreach(target ${name} ${name}_testable)
    add_library(${target} STATIC ${ARGN})
    scav_settings(${target})
    target_include_directories(${target} PUBLIC
      "$<BUILD_INTERFACE:${public_include}>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
  endforeach()

  target_compile_definitions(${name}_testable PUBLIC SCAV_TESTING)
  set(untidied ${name}_testable)
  if(SCAV_TESTING)
    target_compile_definitions(${name} PUBLIC SCAV_TESTING)
    list(APPEND untidied ${name})
  endif()
  # Disables clang-tidy on targets built with SCAV_TESTING, where internal functions
  # have external linkage.
  set_target_properties(${untidied} PROPERTIES CXX_CLANG_TIDY "")

  set_property(GLOBAL APPEND PROPERTY SCAV_LIBRARIES ${name})
  # Records every production source for the coverage gate's list.
  foreach(source IN LISTS ARGN)
    cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      NORMALIZE OUTPUT_VARIABLE absolute)
    set_property(GLOBAL APPEND PROPERTY SCAV_PRODUCTION_SOURCES "${absolute}")
  endforeach()
endfunction()

# scav_install_library(<target> <exported name>) -- installs <target> as
# scav::<exported name>, the name its build-tree ALIAS uses.
function(scav_install_library target exported)
  add_library(scav::${exported} ALIAS ${target})
  set_target_properties(${target} PROPERTIES EXPORT_NAME ${exported})
  install(TARGETS ${target} EXPORT scavTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  )
  # Only include/, so the install tree has the same boundary the build tree does.
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/scav"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h"
  )
endfunction()

# scav_check_layering() -- fails configure when a library or its _testable twin
# links a scav library outside its SCAV_LIBRARY_DEPS list.
function(scav_check_layering)
  get_property(libraries GLOBAL PROPERTY SCAV_LIBRARIES)
  foreach(library IN LISTS libraries)
    string(REGEX REPLACE "^scav" "" short "${library}")
    if(NOT DEFINED SCAV_LIBRARY_DEPS_${short})
      message(FATAL_ERROR
        "scav_check_layering: '${library}' declares no permitted dependencies. Add "
        "SCAV_LIBRARY_DEPS_${short} with the libraries it is allowed to link.")
    endif()
    foreach(variant ${library} ${library}_testable)
      get_target_property(links ${variant} LINK_LIBRARIES)
      foreach(link IN LISTS links)
        string(REGEX REPLACE "_testable$" "" dep "${link}")
        string(REGEX REPLACE "^scav" "" dep_short "${dep}")
        if(dep IN_LIST libraries AND NOT dep_short IN_LIST SCAV_LIBRARY_DEPS_${short})
          message(FATAL_ERROR
            "${variant} links ${link}, which ${library} is not allowed to depend "
            "on. Permitted: '${SCAV_LIBRARY_DEPS_${short}}'.")
        endif()
      endforeach()
    endforeach()
  endforeach()
endfunction()
