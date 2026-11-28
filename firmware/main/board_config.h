#pragma once

/*
 * 本板：ottoRobot V1.1（ESP32-S3 模组，原生 USB，无 USB-UART 桥）
 *
 * 舵机口每一列三针（实物上每个舵机就是一列，黄针总在内侧）：
 *   外针 = GND（棕）
 *   中针 = VBAT_IN，约 3.7 ~ 4.2 V（红）—— 禁止接任何 GPIO、禁止当 LED 正极
 *   内针 = 信号 / GPIO（黄）
 *
 * 6 个舵机信号只可能是下列 GPIO（原理图 IOs 区 -> CORE 区 U2）：
 *   SERVO1 = IO17   SERVO2 = IO18   SERVO5 = IO8
 *   SERVO6 = IO12   SERVO3 = IO39   SERVO4 = IO38
 *
 * 下面两个值是 2026-11-28 用 tools/pin_scan 在实物上实测确认的，不是照图纸猜的
 * （实测记录见 docs/pins.md 第 7 节）：
 *   LED  接"上组"某一列 -> 闪 1 下   -> GPIO17（SERVO1，模组脚 10）
 *   按键 接"下组"某一列 -> 按下变 0  -> GPIO39（SERVO3，模组脚 32）
 * 换列接线时只改这两行，app_main.c 不用动。
 */
#define PIN_LED     17   /* SERVO1 网 -> 模组脚 10（实测：闪 1 下） */
#define PIN_BUTTON  39   /* SERVO3 网 -> 模组脚 32（实测：按下 1->0） */

/* 按键采样与去抖（H3 要求稳定确认约 20~50 ms） */
#define BUTTON_POLL_MS      5
#define BUTTON_DEBOUNCE_MS  30
