/*
 * pin_scan —— 一次性诊断工具（跑完就可以覆盖回主固件）
 *
 * 目的：这块板的舵机排针只标了颜色、没标 GPIO。原理图能算出 6 个信号针
 *       只会是 IO17 / IO18 / IO8 / IO12 / IO39 / IO38 中的一个，但“哪一列
 *       是哪一个”由 PCB 布线决定。本工具不需要万用表：
 *
 *   STEP 1  LED 只接一列（信号针 -> 330Ω -> LED 长脚，LED 短脚 -> GND 针），
 *           工具依次驱动 6 个候选，第 i 个闪 i 下；串口同时打印编号。
 *           ==> 看到 LED 闪 N 下，这一列就是第 N 个候选的 GPIO。
 *
 *   STEP 2  再拿按键接另一列（信号针 + GND 针），工具进入读取模式，
 *           全部候选都是“输入 + 上拉”，按下时由 1 变 0 的那个就是按键脚。
 *           这一阶段所有脚都是输入，按键接到 GND 不会短路任何输出。
 *
 * 安全：D 阶段绝不触碰中间那根红针（VBAT 3.7~4.2V）。
 */

#include <stdbool.h>
#include <stdio.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_idf_version.h"

#define N_CAND 6

/* 原理图网络名 -> GPIO（顺序决定闪烁次数，别随意打乱） */
static const int   s_pin[N_CAND]  = { 17, 18,  8, 12, 39, 38 };
static const char *s_name[N_CAND] = { "SERVO1", "SERVO2", "SERVO5",
                                      "SERVO6", "SERVO3", "SERVO4" };

static void pin_to_input(int pin)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void all_to_input(void)
{
    for (int i = 0; i < N_CAND; i++) {
        pin_to_input(s_pin[i]);
    }
}

/*
 * 等“监视器真的连上”再开始打印。
 * IDF 的连接标志上电瞬间必为 true（随后才靠 SOF 包确认），所以要求连续为真
 * >= 300 ms；最多等 timeout_ms，避免没人开监视器时永远不动。
 */
static void wait_host_or_timeout(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    bool       prev  = false;
    TickType_t since = 0;

    for (;;) {
        const TickType_t now  = xTaskGetTickCount();
        const bool       conn = usb_serial_jtag_is_connected();

        if (conn != prev) {
            prev  = conn;
            since = now;
        }
        if (conn && (now - since) >= pdMS_TO_TICKS(300)) {
            return;
        }
        if ((now - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/*
 * 把 stdin 设成非阻塞：这样“等回车”可以带超时，监视器没开、或者 stdin
 * 在这块板子的原生 USB 控制台上不好用时，工具也不会卡死在第一步。
 */
static void stdin_set_nonblocking(void)
{
    const int fd = fileno(stdin);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

/* 等一次回车；最多等 timeout_ms，没人按键就自动继续 */
static void wait_enter(const char *msg, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();

    printf("%s\n", msg);

    for (;;) {
        if (getchar() != EOF) {
            printf("--- go ---\n\n");
            return;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            printf("--- (no key in %u ms, continue anyway) ---\n\n",
                   (unsigned)timeout_ms);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void led_scan(void)
{
    printf("=== LED scan ===\n");
    printf("每个候选依次点亮 i 次（250 ms 亮 / 250 ms 灭），间隔 1.2 s。\n");
    printf("数清楚 LED 闪了几下，再去下面找对应那行。\n\n");

    for (int i = 0; i < N_CAND; i++) {
        all_to_input();

        const gpio_config_t out = {
            .pin_bit_mask = 1ULL << s_pin[i],
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&out);
        gpio_set_level(s_pin[i], 0);
        vTaskDelay(pdMS_TO_TICKS(200));

        printf("  %d blink(s)  ->  GPIO%-2d  (%s)\n", i + 1, s_pin[i], s_name[i]);
        for (int k = 0; k <= i; k++) {
            gpio_set_level(s_pin[i], 1);
            vTaskDelay(pdMS_TO_TICKS(250));
            gpio_set_level(s_pin[i], 0);
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        pin_to_input(s_pin[i]);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }

    printf("\n=== LED scan done ===\n");
}

static void button_scan(void)
{
    all_to_input();

    printf("=== Button scan ===\n");
    printf("所有候选 = 输入 + 上拉（松开读 1，按下接地读 0）。\n");
    printf("把按键接在任意一列的信号针和 GND 针之间，然后按住。\n");
    printf("哪一行从 1 变成 0，那个 GPIO 就是按键脚。\n\n");

    int last[N_CAND], stable[N_CAND], cnt[N_CAND];
    for (int i = 0; i < N_CAND; i++) {
        last[i] = stable[i] = gpio_get_level(s_pin[i]);
        cnt[i]  = 0;
    }

    printf(" 现在状态：");
    for (int i = 0; i < N_CAND; i++) {
        printf(" GPIO%d=%d", s_pin[i], stable[i]);
    }
    printf("\n\n");

    for (;;) {
        for (int i = 0; i < N_CAND; i++) {
            const int v = gpio_get_level(s_pin[i]);

            if (v != last[i]) {              /* 还在跳变，重新计时 */
                last[i] = v;
                cnt[i]  = 0;
                continue;
            }
            if (v == stable[i]) {
                cnt[i] = 0;
                continue;
            }
            if (++cnt[i] >= 3) {             /* 3 * 10 ms = 30 ms 稳定 */
                stable[i] = v;
                cnt[i]    = 0;
                printf("  CHANGE  GPIO%-2d (%s) = %d%s\n",
                       s_pin[i], s_name[i], v,
                       v == 0 ? "   <== 按下，这就是按键脚" : "");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    stdin_set_nonblocking();

    /* 先等监视器连上，否则启动横幅会打给“空气” */
    wait_host_or_timeout(15000);

    printf("\n=== pin_scan ===  ESP-IDF %s\n", esp_get_idf_version());
    printf("board: ottoRobot V1.1 / ESP32-S3（原生 USB 控制台）\n");
    printf("排针：外针=GND(棕) | 中针=VBAT 3.7~4.2V(红，全程不接) | 内针=信号(黄)\n");
    printf("候选：");
    for (int i = 0; i < N_CAND; i++) {
        printf("  GPIO%d(%s)", s_pin[i], s_name[i]);
    }
    printf("\n\n");

    wait_enter("STEP 1  只接 LED：信号针 -> 330Ω -> LED 长脚，LED 短脚 -> 该列 GND 针。\n"
               "        接好以后在监视器里按一下回车（不按也会在 20 s 后自动开始）。",
               20000);
    led_scan();

    wait_enter("STEP 2  保留 LED，把按键接在另一列：信号针 + GND 针。\n"
               "        （两件东西不要接在同一列上：一列只有一个信号脚）\n"
               "        接好以后在监视器里按一下回车（不按也会在 20 s 后自动开始）。",
               20000);
    button_scan();
}
