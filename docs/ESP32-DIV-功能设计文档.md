# ESP32-DIV 功能设计文档

## 1. 工程定位

ESP32-DIV 是一个 ESP-IDF 原生固件工程，默认目标为 ESP32-S3。工程入口为 `main/app_main`，应用模块位于 `components/esp32_div`，板级配置位于 `components/esp32_div_board`，平台抽象位于 `components/esp32_div_platform`。

当前固件版本常量为 `ESP32DIV_VERSION = "v1.7.0"`。默认板型由 `components/esp32_div_board/include/BoardConfig.h` 选择，当前默认 `BOARD_ESP32_DIV_V3`，可切换到 `BOARD_ESP32_DIV_V4`。

## 2. 架构分层

| 层级 | 目录 | 职责 |
| --- | --- | --- |
| 入口层 | `main/` | 初始化 ESP-IDF 运行环境并进入应用主循环。 |
| 应用层 | `components/esp32_div/` | 菜单、UI、Wi-Fi、BLE、Sub-GHz、IR、RFID/NFC、GPS、设置和文件管理。 |
| 平台层 | `components/esp32_div_platform/` | 字符串、串口、SPI、SD/VFS、Wi-Fi、TLS、NVS、TFT 显示、系统计时和 GPIO 封装。 |
| 板级层 | `components/esp32_div_board/` | 板型、屏幕面板、引脚、SPI 主机和默认参数。 |
| 外设组件 | `components/xpt2046`、`components/pcf8574`、`components/thirdparty_compat` | 触摸、I/O 扩展、RF/IR 等设备适配。 |
| 托管组件 | `managed_components/` | IDF Component Manager 下载的 Espressif 组件。 |

平台层的目标是让业务代码只依赖项目自己的 `Div*` API，并由该层统一映射到 ESP-IDF 驱动和系统服务。

## 3. 依赖策略

托管组件只保留当前工程实际需要的 Espressif 组件：

| 组件 | 用途 |
| --- | --- |
| `espressif/cjson` | JSON 解析和配置文件读写。 |
| `espressif/mdns` | 固件上传和网络服务发现。 |
| `espressif/esp-dsp` | 频谱、FFT 和信号处理。 |

其余功能优先使用 ESP-IDF 内置组件：`esp_wifi`、`bt`/NimBLE、`esp_event`、`esp_netif`、`nvs_flash`、`fatfs`、`sdmmc`、`esp_http_server`、`esp_https_ota`、`app_update`、`esp_driver_*`、FreeRTOS 和 VFS。

## 4. 板级与显示

`BoardConfig.h` 是板型选择入口，同一构建只能启用一个 `BOARD_*`。显示面板通过 `TFT_PANEL_ILI9341` 或 `TFT_PANEL_ST7796` 选择。

`components/esp32_div_board/include/tft_setup.h` 保存项目级显示参数，包括：

- SPI 引脚：`TFT_MISO`、`TFT_MOSI`、`TFT_SCLK`、`TFT_CS`、`TFT_DC`、`TFT_RST`、`TFT_BL`。
- 面板尺寸：ILI9341 为 `240x320`，ST7796 为 `320x480`。
- SPI 频率：默认 `27000000`。
- 背光极性：`TFT_BACKLIGHT_ON HIGH`。
- 颜色顺序、反色模式和面板驱动宏。

显示实现由 `DivDisplay` 和 `DivSprite` 提供，底层使用 ESP-IDF SPI master 驱动。

## 5. 数据存储

