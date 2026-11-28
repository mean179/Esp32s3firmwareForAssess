/*
 * 考核题 H3 —— 阶段 1：按下按键翻转外接 LED
 *
 * 本阶段只做一件事：按键稳定按下 -> LED 状态翻转 -> 串口打一行日志。
 * 去抖采用“采样 + 变化计时 + 稳定电平确认”，全过程不阻塞主循环；
 * 计数、STATUS、ON/OFF/RESET 命令在阶段 2 / 阶段 3 叠加，此处不提前实现。
 *
 * 串口：本板无 USB-UART 芯片，控制台走 ESP32-S3 原生 USB-Serial-JTAG
 *      （sdkconfig.defaults 已把 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 打开）。
 */

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"

#include "board_config.h"

static bool s_led_on = false;

/*
 * 统一延时入口。
 *
 * 坑：CONFIG_FREERTOS_HZ 默认 100，pdMS_TO_TICKS(5) = 5*100/1000 = 0，
 * 而 vTaskDelay(0) 不阻塞、也不主动让出 CPU。主循环若连续 vTaskDelay(0)，
 * IDLE0 会被饿死，task watchdog（默认 5 s）就会一直刷
 * "Task watchdog got triggered ... IDLE0 (CPU 0)"。
 * 这里保证至少让出 1 个 tick。
 */
static void delay_ms(uint32_t ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

/* ---------------- LED ---------------- */

static void led_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_LED, 0);
}

static void led_set(bool on)
{
    s_led_on = on;
    gpio_set_level(PIN_LED, on ? 1 : 0);
}

/* ---------------- 按键 ---------------- */

static void button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* 输入上拉，按键另一端接 GND */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

/*
 * 去抖状态机（每个采样周期调用一次，自身不阻塞）：
 *   raw_prev   上一次采样值
 *   stable     已确认的稳定电平
 *   ts_change  raw 最近一次变化的时刻
 * 只有“连续保持同一电平 >= BUTTON_DEBOUNCE_MS”才更新 stable，
 * 并且只在 stable 落到低电平（按下）时返回 true，所以长按只算一次事件。
 */
static bool button_take_press(void)
{
    static int raw_prev   = 1;
    static int stable     = 1;
    static TickType_t ts_change = 0;

    const int raw = gpio_get_level(PIN_BUTTON);
    const TickType_t now = xTaskGetTickCount();

    if (raw != raw_prev) {                 /* 电平刚变化（可能还在抖） */
        raw_prev  = raw;
        ts_change = now;
        return false;
    }

    if (raw != stable &&
        (now - ts_change) >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
        stable = raw;                      /* 稳定电平确认 */
        return stable == 0;                /* 只把“按下”当事件 */
    }

    return false;
}

/* ---------------- 入口 ---------------- */

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);      /* 串口不缓冲，日志立即出来 */

    led_init();
    button_init();
    led_set(false);

    /*
     * READY 只在 USB 主机（串口监视器）真正连上后打印一次。
     *
     * 注意：IDF 的连接状态是“先假设已连接、随后靠 SOF 包确认”的，
     * 上电那一瞬间 usb_serial_jtag_is_connected() 必定为 true。若直接信它，
     * READY 会在没人看着的时候被打出去（丢弃），然后再也不打。所以这里要求
     * 连接标志连续为真 >= 150 ms 才认定“监视器真的连着”。
     */
    bool       ready_sent = false;
    bool       conn_prev  = false;
    TickType_t conn_since = 0;

    for (;;) {
        const TickType_t now  = xTaskGetTickCount();
        const bool       conn = usb_serial_jtag_is_connected();

        if (conn != conn_prev) {
            conn_prev  = conn;
            conn_since = now;
        }
        if (!ready_sent && conn &&
            (now - conn_since) >= pdMS_TO_TICKS(150)) {
            printf("READY\n");
            ready_sent = true;
        }

        if (button_take_press()) {
            led_set(!s_led_on);
            printf("BUTTON LED=%d\n", s_led_on ? 1 : 0);
        }

        delay_ms(BUTTON_POLL_MS);
    }
}
