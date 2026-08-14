# Applications are consumers (PRD 3.3): they live outside src/ so the CLI and
# viewer cannot quietly become privileged layers.
add_subdirectory(cli)
