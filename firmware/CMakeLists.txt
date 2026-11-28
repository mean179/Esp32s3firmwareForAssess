# ESP32-S3 外接 LED + 按键（考核题 H3）
cmake_minimum_required(VERSION 3.16)

# 本板固定为 ESP32-S3（ottoRobot V1.1，模组 ESP32-S3-N16R8）。
# 在这里写死，保证新克隆的仓库不用先 set-target 也能直接 idf.py build。
set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_led_button)
