/*
 * 考核题 H3 —— 阶段 3：完整命令集（ON / OFF / STATUS / RESET）
 *
 * 固件行为（与题面逐条对应）：
 *   1. 上电：LED 熄灭、计数 0；USB 监视器连上后打印一次 READY
 *   2. 按键：稳定按下 -> 计数 +1 且 LED 翻转，输出 "BUTTON COUNT=n LED=x"；
 *      按住不放只算一次，松开不计数；去抖稳定确认 30 ms
 *   3. 命令（按行、区分大小写、忽略空行）：
 *        ON     -> LED 置开，不动计数      -> "OK LED=1"
 *        OFF    -> LED 置关，不动计数      -> "OK LED=0"
 *        STATUS -> 读状态                  -> "LED=x COUNT=n"
 *        RESET  -> 计数清零，不动 LED      -> "OK COUNT=0"
 *        其它   -> "ERR"，LED 与计数不变
 *   4. 行缓冲：每行最多 32 个命令字符；超长整行丢弃到换行，只报一次 ERR，
 *      之后下一行合法命令照常处理
 *   5. 行结束符：LF、CRLF、裸 CR 都算一行结束
 *   6. 非阻塞：按键与串口在同一个循环里轮询，按住按键期间串口照常响应
 *
 * 为什么主循环不会被串口卡住：
 *   本板串口是原生 USB-Serial-JTAG。IDF 的 VFS 默认实现就是
 *   “读非阻塞 + 写忙等 TX FIFO”（见 driver/usb_serial_jtag_vfs.h 注释），
 *   没有数据时 read() 立刻返回 -1/EWOULDBLOCK，所以轮询按键和轮询串口
 *   可以在同一个循环里，谁也不用等谁。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "board_config.h"

/* 题面：每行最多 32 个命令字符（不含行结束符） */
#define CMD_MAX_CHARS 32

/* 应用状态：上电后 LED 灭、计数 0 */
static bool     s_led_on = false;
static uint32_t s_count  = 0;

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

/* ---------------- 串口命令 ---------------- */

static void console_init(void)
{
    /*
     * 默认 RX 换行转换是 CRLF：监视器按回车发的是裸 CR，会被 VFS 的
     * look-ahead 挂住（等下一个字节）。这里关掉转换，自己同时认 \r 和 \n。
     */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
}

static void handle_command(const char *cmd)
{
    if (strcmp(cmd, "ON") == 0) {
        led_set(true);                       /* 只改 LED，不动计数 */
        printf("OK LED=1\n");
    } else if (strcmp(cmd, "OFF") == 0) {
        led_set(false);                      /* 只改 LED，不动计数 */
        printf("OK LED=0\n");
    } else if (strcmp(cmd, "STATUS") == 0) {
        printf("LED=%d COUNT=%u\n", s_led_on ? 1 : 0, (unsigned)s_count);
    } else if (strcmp(cmd, "RESET") == 0) {
        s_count = 0;                         /* 只清计数，不动 LED */
        printf("OK COUNT=0\n");
    } else {
        /* 非法命令（含大小写不符）：回 ERR，状态保持不变 */
        printf("ERR\n");
    }
}

/* 每轮主循环调用一次：能读多少读多少，读不到就立刻返回 */
static void console_poll(void)
{
    static char   line[CMD_MAX_CHARS + 1];
    static size_t len      = 0;
    static bool   overflow = false;

    char    buf[32];
    ssize_t n;

    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            const char c = buf[i];

            if (c == '\r' || c == '\n') {            /* 一行结束 */
                if (!overflow && len > 0) {
                    line[len] = '\0';
                    handle_command(line);
                }
                len      = 0;                        /* 空行直接忽略 */
                overflow = false;
                continue;
            }

            if (overflow) {
                continue;                            /* 超长：丢弃到换行 */
            }
            if (len >= CMD_MAX_CHARS) {              /* 第 33 个字符 */
                printf("ERR\n");                     /* 只报一次 */
                overflow = true;
                continue;
            }
            line[len++] = c;
        }
    }
}

/* ---------------- 入口 ---------------- */

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);      /* 串口不缓冲，日志立即出来 */

    led_init();
    button_init();
    console_init();
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

        console_poll();                    /* 串口：非阻塞轮询 */

        if (button_take_press()) {         /* 按键：稳定按下才算一次 */
            s_count++;
            led_set(!s_led_on);
            printf("BUTTON COUNT=%u LED=%d\n",
                   (unsigned)s_count, s_led_on ? 1 : 0);
        }

        delay_ms(BUTTON_POLL_MS);
    }
}
