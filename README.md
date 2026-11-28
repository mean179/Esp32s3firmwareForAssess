# ESP32-S3 串口小灯与按键计数器

深蓝实验室电控组第二轮考核题 · **硬件方向**（H1–H3）的实物工程仓库。

在 ottoRobot V1.1（ESP32-S3 模组）开发板上，用一个**外接红色 LED** 和一个**轻触按键**
完成串口控制、按键计数与状态反馈。板上没有 USB-UART 桥，串口走 ESP32-S3 原生 USB-Serial-JTAG。

## 进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 按下按键翻转 LED | ✅ 已实现，实物验证通过 |
| 2 | 按键计数 + `STATUS` 上报 | ✅ 已实现（待本轮实测确认） |
| 3 | `ON` / `OFF` / `RESET` 完整命令 | ⬜ 待做 |

实测记录见 [docs/test-log.md](docs/test-log.md)。

## 硬件与接线

| 项目 | 内容 |
|---|---|
| 开发板 | ottoRobot V1.1（自绘机器人主板，嘉立创 EDA） |
| 模组 | ESP32-S3-N16R8（16 MB flash / 8 MB PSRAM） |
| 串口 | 原生 USB-Serial-JTAG（IO19/IO20），**没有 CH340/CP210x** |
| 接线口 | 板载舵机排针：外针 GND（棕）/ 中针 VBAT 3.7–4.2 V（红，禁用）/ 内针 信号（黄） |
| LED | 直插红色 LED + 330 Ω 限流，GPIO 高电平点亮 |
| 按键 | 6×6 轻触按键，一端 GPIO、一端 GND，芯片内部上拉（按下 = 低电平） |
| 引脚 | `firmware/main/board_config.h`（**实测**：`LED=IO17`、`按键=IO39`） |

详细的行列对应、6 个候选 GPIO、以及“哪一列是哪个 SERVO”的实测方法见
[docs/pins.md](docs/pins.md)。

> ⚠️ 红色中针是电池电压（3.7–4.2 V），不是 3.3 V、也不是 5 V，全程不接任何东西。

## 环境

- **ESP-IDF v5.5.5**（`C:\APPs\ESPIDF\frameworks\esp-idf-v5.5.5`），xtensa-esp-elf 14.2.0
- 目标芯片 `esp32s3`，flash 16 MB
- 控制台 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`，监视器 115200 8N1（USB CDC 实际不限速，按题面记录）
- 配置在 `firmware/sdkconfig.defaults`；`sdkconfig` 与 `build/` 不入库

## 目录

```
.
├─ README.md
├─ .gitignore
├─ docs/                    # 引脚/接线记录、测试记录
├─ assets/                  # 实拍照片
├─ firmware/                # 主固件（ESP-IDF 工程，入口 main/app_main.c）
│  ├─ CMakeLists.txt
│  ├─ sdkconfig.defaults
│  └─ main/{CMakeLists.txt, app_main.c, board_config.h}
└─ tools/pin_scan/          # 一次性工具：不必用万用表，测出舵机口信号针是哪个 GPIO
```

## 编译与烧录

每个新终端先导入 ESP-IDF 环境：

```powershell
$env:IDF_TOOLS_PATH = 'C:\APPs\ESPIDF'
& 'C:\APPs\ESPIDF\frameworks\esp-idf-v5.5.5\export.ps1'
```

主固件：

```powershell
cd firmware
idf.py set-target esp32s3        # 仅首次；sdkconfig 不入库
idf.py build
idf.py -p COM3 flash monitor     # COM3 换成设备管理器里的 "USB 串行设备"
```

- 端口在“设备管理器 → 端口(COM 和 LPT)”里看，本板枚举为 **USB 串行设备 (COMx)**
  （USB\VID_303A&PID_1001，ESP32-S3 USB-Serial-JTAG）。
- 退出监视器：`Ctrl+]`。
- 不确定引脚时先跑 `tools/pin_scan`（见 docs/pins.md 第 5 节），30 秒就能测出来。

## 当前行为（阶段 1）

上电后 LED 熄灭、计数为 0；USB 串口监视器连上后打印一次：

```
READY
```

按下按键（稳定 30 ms 去抖确认）→ 计数 +1 且 LED 翻转 → 串口打印：

```
BUTTON COUNT=1 LED=1
```

再按一次（长按只算一次，松开不触发）：

```
BUTTON COUNT=2 LED=0
```

串口命令按行接收（LF / CRLF / 裸 CR 都可），命令**区分大小写**，空行忽略，
每行最多 32 个命令字符（超长整行丢弃，只报一次 `ERR`）：

| 命令 | 作用 | 输出 |
|---|---|---|
| `STATUS` | 读 LED 与计数 | `LED=1 COUNT=2` |
| 其它（含 `ON` / `OFF` / `RESET`） | 阶段 3 尚未实现 | `ERR` |

> READY 设计为“USB 主机连上后立即打印一次”：这块板没有 UART 桥，没人连监视器时
> printf 会被丢弃，先打就看不见。

## 已知限制

- 引脚已用 `tools/pin_scan` 在实物上实测确认：`LED=IO17`（闪 1 下）、`按键=IO39`（按下 1→0）。
  换列接线时只改 `board_config.h` 两行，主程序不用动。
- `ON` / `OFF` / `RESET` 属于阶段 3，当前与其它非法命令一样回 `ERR`。
