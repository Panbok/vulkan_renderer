# The FPS sample game. It reaches the engine only through sdk.h; the runtime
# link supplies the platform definitions its headers need. Bakery builds the
# same sources from fps.script.json as a shared library that sees only the
# SDK and foundation headers.
add_library(vkr_script_fps STATIC
    src/fps_camera_rig.c
    src/fps_door.c
    src/fps_input.c
    src/fps_locomotion.c
    src/fps_module.c
    src/fps_player.c
    src/fps_player_animation.c
    src/fps_weapon.c)
vkr_configure_library(vkr_script_fps)
target_include_directories(vkr_script_fps PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vkr_script_fps PUBLIC vkr_runtime)
