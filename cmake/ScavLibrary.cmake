include_guard(GLOBAL)

# What each library may link. Notably draw may not link layout: a builder reads
# geometry columns and does not care who wrote them.
set(SCAV_LIBRARY_DEPS_core "")
set(SCAV_LIBRARY_DEPS_layout core)
set(SCAV_LIBRARY_DEPS_draw core)
set(SCAV_LIBRARY_DEPS_svg draw)
set(SCAV_LIBRARY_DEPS_imgui draw)

# scav_settings(<target>)
#
# scav's warning set, sanitizer, coverage instrumentation and include paths.
function(scav_settings target)
  # BUILD_INTERFACE so an exported archive names none of them. COMPILE_ONLY would
  # be the more precise relationship but survives into the export by name.
  target_link_libraries(${target} PRIVATE
    $<BUILD_INTERFACE:scav_warnings>
    $<BUILD_INTERFACE:scav_lang_rules>
    $<BUILD_INTERFACE:scav_sanitizer>
    $<BUILD_INTERFACE:scav_coverage>
  )
  # The whole public/private boundary: a library's own sources and tests reach
  # `src/<lib>/...`, and nothing that merely links it can.
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  # The cross-library vocabulary. A library's own API is added below.
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  )
  target_compile_features(${target} PUBLIC cxx_std_20)
endfunction()

# scav_static_library(<name> <source>...)
#
# Two archives from one source list: <name> ships, and <name>_testable carries
# SCAV_TESTING so a test can link a function that otherwise has internal linkage.
# Link them, install them and test them yourself.
function(scav_static_library name)
  if(NOT ARGN)
    message(FATAL_ERROR "scav_static_library(${name}): no sources")
  endif()

  # The library's API, and the only thing a consumer can reach: everything else
  # under `src/<lib>/` needs an -I that only this library and its tests get.
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
  # clang-tidy sees internal linkage as internal only where SCAV_TESTING is off.
  # Anywhere else it reports a cross-TU fact it cannot see from inside one TU.
  set_target_properties(${untidied} PROPERTIES CXX_CLANG_TIDY "")

  set_property(GLOBAL APPEND PROPERTY SCAV_LIBRARIES ${name})
  # The coverage gate needs what was *supposed* to be tested: a file no test links
  # is absent from the report entirely, so a report-driven check would miss it.
  foreach(source IN LISTS ARGN)
    cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      NORMALIZE OUTPUT_VARIABLE absolute)
    set_property(GLOBAL APPEND PROPERTY SCAV_PRODUCTION_SOURCES "${absolute}")
  endforeach()
endfunction()

# scav_install_library(<target> <exported name>)
#
# An ALIAS is not exported, so EXPORT_NAME is what lets an installed tree and an
# add_subdirectory tree spell the dependency the same way.
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

# scav_check_layering()
#
# Reads what the targets actually link rather than what a wrapper was told, so an
# ordinary target_link_libraries cannot slip a forbidden edge past it.
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
