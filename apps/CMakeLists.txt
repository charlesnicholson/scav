# Applications are consumers: outside src/, so the CLI and viewer cannot quietly
# become privileged layers.
add_subdirectory(cli)
