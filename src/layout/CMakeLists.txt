# The layout library: its input contracts, the decomposition, and the
# pipeline that writes the geometry columns.
scav_static_library(scavlayout
  c_api.cpp
  layout.cpp
  profile.cpp
  router.cpp
  spaces.cpp
  decompose.cpp
)
scav_install_library(scavlayout layout)
target_link_libraries(scavlayout PUBLIC scavcore)
target_link_libraries(scavlayout_testable PUBLIC scavcore_testable)

# The redistributable carries every library's C surface.
target_sources(scav_shared PRIVATE c_api.cpp)
target_link_libraries(scav_shared PRIVATE scavlayout)

if(SCAV_BUILD_TESTS)
  scav_tests(scav_layout_tests
    layout_tests.cpp
    profile_tests.cpp
    router_tests.cpp
    spaces_tests.cpp
    decompose_tests.cpp
  )
  target_link_libraries(scav_layout_tests PRIVATE scavlayout_testable)
  # A floor is asserted only where the number means anything: never under
  # instrumentation, never in Debug, and only when floors are asked for.
  set(scav_perf_light OFF)
  if(NOT SCAV_SANITIZER STREQUAL "NONE" OR SCAV_COVERAGE)
    set(scav_perf_light ON)
  endif()
  set(scav_light "$<OR:$<BOOL:${scav_perf_light}>,$<CONFIG:Debug>>")
  target_compile_definitions(scav_layout_tests PRIVATE
    "SCAV_PERF_ASSERT_FLOOR=$<IF:$<AND:$<BOOL:${SCAV_PERF_FLOORS}>,$<NOT:${scav_light}>>,1,0>")
endif()
