# The draw library: font metrics, the DrawList render IR, the helper layer, and the
# reference builder, which reads geometry columns by name.

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

# Adds layout's public headers for their PODs and inline functions;
# scav_check_layering forbids linking layout.
foreach(target scavdraw scavdraw_testable)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/layout/include>")
endforeach()

# Adds the C surface to the redistributable shared library, which links the scavdraw
# archive through scavsvg.
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
