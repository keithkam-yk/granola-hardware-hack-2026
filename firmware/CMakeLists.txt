cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dogfight)

# The vendor BSP logs a uint32_t through a %X, which this IDF's log macros
# reject under -Werror. Scoped to that component so our own code keeps the
# stricter setting.
idf_component_get_property(bsp_lib waveshare__esp32_s3_touch_amoled_1_8 COMPONENT_LIB)
target_compile_options(${bsp_lib} PRIVATE -Wno-error=format)
