# nhext - the external half. A small root binary that reads the game from the
# outside; it never injects, so the game's anti-cheat has nothing in-process to see.
add_executable(nhext src/ext.c)
target_compile_options(nhext PRIVATE -O2 -Wall -Wextra)
