# One directory per subsystem. `toy` is bootstrap scaffolding: it is what proves
# the harness reports failures, so it outlives the phase that introduced it.

add_subdirectory(core)
add_subdirectory(toy)