| 数据 | 主路径 | 说明 |
| --- | --- | --- |
| 应用设置 | `/config/settings.json` + NVS | 设置文件优先，NVS 用于关键小数据。 |
| 触摸校准 | `/config/settings.json` | 按 `TOUCH_PROFILE_ID` 分板保存。 |
| Captive Portal 凭据 | `/captive_portal/captured.csv` + NVS | SD 保存日志，NVS 保存当前索引和小规模状态。 |
| Sub-GHz profile | `/subghz/profiles_current.bin`、`/subghz/profiles_*.bin` | 当前 profile 与导出快照。 |
| IR profile | `/ir/*.ir` | 记录载波频率、raw 时序和名称。 |
| BLE Ducky 脚本 | `/ducky/*.duck`、`/ducky/*.txt` | 从 SD 读取并通过 BLE HID 执行。 |
| Wardriver 日志 | `/wd_*.csv`、`/wardrive/*.csv`、`/wd_wigle_upload.csv` | GNSS + Wi-Fi/BLE 观测记录。 |
| 固件文件 | `/firmware.bin` | SD 固件更新输入文件。 |

SD 文件访问通过 `DivSd`/`DivFile` 映射到 VFS/FATFS；持久键值通过 `DivNvStore` 映射到 NVS。

## 6. 功能模块

| 模块 | 源文件 | 主要能力 |
| --- | --- | --- |
| 主菜单和导航 | `app.cpp`、`utils.cpp`、`Touchscreen.cpp`、`KeyboardUI.cpp` | 启动、菜单、状态栏、触摸导航、键盘输入、通知弹窗。 |
| Wi-Fi | `wifi.cpp` | 扫描、监控、PCAP、Beacon、Deauth、Portal、Probe、固件上传。 |
| BLE / 2.4GHz | `bluetooth.cpp`、`ducky.cpp` | BLE 扫描、广播、HID、检测、2.4GHz 干扰和协议工具。 |
| Sub-GHz | `subghz.cpp` | CC1101 接收、重放、干扰、profile 管理。 |
| IR | `ir.cpp` | IR 采集、重放、遥控器配置和 profile 管理。 |
| RFID/NFC | `rfid.cpp` | PN532 读卡、克隆、dump、擦除、读卡器干扰和解码。 |
| GPS | `gps.cpp` | GNSS 读取、卫星视图、Wi-Fi/BLE wardriver、WiGLE CSV。 |
| DSP | `signal_dsp.cpp` | 频谱窗口、FFT 和图形数据处理。 |

## 7. 无线与任务模型

Wi-Fi 由 `esp_wifi` 管理，后台扫描任务在主界面空闲时运行，前台功能会先暂停后台任务以避免射频资源冲突。BLE 使用 IDF NimBLE host，广播、扫描和 HID 服务在业务模块内按需启动和停止。

长时间任务使用 FreeRTOS task 或循环内分片执行，所有功能必须响应 `feature_exit_requested` 和返回键事件。

## 8. 固件更新

SD 更新读取 `/firmware.bin` 并通过 ESP-IDF app update API 写入 OTA 分区，成功后重启。

Web 更新由 `esp_http_server` 提供上传接口，上传完成后通过相同的 app update API 写入固件。

## 9. UI 规则

- 功能页面使用 `featureClearContent()` 清理内容区域，保留底部触摸导航栏。
- 状态栏由 `drawStatusBar()` 和后台状态任务刷新。
- 触摸校准按板型 profile 保存，不能跨板共用。
- 显示尺寸和布局必须从 `tft.width()`、`tft.height()` 或板级宏读取。
- 新 UI 控件应复用 `FeatureUI`、`KeyboardUI` 和现有颜色常量。

## 10. 维护规则

1. 新代码只能依赖 ESP-IDF、托管 Espressif 组件或项目内 `Div*` 平台 API。
2. 板级引脚只在 `BoardConfig.h`、`BoardPins.h`、`tft_setup.h` 和 `shared.h` 中维护。
3. 文件系统路径必须以本文第 5 节为准，新增路径需要同步更新文档。
4. 功能模块不得直接初始化共享射频或 SPI 资源后长期占用；退出时必须恢复共享总线和后台状态。
5. 构建验证以 `idf.py build` 为准，默认目标为 ESP32-S3。
