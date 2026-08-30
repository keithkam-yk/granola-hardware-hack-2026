idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s3_touch_amoled_1_8 esp_driver_i2c esp_driver_gpio esp_timer esp_lvgl_port
)

target_add_binary_data(
    ${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../../../assets/pigeon/runtime/controller_screen_448x368.rgb565"
    BINARY
    RENAME_TO "pigeon_screen_rgb565"
)

target_add_binary_data(
    ${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../../../assets/pigeon/runtime/flap_wings_animation_v1.argb8888"
    BINARY
    RENAME_TO "flap_wings_animation_v1_argb8888"
)

target_add_binary_data(
    ${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../../../assets/pigeon/runtime/deuce_payload_animation_v1.argb8888"
    BINARY
    RENAME_TO "deuce_payload_animation_v1_argb8888"
)
