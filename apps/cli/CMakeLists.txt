# The scav executable. Not installed yet: P3 owns the shipped CLI surface
# (fmt, deps, dump --json); until then this is a repo tool with one verb.
add_executable(scav main.cpp)
scav_settings(scav)
target_link_libraries(scav PRIVATE scavcore)
