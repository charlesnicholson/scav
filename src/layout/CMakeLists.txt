# Space tables, profile, and the router registry -- what layout consumes; the
# phases land beside them.
scav_static_library(scavlayout
  c_api.cpp
  layout.cpp
  profile.cpp
  router.cpp
  spaces.cpp
  split.cpp
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
    split_tests.cpp
  )
  target_link_libraries(scav_layout_tests PRIVATE scavlayout_testable)
endif()
