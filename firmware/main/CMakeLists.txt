idf_component_register(
    SRCS "imu_streamer.c"
    INCLUDE_DIRS "."
    REQUIRES qmi8658 esp32_s3_touch_amoled_1_8 esp_driver_i2c esp_timer nvs_flash bt esp_hid
)
