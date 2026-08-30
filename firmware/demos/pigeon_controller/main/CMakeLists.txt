# DOGFIGHT_PROBE=1 swaps the app for the button-discovery probe, which is a
# throwaway build used to confirm hardware rather than a mode of the game.
if(DEFINED ENV{DOGFIGHT_PROBE})
    idf_component_register(SRCS "tools/button_discovery.c" INCLUDE_DIRS ".")
else()
    idf_component_register(SRCS "main.c" "hud.c" "link.c" "controller.c" "net.c"
                           INCLUDE_DIRS ".")
endif()

# The panel art, shared with demos/pigeon_preview rather than copied, so the
# two never drift. Runtime formats only: the screen is RGB565 and the two
# action loops are predecoded ARGB8888 frames, which avoids a GIF decoder on
# the device while keeping the authored frame timing and alpha.
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../../../assets/pigeon/runtime/controller_screen_448x368.rgb565"
    BINARY RENAME_TO "pigeon_screen_rgb565")
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../../../assets/pigeon/runtime/flap_wings_animation_v1.argb8888"
    BINARY RENAME_TO "flap_wings_animation_v1_argb8888")
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../../../assets/pigeon/runtime/deuce_payload_animation_v1.argb8888"
    BINARY RENAME_TO "deuce_payload_animation_v1_argb8888")
