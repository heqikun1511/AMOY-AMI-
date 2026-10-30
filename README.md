# ESP32-P4 LVGL 仪表盘

本项目是在正点原子 ESP32-P4 开发板上完成的 LVGL 8 图形界面移植工程。界面由 EEZ Studio 生成，支持 LCD 显示、GT9xxx 电容触摸、按键切换页面，以及通过 CAN（TWAI）接收转速数据并更新仪表盘车速。

## 主要功能

- 基于 ESP-IDF 和 FreeRTOS
- 使用 LVGL 8.3（组件管理器会自动下载兼容版本）
- 支持正点原子 RGB LCD 和 MIPI DSI LCD
- 支持 GT9xxx 系列电容触摸屏
- 支持 1920 × 1080 横屏 UI
- 支持 Driver View / Autonomous 两个页面
- KEY1 按键循环切换页面
- 通过 CAN 扩展帧接收电机转速，并映射为 0～120 的车速显示

## 硬件环境

- 正点原子 ESP32-P4 开发板
- 正点原子 RGB LCD 或 MIPI LCD
- GT9xxx 电容触摸屏
- CAN 收发器及相应总线设备（可选）
- 建议使用带 PSRAM 的配置；当前工程已启用 PSRAM

当前应用使用的引脚如下：

| 功能 | GPIO | 说明 |
| --- | ---: | --- |
| KEY1 | GPIO35 | 低电平有效 |
| CAN TX | GPIO27 | 可在 `main/main.c` 中修改 |
| CAN RX | GPIO26 | 可在 `main/main.c` 中修改 |
| LCD 背光 | GPIO53 | 由 BSP 驱动管理 |
| LCD 复位 | GPIO52 | 由 BSP 驱动管理 |

> RGB LCD 和 MIPI LCD 同时接入时，驱动会优先选择 RGB LCD。不同板卡或屏幕的引脚可能不同，请以实际硬件原理图为准。

## 软件依赖

- 支持 ESP32-P4 的 ESP-IDF
- CMake 3.16 或更高版本
- LVGL `^8.3.11`（由 ESP-IDF Component Manager 管理）

使用前请先安装 ESP-IDF，并加载其开发环境：

```bash
. /path/to/esp-idf/export.sh
```

## 编译和烧录

进入项目根目录后执行：

```bash
idf.py set-target esp32p4
idf.py build
```

连接开发板并烧录、打开串口监视器：

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

请根据实际串口修改 `/dev/ttyUSB0`。退出监视器可按 `Ctrl+]`。

如需修改 Flash、PSRAM、日志级别或其他参数，可运行：

```bash
idf.py menuconfig
```

## 使用说明

上电后程序会依次初始化 NVS、LCD、触摸屏、LVGL、CAN 和 UI 更新任务。

- 短按 KEY1，可在 `Driver View` 和 `Autonomous` 页面间循环切换。
- 程序接收 CAN 扩展帧 `0x08C1EF21` 或 `0x0CFFC6EF`。
- 报文前两个字节按小端格式解析为转速，分辨率为 `0.5 rpm/bit`。
- 当前车速显示按 `abs(rpm) × 120 / 10000` 映射，并限制在 0～120。
- CAN 初始化失败不会阻止界面运行，相关警告会输出到串口。

CAN ID、引脚和转速映射逻辑可在以下文件中调整：

- `main/main.c`
- `main/BSP/bsp_can.h`
- `main/BSP/bsp_can.c`

## 项目结构

```text
.
├── CMakeLists.txt                 # ESP-IDF 工程入口
├── sdkconfig                      # 工程配置
├── components/
│   ├── BSP/                       # LCD、触摸、I²C、LED 驱动
│   └── Middlewares/               # 中间件组件目录
└── main/
    ├── main.c                     # 程序入口、CAN 数据处理、按键与 UI 更新
    ├── APP/
    │   └── lvgl_demo.c            # LVGL 显示和输入设备适配
    ├── BSP/
    │   └── bsp_can.c              # CAN（TWAI）驱动
    └── ui/                        # EEZ Studio 导出的界面、字体和图片资源
```

## 注意事项

- 1920 × 1080 全屏绘制和旋转缓冲区会占用较多 PSRAM。
- MIPI 竖屏会在软件中旋转为 LVGL 横屏坐标，触摸坐标也会同步转换。
- 接入大尺寸 LCD 时请确保供电充足；10.1 英寸屏可按开发板说明使用双 Type-C 供电。
- 修改或重新导出 EEZ Studio UI 后，应检查分辨率、对象名称和 LVGL 8 API 是否保持兼容。
- `managed_components/` 为组件管理器生成目录，无需手动提交到版本库。
- 本工程使用eezstudio来完成，不支持flow功能特性
