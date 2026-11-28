# 一次性工具：测出舵机口信号针是哪个 GPIO（不需要万用表）
cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pin_scan)
