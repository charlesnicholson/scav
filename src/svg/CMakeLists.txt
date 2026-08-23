# The reference SVG backend: one DrawList in, one document out. It links draw
# for the DrawList and the metrics its textLength comes from, and nothing else.

scav_static_library(scavsvg
  c_api.cpp
  svg.cpp
)
scav_install_library(scavsvg svg)
target_link_libraries(scavsvg PUBLIC scavdraw)
target_link_libraries(scavsvg_testable PUBLIC scavdraw_testable)

# The redistributable carries every library's C surface.
target_sources(scav_shared PRIVATE c_api.cpp)
target_link_libraries(scav_shared PRIVATE scavsvg)

if(SCAV_BUILD_TESTS)
  scav_tests(scav_svg_tests
    svg_tests.cpp
    tests/functional_svg_tests.cpp
  )
  target_link_libraries(scav_svg_tests PRIVATE scavsvg_testable scavlayout_testable)
endif()
