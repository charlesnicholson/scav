# The scav executable, a repo tool. One file per verb: they share loading and
# nothing else.
add_executable(scav
  cli.cpp
  deps.cpp
  dump.cpp
  fmt.cpp
  main.cpp
)
scav_settings(scav)
target_link_libraries(scav PRIVATE scavcore)
