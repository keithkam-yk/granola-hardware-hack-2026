# DOGFIGHT_PROBE=1 swaps the app for the button-discovery probe, which is a
# throwaway build used to confirm hardware rather than a mode of the game.
if(DEFINED ENV{DOGFIGHT_PROBE})
    idf_component_register(SRCS "tools/button_discovery.c" INCLUDE_DIRS ".")
else()
    idf_component_register(SRCS "main.c" "hud.c" INCLUDE_DIRS ".")
endif()
