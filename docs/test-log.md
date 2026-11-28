# 实测记录（H3 串口小灯 + 按键计数器）

> 只记录**真实跑过**的东西；没测的写“待测”，不补想象的数据。

## 0. 固定环境

| 项 | 值 |
|---|---|
| 开发板 | ottoRobot V1.1（自绘机器人主板，嘉立创 EDA） |
| 模组 | ESP32-S3-N16R8（实拍丝印见 `assets/`） |
| 工具链 | ESP-IDF **v5.5.5**，xtensa-esp-elf 14.2.0 |
| 目标 | `esp32s3`，16 MB flash |
| 串口 | 原生 USB-Serial-JTAG，枚举为“USB 串行设备 (COM4)”，115200 8N1 |
| 烧录 | `idf.py -p COM4 flash`（esptool，非 OpenOCD/JTAG） |
| 接线 | LED：上组排针（黄针→330 Ω→LED 长脚，短脚→棕针）；按键：下组排针（黄针→按键一脚，对角脚→棕针） |
| 引脚 | `LED=IO17`、`按键=IO39`（实测，见下表） |

## 1. 阶段 1 实测（2026-11-28，已通过）

### 1.1 引脚识别（`tools/pin_scan`）

| 项目 | 现象 | 结论 |
|---|---|---|
| LED 那列 | 扫描时 LED **闪 1 下**，其余候选不亮 | `LED = GPIO17`（SERVO1，模组脚 10） |
| 按键那列 | 按住时 `CHANGE GPIO39 = 0`，松开回到 1 | `按键 = GPIO39`（SERVO3，模组脚 32） |

> 扫描结束后 LED 会呈“微亮常亮”：工具把引脚切回了“输入 + 上拉”，45 kΩ 上拉漏
> 约 30–100 µA 过 LED。属工具行为，不是接线或器件问题。

### 1.2 阶段 1 串口输出（实际）

```
READY
BUTTON LED=1      ← 按一下
BUTTON LED=0      ← 再按一下
```

### 1.3 遇到的问题：task watchdog 刷屏

| 项 | 内容 |
|---|---|
| 现象 | 串口每 5 s 打印一次 `task_wdt: Task watchdog got triggered`，CPU 0 跑的是 `main`，backtrace 停在 `vTaskDelay`（`app_main.c:132`） |
| 环境 | ESP-IDF v5.5.5，`CONFIG_FREERTOS_HZ=100` |
| 原因 | 主循环 `vTaskDelay(pdMS_TO_TICKS(5))`：5 ms 在 100 Hz tick 下换算成 **0 tick**，`vTaskDelay(0)` 不让出 CPU，IDLE0 被饿死，task WDT 超时 |
| 修复 | 封装 `delay_ms()`，换算结果不足 1 tick 时补成 1 tick |
| 验证 | 修复后连续运行不再出现 WDT 打印（阶段 1 复测通过） |

## 2. 阶段 2 实测（本轮）

### 2.1 测试步骤与结果

“实测”列待本轮跑完填写。

| # | 操作 | 期望输出 / 现象 | 实测 | 结论 |
|---|---|---|---|---|
| 1 | 上电并打开监视器 | 打印一条 `READY`；LED 熄灭 |  |  |
| 2 | 输入 `STATUS` + 回车 | `LED=0 COUNT=0` |  |  |
| 3 | 短按一次按键 | LED 点亮；`BUTTON COUNT=1 LED=1` |  |  |
| 4 | 输入 `STATUS` | `LED=1 COUNT=1` |  |  |
| 5 | 再短按一次 | LED 熄灭；`BUTTON COUNT=2 LED=0` |  |  |
| 6 | 按住 2 s 再松开 | 全程只出现一行 `BUTTON COUNT=3 LED=1` |  |  |
| 7 | 按住期间输入 `STATUS` | 立即回 `LED=1 COUNT=3`（按住不影响串口） |  |  |
| 8 | 输入 `UNKNOWN` | `ERR`；LED 与计数不变 |  |  |
| 9 | 输入 `status`（小写） | `ERR`（命令区分大小写） |  |  |
| 10 | 只按回车（空行） | 无任何输出 |  |  |
| 11 | 输入 40 个字符的超长行 | 只出现 **一次** `ERR`；下一行 `STATUS` 仍正常 |  |  |
| 12 | 用 LF 换行发送 `STATUS` | 与 CRLF 一样正常识别 |  |  |
| 13 | 重新上电 | 计数回到 0、LED 熄灭、再打一次 `READY` |  |  |

### 2.2 实现要点（与本节测试对应）

- **非阻塞**：USB-Serial-JTAG 的 VFS 默认就是“读非阻塞、写忙等 TX FIFO”
  （`driver/usb_serial_jtag_vfs.h`），因此主循环里轮询 `read()` 不会挡住按键轮询；
  按键采样 5 ms（实际 1 tick = 10 ms），去抖 30 ms。
- **换行兼容**：默认 RX 会把 CRLF 当换行，但串口监视器按回车发的是**裸 CR**，
  会被 VFS 的 look-ahead 挂住等下一个字节。所以把 RX line endings 设成
  `ESP_LINE_ENDINGS_LF`（不转换），由固件自己把 `\r` 和 `\n` 都当行结束。
- **超长行**：第 33 个命令字符到达时立即输出一次 `ERR` 并进入“丢弃直到换行”状态，
  换行后状态复位，下一行照常处理。
- **命令区分大小写**：`strcmp` 精确比较，`status` 不等于 `STATUS`。

## 3. 遗留 / 下一步

- `ON` / `OFF` / `RESET` 尚未实现（阶段 3），当前一律回 `ERR`。
- 阶段 3 还要补：命令执行后 `LED` / `COUNT` 语义不变（`ON`/`OFF` 不改计数、
  `RESET` 不改 LED）、以及阶段的完整串口样例对照。
