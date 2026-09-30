# The draw library: font metrics, the DrawList render IR, the helper layer, and
# the reference builder. No dependency on layout -- a builder reads geometry
# columns by name and does not care who wrote them.

# The bundled font's metrics are bundled_font_table.h, generated from the TTF by
# gen_font_table.py; the TTF itself is installed beside the CLI, not compiled in.
scav_static_library(scavdraw
  c_api.cpp
  metrics.cpp
  drawlist.cpp
  helpers.cpp
  builder.cpp
)

scav_install_library(scavdraw draw)
scav_optimize_for_size(scavdraw)
target_link_libraries(scavdraw PUBLIC scavcore)
target_link_libraries(scavdraw_testable PUBLIC scavcore_testable)

# Layout's public headers are POD vocabulary the measurement pass needs; the
# link edge stays absent, which is what scav_check_layering enforces.
foreach(target scavdraw scavdraw_testable)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/layout/include>")
endforeach()

# The redistributable carries every library's C surface. The archive arrives
# transitively -- svg links draw PUBLIC, and a direct link here would repeat it
# on the link line.
target_sources(scav_shared PRIVATE c_api.cpp)

if(SCAV_BUILD_TESTS)
  scav_tests(scav_draw_tests
    c_api_tests.cpp
    metrics_tests.cpp
    drawlist_tests.cpp
    helpers_tests.cpp
    builder_tests.cpp
    tests/functional_drawlist_tests.cpp
  )
  target_link_libraries(scav_draw_tests PRIVATE scavdraw_testable scavlayout_testable)
endif()
