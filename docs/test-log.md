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

本轮（2026-11-28）在实物上逐条执行，串口输出与下表“期望”逐条一致。

| # | 操作 | 期望输出 / 现象 | 实测 | 结论 |
|---|---|---|---|---|
| 1 | 上电并打开监视器 | 打印一条 `READY`；LED 熄灭 | 与期望一致 | ✅ |
| 2 | 输入 `STATUS` + 回车 | `LED=0 COUNT=0` | 与期望一致 | ✅ |
| 3 | 短按一次按键 | LED 点亮；`BUTTON COUNT=1 LED=1` | 与期望一致 | ✅ |
| 4 | 输入 `STATUS` | `LED=1 COUNT=1` | 与期望一致 | ✅ |
| 5 | 再短按一次 | LED 熄灭；`BUTTON COUNT=2 LED=0` | 与期望一致 | ✅ |
| 6 | 按住 2 s 再松开 | 全程只出现一行 `BUTTON COUNT=3 LED=1` | 与期望一致 | ✅ |
| 7 | 按住期间输入 `STATUS` | 立即回 `LED=1 COUNT=3`（按住不影响串口） | 与期望一致 | ✅ |
| 8 | 输入 `UNKNOWN` | `ERR`；LED 与计数不变 | 与期望一致 | ✅ |
| 9 | 输入 `status`（小写） | `ERR`（命令区分大小写） | 与期望一致 | ✅ |
| 10 | 只按回车（空行） | 无任何输出 | 与期望一致 | ✅ |
| 11 | 输入 40 个字符的超长行 | 只出现 **一次** `ERR`；下一行 `STATUS` 仍正常 | 与期望一致 | ✅ |
| 12 | 用 LF 换行发送 `STATUS` | 与 CRLF 一样正常识别 | 与期望一致 | ✅ |
| 13 | 重新上电 | 计数回到 0、LED 熄灭、再打一次 `READY` | 与期望一致 | ✅ |

### 2.2 环境现象：`idf.py monitor` 里打字看不见自己的输入（不是固件问题）

现象：在 PowerShell 里的 `idf.py monitor` 中敲 `STATUS` 时屏幕上不显示字符，但回车后
设备响应完全正确 —— 说明字节已经送到板子上，只是**本地回显**没有。

原因（查源码确认，与固件无关）：

- `esp_idf_monitor/base/console_reader.py` 在 Windows 上用 `msvcrt.kbhit()` 轮询，
  再调用 `console.getkey()` 取键；这个 `Console` 来自 pyserial 的
  `serial/tools/miniterm.py`，其 Windows 实现在 `getkey()` 里用的是 `msvcrt.getwch()` ——
  这个 API 本身就是**读键不回显**，而且按键直接转发给串口，监视器自己不打印。
- 基类 `ConsoleBase.setup()` 的注释写得很直白：`Set console to read single characters, no echo`。
  非 Windows 平台走的是把 tty 设成 raw（`VMIN=0`、`VTIME=2`），效果一样：内核回显被关掉。
- 本版 `esp-idf-monitor 1.9.0` **没有 `--echo` 参数**（`python -m esp_idf_monitor --help`
  和包内 `add_argument` 都查过）。所以“盲打”是正常现象。
- 用 UART 口的板子通常感觉不到这个问题，是因为**设备端在回显**（ROM/bootloader 或
  esp_console 的 linenoise）。本固件按题面不实现回显，所以看不到自己敲的字。

顺带确认了一个有用的细节：`--eol` 的默认值在 Windows 上是 **CR**（帮助里写
“Defaults to LF for Linux targets and CR otherwise”），正好印证第 2.3 节换行处理的必要性。

可用做法（只是备选，不影响本题结论）：换用 PuTTY/Tera Term 这类本地回显的串口终端；
或者在固件里把收到的字符回显一遍（会污染命令协议，不建议用于提交版本）。


### 2.3 实现要点（与本节测试对应）

- **非阻塞**：USB-Serial-JTAG 的 VFS 默认就是“读非阻塞、写忙等 TX FIFO”
  （`driver/usb_serial_jtag_vfs.h`），因此主循环里轮询 `read()` 不会挡住按键轮询；
  按键采样 5 ms（实际 1 tick = 10 ms），去抖 30 ms。
- **换行兼容**：默认 RX 会把 CRLF 当换行，但串口监视器按回车发的是**裸 CR**，
  会被 VFS 的 look-ahead 挂住等下一个字节。所以把 RX line endings 设成
  `ESP_LINE_ENDINGS_LF`（不转换），由固件自己把 `\r` 和 `\n` 都当行结束。
