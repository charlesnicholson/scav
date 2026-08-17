# The scav executable, a repo tool.
add_executable(scav main.cpp)
scav_settings(scav)
target_link_libraries(scav PRIVATE scavcore)
