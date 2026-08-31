# The draw library: font metrics, the DrawList render IR, the helper layer, and
# the reference builder. No dependency on layout -- a builder reads geometry
# columns by name and does not care who wrote them.

# The bundled font is embedded rather than loaded from a path: it is a hashed
# input, so it has to travel with the code.
scav_embed_bytes("${PROJECT_SOURCE_DIR}/assets/font/JetBrainsMono-Regular.ttf"
  bundled_ttf font_source)

scav_static_library(scavdraw
  c_api.cpp
  metrics.cpp
  drawlist.cpp
  helpers.cpp
  builder.cpp
  "${font_source}"
)
# The generated font header sits in the build tree beside its source.
foreach(target scavdraw scavdraw_testable)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

scav_install_library(scavdraw draw)
target_link_libraries(scavdraw PUBLIC scavcore)
target_link_libraries(scavdraw_testable PUBLIC scavcore_testable)

# Layout's public headers are POD vocabulary the measurement pass needs; the
# link edge stays absent, which is what scav_check_layering enforces.
foreach(target scavdraw scavdraw_testable)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/layout/include>")
endforeach()

# The redistributable carries every library's C surface.
target_sources(scav_shared PRIVATE c_api.cpp "${font_source}")
target_link_libraries(scav_shared PRIVATE scavdraw)

if(SCAV_BUILD_TESTS)
  scav_tests(scav_draw_tests
    metrics_tests.cpp
    drawlist_tests.cpp
    helpers_tests.cpp
    builder_tests.cpp
    tests/functional_drawlist_tests.cpp
  )
  target_link_libraries(scav_draw_tests PRIVATE scavdraw_testable scavlayout_testable)
endif()
