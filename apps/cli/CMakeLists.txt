# The scav executable, a repo tool. One file per verb: they share loading and
# nothing else.
add_executable(scav
  cli.cpp
  deps.cpp
  dump.cpp
  fmt.cpp
  main.cpp
  render.cpp
)
scav_settings(scav)
# Two edges reach all four archives: svg links draw PUBLIC and both leaves link
# core PUBLIC. Naming an archive that also arrives transitively repeats it on the
# link line.
target_link_libraries(scav PRIVATE scavlayout scavsvg)
