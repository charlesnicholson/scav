set(SCAV_THREAD_BACKEND "AUTO" CACHE STRING
  "Thread shim backend: AUTO|NULL|PTHREAD|WIN32")
set_property(CACHE SCAV_THREAD_BACKEND PROPERTY STRINGS AUTO NULL PTHREAD WIN32)

set(scav_thread_backend "${SCAV_THREAD_BACKEND}")
if(scav_thread_backend STREQUAL "AUTO")
  if(WIN32)
    set(scav_thread_backend "WIN32")
  else()
    set(scav_thread_backend "PTHREAD")
  endif()
endif()
if(NOT scav_thread_backend MATCHES "^(NULL|PTHREAD|WIN32)$")
  message(FATAL_ERROR
    "SCAV_THREAD_BACKEND=${SCAV_THREAD_BACKEND}: expected AUTO, NULL, PTHREAD or WIN32")
endif()
# A cache entry, so the root scope that configures the package file can read it.
set(SCAV_THREAD_BACKEND_RESOLVED "${scav_thread_backend}" CACHE INTERNAL
  "SCAV_THREAD_BACKEND with AUTO resolved")
string(TOLOWER "${scav_thread_backend}" scav_thread_backend_file)

# The layout library: its input contracts, the decomposition, and the
# pipeline that writes the geometry columns.
scav_static_library(scavlayout
  # A src/-root primitive belonging to no library; layout compiles the backend
  # its build selected, and anything linking layout reaches the shim.
  "../scav_thread_${scav_thread_backend_file}.cpp"
  c_api.cpp
  layout.cpp
  profile.cpp
  router.cpp
  router_straight.cpp
  router_orthogonal.cpp
  spaces.cpp
  decompose.cpp
  order.cpp
  coords.cpp
  pack.cpp
  size.cpp
  route.cpp
  nudge.cpp
  label.cpp
  cost.cpp
)
scav_install_library(scavlayout layout)
target_link_libraries(scavlayout PUBLIC scavcore)
target_link_libraries(scavlayout_testable PUBLIC scavcore_testable)

# PUBLIC: a static archive carries no link flags of its own, so the consumer of
# either variant is where -pthread has to land.
if(scav_thread_backend STREQUAL "PTHREAD")
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(scavlayout PUBLIC Threads::Threads)
  target_link_libraries(scavlayout_testable PUBLIC Threads::Threads)
endif()

# The redistributable carries every library's C surface.
target_sources(scav_shared PRIVATE c_api.cpp)
target_link_libraries(scav_shared PRIVATE scavlayout)

if(SCAV_BUILD_TESTS)
  scav_tests(scav_layout_tests
    c_api_tests.cpp
    layout_tests.cpp
    bench_tests.cpp
    gauntlet_tests.cpp
    # Suite-level, beside the fixture both it and the unit tests drive (3.3).
    tests/determinism_tests.cpp
    tests/test_synth.cpp
    profile_tests.cpp
    router_tests.cpp
    router_orthogonal_tests.cpp
    spaces_tests.cpp
    decompose_tests.cpp
    order_tests.cpp
    coords_tests.cpp
    geom_tests.cpp
    pack_tests.cpp
    size_tests.cpp
    route_tests.cpp
    nudge_tests.cpp
    label_tests.cpp
    cost_tests.cpp
    # The src/-root primitives standing in for the standard library's: they
    # belong to no subsystem, so their tests sit beside them.
    ../scav_rnd_tests.cpp
    ../scav_shard_tests.cpp
    ../scav_thread_tests.cpp
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