- **超长行**：第 33 个命令字符到达时立即输出一次 `ERR` 并进入“丢弃直到换行”状态，
  换行后状态复位，下一行照常处理。
- **命令区分大小写**：`strcmp` 精确比较，`status` 不等于 `STATUS`。

## 3. 阶段 3 实测（本轮，完整命令集）

### 3.1 复现题面的“没有按键动作时”样例

从初始状态开始，按顺序逐条发送（“期望”列即题面给出的输出）：

| # | 输入 | 期望输出 | 实测 | 结论 |
|---|---|---|---|---|
| 1 | （上电） | `READY` | 与期望一致（LED 灭） | ✅ |
| 2 | `STATUS` | `LED=0 COUNT=0` | 与期望一致 | ✅ |
| 3 | `ON` | `OK LED=1` | 与期望一致 | ✅ |
| 4 | `STATUS` | `LED=1 COUNT=0` | 与期望一致 | ✅ |
| 5 | `RESET` | `OK COUNT=0` | 与期望一致 | ✅ |
| 6 | `OFF` | `OK LED=0` | 与期望一致 | ✅ |
| 7 | `UNKNOWN` | `ERR` | 与期望一致 | ✅ |

### 3.2 复现题面的“按键样例”

| # | 操作 | 期望输出 / 现象 | 实测 | 结论 |
|---|---|---|---|---|
| 8 | 按下并松开一次 | `BUTTON COUNT=1 LED=1`，灯亮 | 与期望一致 | ✅ |
| 9 | 再次按下并保持 2 s，然后松开 | 只出现一次 `BUTTON COUNT=2 LED=0`；保持期间不再计数 | 与期望一致 | ✅ |
| 10 | 保持按下期间发 `STATUS` | 立刻回 `LED=0 COUNT=2` | 与期望一致 | ✅ |
| 11 | 松开后发 `RESET` | `OK COUNT=0`；LED 仍为 0 | 与期望一致 | ✅ |

### 3.3 边界与异常

| # | 操作 | 期望输出 / 现象 | 实测 | 结论 |
|---|---|---|---|---|
| 12 | `on`（小写） | `ERR`，LED 与计数不变 | 与期望一致 | ✅ |
| 13 | 只按回车（空行） | 无输出 | 与期望一致 | ✅ |
| 14 | 连续 40 个字符的整行 | 只出现一次 `ERR`；紧接着 `STATUS` 正常响应 | 与期望一致 | ✅ |
| 15 | `idf.py monitor --eol CRLF` 后发 `STATUS` | 正常响应 | 未单独跑（默认 CR 路径已覆盖） | ⚠️ 未测 |
| 16 | `idf.py monitor --eol LF` 后发 `STATUS` | 正常响应 | 未单独跑（同上） | ⚠️ 未测 |
| 17 | 按住按键不放，同时发 `ON` / `OFF` | 命令照常响应；松开后只多计一次 | 与期望一致 | ✅ |
| 18 | 重新上电 | `READY`；LED 灭；`STATUS` 回 `LED=0 COUNT=0` | 与期望一致 | ✅ |

> 第 2–14 项直接用默认监视器敲回车即可：`idf.py monitor` 在 Windows 上默认 `--eol CR`
> （发裸 CR），固件已兼容。15/16 两项需要临时换成 `--eol CRLF` / `--eol LF` 各验一次，
> 本轮**没有单独跑**，如实标注为未测（固件里三条换行走的是同一段解析逻辑）。

### 3.4 命令语义自检（对照题面表格）

| 命令 | 题面要求 | 实现 |
|---|---|---|
| `ON` | LED 置开，**不改变计数** | `led_set(true)` 后打印 `OK LED=1`，不触碰 `s_count` |
| `OFF` | LED 置关，**不改变计数** | `led_set(false)` 后打印 `OK LED=0`，不触碰 `s_count` |
| `STATUS` | 读 LED 与计数 | 打印 `LED=%d COUNT=%u` |
| `RESET` | **只**清计数，**不改变 LED** | `s_count = 0` 后打印 `OK COUNT=0`，不触碰 LED |

## 4. 遗留 / 下一步

- G1 收尾：`docs/` 补 150 字 Git 基础说明；README 补齐“环境版本 / 编译 / 烧录 / 串口”四段。
- 提交材料：接线表 + 实物照片（`assets/`）、1–3 分钟实物演示、串口记录截图。
- 硬件方向不再扩展功能：网络、显示屏、传感器、PCB 都不在本题范围。
