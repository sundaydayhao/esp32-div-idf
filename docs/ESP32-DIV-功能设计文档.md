# ESP32-DIV v1/v2/v3/v4 功能设计文档

本文档用于把当前仓库中 ESP32-DIV v1/v2/v3/v4 固件支持的功能、硬件差异、菜单结构、数据文件和运行限制整理成后续 AI 开发上下文。它既是功能设计文档，也是 memory bank：后续修改代码前应先读取本文，确认板型宏、屏幕模组、引脚差异和功能边界。

内容依据当前源码、随仓硬件资料、`hardware/schematic/v3`、`hardware/schematic/v4`、LCDWiki 屏幕资料和 `docs/firmware/` 预编译产物整理。v3/v4 的用户给定约束是：两者板子逻辑相同，唯一区别是 TFT 屏幕尺寸和驱动不同；固件逻辑需要用 `BOARD_ESP32_DIV_V3` 与 `BOARD_ESP32_DIV_V4` 宏区分。

适用范围：

- 设备：ESP32-DIV V1 ESP32U、ESP32-DIV V2、ESP32-DIV V3、ESP32-DIV V4。
- 固件版本常量：`ESP32DIV_VERSION = "v1.7.0"`。
- 当前 IDF 迁移代码默认编译目标：`BOARD_ESP32_DIV_V3`。历史 Arduino 文档中 v2 曾是默认目标，后续修改时必须以实际 `components/esp32_div_board/include/BoardConfig.h` 和 `components/esp32_div/include/shared.h` 为准。
- 功能定位：多频段无线安全研究工具，覆盖 Wi-Fi、BLE、2.4GHz NRF24、Sub-GHz CC1101、IR、RFID/NFC、GPS 和设备维护。
- 使用前提：仅用于自有设备、授权实验室、教学研究和合规测试。

## 1. 源码结构与关键文件

| 文件或目录 | 作用 |
| --- | --- |
| `components/esp32_div_board/include/BoardConfig.h` | IDF 工程的板型选择入口；应只启用一个 `BOARD_*` 宏。当前默认 `BOARD_ESP32_DIV_V3`，可切换到 `BOARD_ESP32_DIV_V4`。 |
| `components/esp32_div_board/include/tft_setup.h` | TFT_eSPI 项目级配置；按 `TFT_PANEL_ILI9341` 或 `TFT_PANEL_ST7796` 选择驱动、尺寸、SPI 频率和 TFT 引脚。 |
| `components/esp32_div/include/shared.h` | 板级名称、能力标记、触摸 profile、PCF8574、SD、PN532、GPS、CC1101、NRF24、IR 等引脚映射和功能开关。 |
| `components/esp32_div/src/app.cpp` | 主入口、主菜单、子菜单、设置页、About 页、初始化流程。 |
| `components/esp32_div/src/wifi.cpp` | Wi-Fi 工具、Captive Portal、固件更新、Wi-Fi 后台扫描。 |
| `components/esp32_div/src/bluetooth.cpp` | BLE 工具、2.4GHz NRF24 扫描和干扰、Bluetooth Sniffer、BLE 后台扫描。 |
| `components/esp32_div/src/subghz.cpp` | CC1101 Sub-GHz Replay、Jammer、Saved Profile、蜂鸣器提示。 |
| `components/esp32_div/src/ir.cpp` | IR 录制、保存、回放、通用遥控器和 SD 红外配置导入。 |
| `components/esp32_div/src/rfid.cpp` | PN532 RFID/NFC 读卡、克隆、擦除、Dump、Access Decode 和干扰类流程。 |
| `components/esp32_div/src/gps.cpp` | GPS 卫星扫描、Wardriver、WiGLE 上传。 |
| `components/esp32_div/src/ducky.cpp` | ESP32-S3 BLE HID Keyboard 与 DuckyScript 执行。 |
| `components/esp32_div/src/utils.cpp` | UI 工具、设置页、状态栏、SD 文件管理器、触摸校准、PCF8574 扫描等公共能力。 |
| `components/esp32_div/src/SettingsStore.cpp` | `/config/settings.json` 和 NVS 中设置、触摸校准 profile 的加载和保存。 |
| `Libraries/User_Setup v1.h` | v1 的 TFT_eSPI 配置。 |
| `Libraries/User_Setup v2.h` | v2 的 TFT_eSPI 配置。 |
| `hardware/schematic/v3/` | v3 硬件原理图截图，包含 ESP32 周边、电源、USB-C、TFT 接口、PCF8574、WS2812、IR/CC1101/NRF24L01 等。 |
| `hardware/schematic/v4/` | v4 硬件原理图截图；当前文件 hash 与 v3 相同，结合用户说明可视为仅替换 TFT 模组。 |
| `docs/firmware/` | v1/v2/CYD 的 v1.7.0 Web Flasher 固件文件。 |
| `ducky scripts/` | 随仓示例 DuckyScript，运行时应复制到 SD 卡 `/ducky`。 |

## 2. v1/v2/v3/v4 功能总览

四个 ESP32-DIV 板型共享同一套应用菜单和功能模块。v1 是经典 ESP32，缺少 ESP32-S3 BLE HID Ducky 能力；v2/v3/v4 都按 ESP32-S3 能力处理。v3/v4 在业务功能上应完全一致，区别只落在 TFT 屏幕模组、驱动和分辨率上。

| 功能域 | v1 | v2 | v3 | v4 | 说明 |
| --- | --- | --- | --- | --- | --- |
| Wi-Fi 工具 | 支持 | 支持 | 支持 | 支持 | Packet Monitor、Beacon Spammer、Deauther、Probe Flood、Deauth Detector、WiFi Scanner、Captive Portal。 |
| Bluetooth/BLE 工具 | 支持 | 支持 | 支持 | 支持 | BLE Jammer、BLE Spoofer、Sour Apple、Sniffer、BLE Scanner。 |
| BLE Rubber Ducky | 不支持 | 支持 | 支持 | 支持 | 由 `FEATURE_BLE_DUCKY = BOARD_HAS_ESP32S3` 控制。v1 为经典 ESP32，v2/v3/v4 按 ESP32-S3 处理。 |
| 2.4GHz NRF24 工具 | 支持 | 支持 | 支持 | 支持 | 需要 3 个 NRF24 模块，提供 2.4GHz Scanner 和 Proto Kill。 |
| Sub-GHz CC1101 | 支持 | 支持 | 支持 | 支持 | Replay Attack、SubGHz Jammer、Saved Profile。 |
| IR | 支持 | 支持 | 支持 | 支持 | IR Record、Saved Profile、Universal Controller。 |
| RFID/NFC | 支持 | 支持 | 支持 | 支持 | 需要 PN532 SPI 模块，支持 ISO14443A 标签相关流程。 |
| GPS | 支持 | 支持 | 支持 | 支持 | 需要 GPS 串口模块，支持 Wardriver 和 Satellite Scanner。 |
| PCF8574 物理按键 | 支持 | 支持 | 支持 | 支持 | v2/v3/v4 均使用 I2C SDA GPIO8、SCL GPIO9，地址默认 `0x20`。 |
| 触摸导航 | 支持 | 支持 | 支持 | 支持 | XPT2046；触摸校准按 `TOUCH_PROFILE_ID` 分板保存。 |
| 设置/维护 | 支持 | 支持 | 支持 | 支持 | Serial Monitor、Update Firmware、Touch Calibrate、SD File Manager、Settings、About。 |

## 3. 板级差异

### 3.1 板型与芯片

| 项目 | v1 | v2 | v3 | v4 |
| --- | --- | --- | --- | --- |
| 板型宏 | `BOARD_ESP32_DIV_V1` | `BOARD_ESP32_DIV_V2` | `BOARD_ESP32_DIV_V3` | `BOARD_ESP32_DIV_V4` |
| 板名 | `ESP32-DIV V1 ESP32U` | `ESP32-DIV V2` | `ESP32-DIV V3` | `ESP32-DIV V4` |
| 主控能力标记 | `BOARD_HAS_ESP32S3 = 0` | `BOARD_HAS_ESP32S3 = 1` | `BOARD_HAS_ESP32S3 = 1` | `BOARD_HAS_ESP32S3 = 1` |
| PCF8574 按键 | 有 | 有 | 有 | 有 |
| 默认选择 | 需手动启用 | 历史默认 | 当前 `BoardConfig.h` 默认 | 当前可切换目标 |
| BLE HID Ducky | 无 | 有 | 有 | 有 |
| v3/v4 关系 | 不适用 | 不适用 | 与 v4 板级逻辑相同 | 与 v3 板级逻辑相同，仅 TFT 模组不同 |

后续代码修改原则：

- 必须用 `BOARD_ESP32_DIV_V3` 和 `BOARD_ESP32_DIV_V4` 区分 v3/v4 板型身份、触摸 profile、固件打包和 UI/布局条件。
- TFT 面板驱动可以由板型默认派生：v3 默认 `TFT_PANEL_ILI9341`，v4 默认 `TFT_PANEL_ST7796`。
- 不应只用 `TFT_PANEL_*` 替代板型宏，因为触摸校准、固件命名、硬件文档、打包产物和后续可能的差异都需要保留板型身份。

### 3.2 v3/v4 屏幕硬件差异

v3 和 v4 的原理图截图目录内容当前完全一致；两者的用户定义差异是 TFT 模组不同。LCDWiki 资料显示这两块屏都使用 4-wire SPI 显示接口，触摸控制器为 XPT2046，因此除显示驱动和分辨率外，触摸、背光、SPI 引脚和应用功能应保持同一逻辑。

| 项目 | v3 | v4 |
| --- | --- | --- |
| 硬件目录 | `hardware/schematic/v3/` | `hardware/schematic/v4/` |
| 屏幕资料 | <https://www.lcdwiki.com/zh/3.2inch_SPI_Module_ILI9341_SKU:MSP3218> | <https://www.lcdwiki.com/zh/4.0inch_SPI_Module_ST7796> |
| LCDWiki 模组 | 3.2inch SPI Module ILI9341，SKU MSP3218 | 4.0inch SPI Module ST7796 |
| TFT 驱动 IC | ILI9341 | ST7796/ST7796S |
| 物理尺寸 | 3.2 inch | 4.0 inch |
| 原生像素 | 320x240 | 480x320 |
| TFT_eSPI portrait 宏尺寸 | `TFT_WIDTH 240`、`TFT_HEIGHT 320` | `TFT_WIDTH 320`、`TFT_HEIGHT 480` |
| 显示接口 | 4-wire SPI | 4-wire SPI |
| 触摸控制器 | XPT2046 | XPT2046 |
| 固件面板宏 | `TFT_PANEL_ILI9341` | `TFT_PANEL_ST7796` |
| TFT_eSPI 驱动宏 | `ILI9341_DRIVER` | `ST7796_DRIVER` |
| SPI 频率 | `SPI_FREQUENCY 27000000` | `SPI_FREQUENCY 27000000` |
| SPI 读频率 | `SPI_READ_FREQUENCY 20000000` | `SPI_READ_FREQUENCY 20000000` |
| 颜色顺序 | `TFT_RGB_ORDER TFT_BGR` | `TFT_RGB_ORDER TFT_BGR` |
| 屏幕反色 | `TFT_INVERSION_OFF` | `TFT_INVERSION_OFF` |
| 默认旋转 | `TFT_ROTATION 0` | `TFT_ROTATION 0` |

后续 UI 修改注意：

- 旧功能界面中有不少位置仍使用固定 `240`、`320` 或底部 `y=304` 一类坐标。v4 的 320x480 portrait 尺寸会放大可用区域，后续适配时应优先使用 `tft.width()`、`tft.height()`、`TFT_WIDTH`、`TFT_HEIGHT`、`SCREEN_WIDTH`、`SCREEN_HEIGHT` 等运行时或宏尺寸。
- v3/v4 功能逻辑一致；不要因为 v4 屏幕更大而改变菜单层级、外设行为或数据路径。适配应主要是布局、可见行数、触摸区域和图形比例。
- v4 的 ST7796 帧缓冲面积为 v3 的 2 倍，刷新大量全屏内容时要关注 SPI 带宽、阻塞时间和内存占用。

### 3.3 触摸屏、背光和触摸校准

| 项目 | v1 | v2 | v3 | v4 |
| --- | --- | --- | --- | --- |
| 触摸 profile | `ESP32_DIV_V1` | `ESP32_DIV_V2` | `ESP32_DIV_V3` | `ESP32_DIV_V4` |
| 默认触摸 X | `300..3800` | `280..3850` | `280..3850` | `280..3850` |
| 默认触摸 Y | `300..3800` | `320..3750` | `320..3750` | `320..3750` |
| TFT 旋转 | `TFT_ROTATION 0` | `TFT_ROTATION 0` | `TFT_ROTATION 0` | `TFT_ROTATION 0` |
| XPT2046 CS | GPIO33 | GPIO18 | GPIO18 | GPIO18 |
| XPT2046 MOSI | GPIO23 | GPIO35 | GPIO35 | GPIO35 |
| XPT2046 MISO | GPIO19 | GPIO37 | GPIO37 | GPIO37 |
| XPT2046 CLK | GPIO18 | GPIO36 | GPIO36 | GPIO36 |
| XPT2046 IRQ | 255，未使用 | 255，未使用 | 255，未使用 | 255，未使用 |
| XPT2046 host | `SPI2_HOST` | `SPI3_HOST` | `SPI2_HOST` | `SPI2_HOST` |
| 固件背光 PWM | GPIO32 | GPIO7 | GPIO7 | GPIO7 |
| PWM | 通道 0，5kHz，8-bit | 通道 0，5kHz，8-bit | 通道 0，5kHz，8-bit | 通道 0，5kHz，8-bit |
| TFT_eSPI 背光宏 | 旧配置相关 | `TFT_BL 7` | `TFT_BL 7` | `TFT_BL 7` |

触摸逻辑：

- `Touchscreen.cpp` 先读取 XPT2046 raw ADC，再根据 settings 中的 `touchXMin/touchXMax/touchYMin/touchYMax` 映射到屏幕坐标。
- 非 CYD 板默认会翻转 Y 轴：`rawY` 从 `touchYMax..touchYMin` 映射到 `0..TFT_HEIGHT-1`。
- 设置保存时用 `TOUCH_PROFILE_ID` 区分板型，避免一张 SD 卡在不同板子间复用时误用触摸校准。
- v3/v4 虽然默认 raw 范围一致，也必须保留各自 profile：v3 是 `ESP32_DIV_V3`，v4 是 `ESP32_DIV_V4`。

### 3.4 PCF8574 按键映射

PCF8574 地址默认自动检测 `0x20..0x27`，也可以在 `BoardConfig.h` 中用 `pcf_ADDR` 固定。

| 按键 | v1 PCF8574 引脚 | v2 PCF8574 引脚 | v3 PCF8574 引脚 | v4 PCF8574 引脚 |
| --- | --- | --- | --- | --- |
| UP | 6 | 7 | 3 | 3 |
| DOWN | 3 | 5 | 5 | 5 |
| LEFT | 4 | 3 | 6 | 6 |
| RIGHT | 5 | 4 | 4 | 4 |
| SELECT | 7 | 6 | 7 | 7 |

| 项目 | v1 | v2 | v3 | v4 |
| --- | --- | --- | --- | --- |
| I2C SDA | GPIO8 | GPIO8 | GPIO8 | GPIO8 |
| I2C SCL | GPIO9 | GPIO9 | GPIO9 | GPIO9 |
| 固定地址入口 | `pcf_ADDR` | `pcf_ADDR` | `pcf_ADDR` | `pcf_ADDR` |
| 当前默认地址 | 由配置决定 | `0x20` | `0x20` | `0x20` |
| 无 PCF 降级 | 触摸导航可用 | 触摸导航可用 | 触摸导航可用 | 触摸导航可用 |

### 3.5 SD、PN532、UART、GPS

| 项目 | v1 | v2 | v3 | v4 |
| --- | --- | --- | --- | --- |
| SD CS | GPIO5 | GPIO10 | GPIO10 | GPIO10 |
| SD MOSI | GPIO23 | GPIO11 | GPIO11 | GPIO11 |
| SD MISO | GPIO19 | GPIO13 | GPIO13 | GPIO13 |
| SD SCLK | GPIO18 | GPIO12 | GPIO12 | GPIO12 |
| SD CD | 未定义 | GPIO38 | GPIO38 | GPIO38 |
| PN532 SCK | GPIO12 | GPIO12 | GPIO12 | GPIO12 |
| PN532 MISO | GPIO11 | GPIO11 | GPIO11 | GPIO11 |
| PN532 MOSI | GPIO13 | GPIO13 | GPIO13 | GPIO13 |
| PN532 SS | GPIO4 | GPIO4 | GPIO4 | GPIO4 |
| 通用 RX | GPIO16 | GPIO6 | GPIO6 | GPIO6 |
| 通用 TX | GPIO26 | GPIO3 | GPIO3 | GPIO3 |
| GPS RX | GPIO3 | GPIO47 | GPIO47 | GPIO47 |
| GPS TX | GPIO1 | GPIO48 | GPIO48 | GPIO48 |

注意：PN532 默认引脚和部分模块引脚可能与外设扩展实际接线相关，若硬件不同应在 `BoardConfig.h` 或构建参数中覆写。

### 3.6 CC1101、NRF24、IR、蜂鸣器

| 项目 | v1 | v2 | v3 | v4 |
| --- | --- | --- | --- | --- |
| CC1101 SCK | GPIO18 | GPIO12 | GPIO12 | GPIO12 |
| CC1101 MISO | GPIO19 | GPIO13 | GPIO13 | GPIO13 |
| CC1101 MOSI | GPIO23 | GPIO11 | GPIO11 | GPIO11 |
| CC1101 CS | GPIO27 | GPIO5 | GPIO5 | GPIO5 |
| SubGHz RX/GDO2 | GPIO16 | GPIO3 | GPIO3 | GPIO3 |
| SubGHz TX/GDO0 | GPIO26 | GPIO6 | GPIO6 | GPIO6 |
| NRF24 #1 CE/CSN | GPIO4 / GPIO5 | GPIO15 / GPIO4 | GPIO15 / GPIO4 | GPIO15 / GPIO4 |
| NRF24 #2 CE/CSN | GPIO26 / GPIO27 | GPIO47 / GPIO48 | GPIO47 / GPIO48 | GPIO47 / GPIO48 |
| NRF24 #3 CE/CSN | GPIO16 / GPIO17 | GPIO14 / GPIO21 | GPIO14 / GPIO21 | GPIO14 / GPIO21 |
| IR RX | GPIO4 | GPIO21 | GPIO21 | GPIO21 |
| IR TX | GPIO5 | GPIO14 | GPIO14 | GPIO14 |
| IR 默认载波 | 38 kHz | 38 kHz | 38 kHz | 38 kHz |
| 蜂鸣器默认 | `BUZZER_PIN -1` | `BUZZER_PIN -1`，可覆写 | 当前 `BoardConfig.h` 覆写为 GPIO2，active buzzer，DC on/off | 与 v3 逻辑一致，若硬件相同应使用 GPIO2 |

### 3.7 TFT_eSPI 配置

| 项目 | v1 `User_Setup v1.h` | v2 `User_Setup v2.h` | v3 IDF `tft_setup.h` | v4 IDF `tft_setup.h` |
| --- | --- | --- | --- | --- |
| 板型宏 | `BOARD_ESP32_DIV_V1` | `BOARD_ESP32_DIV_V2` | `BOARD_ESP32_DIV_V3` | `BOARD_ESP32_DIV_V4` |
| 面板选择宏 | 历史 setup 固定 | 历史 setup 固定 | `TFT_PANEL_ILI9341` | `TFT_PANEL_ST7796` |
| 驱动 | ILI9341 | ILI9341 | ILI9341 | ST7796 |
| 原生分辨率 | 320x240 | 320x240 | 320x240 | 480x320 |
| portrait 宏尺寸 | 240x320 | 240x320 | 240x320 | 320x480 |
| TFT MISO | GPIO12 | GPIO37 | GPIO37 | GPIO37 |
| TFT MOSI | GPIO13 | GPIO35 | GPIO35 | GPIO35 |
| TFT SCLK | GPIO14 | GPIO36 | GPIO36 | GPIO36 |
| TFT CS | GPIO15 | GPIO17 | GPIO17 | GPIO17 |
| TFT DC | GPIO2 | GPIO16 | GPIO16 | GPIO16 |
| TFT RST | GPIO0 | GPIO0 | -1 | -1 |
| TFT BL | GPIO4 | 固件用 `BACKLIGHT_PIN GPIO7` | GPIO7 | GPIO7 |
| SPI 频率 | 55 MHz | 27 MHz | 27 MHz | 27 MHz |
| SPI 读频率 | 未统一记录 | 20 MHz | 20 MHz | 20 MHz |
| SPI 端口 | `USE_HSPI_PORT` | `USE_HSPI_PORT` | TFT_eSPI 默认 SPI 实例 | TFT_eSPI 默认 SPI 实例 |

## 4. 启动流程与系统级能力

`setup()` 的核心初始化顺序：

1. 初始化串口 `115200`。
2. 初始化 TFT、屏幕旋转、背光 PWM 和启动 Logo。
3. 初始化 SD、设置存储、触摸校准参数。
4. 初始化 PCF8574 按键，若地址自动检测失败则降级。
5. 初始化 BLE/NimBLE 兼容层。
6. 在 v2/v3/v4 上初始化 BLE Rubber Ducky 服务；v1 因 `BOARD_HAS_ESP32S3 = 0` 不启用。
7. 启动 Wi-Fi/BLE 后台扫描任务。
8. 启动状态栏任务、触摸屏和主菜单。

系统状态栏能力：

- 电池图标：默认 `BATTERY_ADC_PIN = -1`，源码保留电压分压参数，但默认不读取真实 ADC。
- SD 状态：显示 SD 是否可用。
- Wi-Fi/BLE 数量：来自后台或前台扫描缓存。
- GPS 图标：Wardriver 运行时显示。
- 亮度：`0..255`，通过 PWM 控制。
- 触摸导航条：`TOUCH_BUTTON_CUE_ENABLED = 1` 时显示底部 5 个触摸按钮提示。

默认关闭或未完全硬件化的项：

- `shared.h` 默认 `BUZZER_PIN = -1`，表示不启用蜂鸣器；当前 `BoardConfig.h` 对 v3/v4 硬件上下文覆写为 GPIO2、active buzzer、DC on/off。
- `neopixelEnabled` 是设置项，README 提到 WS2812 NeoPixels，但当前核心配置中没有统一 NeoPixel 引脚定义。后续开发时应先确认硬件和驱动路径。

## 5. 菜单地图

主菜单共 8 项：

| 主菜单 | 子菜单 |
| --- | --- |
| WiFi | Packet Monitor、Beacon Spammer、WiFi Deauther、Probe Request Flood、Deauth Detector、WiFi Scanner、Captive Portal、Back to Main Menu |
| 2.4GHz | Scanner、Proto Kill、Back to Main Menu |
| More | IR Remote、RFID/NFC、GPS、Main Menu |
| Settings | 全屏设置页 |
| Bluetooth | BLE Jammer、BLE Spoofer、Sour Apple、Sniffer、BLE Scanner、BLE Rubber Ducky、Back to Main Menu |
| SubGHz | Replay Attack、SubGHz Jammer、Saved Profile、Back to Main Menu |
| Tools | Serial Monitor、Update Firmware、Touch Calibrate、SD File Manager、Back to Main Menu |
| About | 项目信息页 |

More 下的二级菜单：

| More 功能 | 子菜单 |
| --- | --- |
| IR Remote | Record、Saved Profile、Universal Controller、Back to Main Menu |
| RFID/NFC | Card Reader、Card Clone、Erase、Dump、Decode Access、Jam Reader、Tag Disrupt、Disrupt Emulate、Back to Main Menu |
| GPS | Wardriver、Satellite Scanner、Back to Main Menu |

## 6. Wi-Fi 功能

### 6.1 Packet Monitor

用途：实时监听 802.11 数据包，显示频道、数据包量、Deauth 数量和类似频谱/瀑布图的视图。

能力：

- 使用 ESP32 Wi-Fi promiscuous 模式监听。
- 支持 1 到 14 频道轮询或切换。
- 统计总包数、Deauth 帧数量和当前频道活动。
- 屏幕显示实时图形，便于观察流量密度。
- SD 可用时自动创建 PCAP 文件：`/captures/ptm_%04u.pcap`。
- PCAP 使用全局头和 radiotap 信息，保留信道频率、RSSI、时间戳、原始 802.11 帧片段。
- 退出功能时会关闭 promiscuous 模式并停止 PCAP 写入。

依赖与限制：

- 需要 ESP32 Wi-Fi radio。
- SD 不可用时仍可监控屏幕数据，但不会保存 PCAP。
- 前台功能运行时会暂停后台 Wi-Fi/BLE 扫描。

### 6.2 Beacon Spammer

用途：广播伪造 Beacon 帧，用于实验环境中测试扫描器、客户端列表和信道噪声响应。

能力：

- 构造原始 802.11 Beacon 帧。
- 支持随机、列表或自定义 SSID 模式。
- 使用随机源 MAC/BSSID 组合。
- 通过 UI 控制启动/停止、切换 SSID 模式和刷新输出。

限制：

- 属于实验室射频流量生成工具，应只在授权环境中使用。

### 6.3 WiFi Deauther

用途：在授权网络中生成 Deauthentication 帧，用于测试终端重连、检测系统和防护策略。

能力：

- 扫描或复用缓存 AP 列表。
- 按 SSID、BSSID、RSSI、信道选择目标。
- 对选中 AP 发送 Deauth 帧。
- 会启动 AP 模式，默认 SSID 为 `ESP32-DIV`，密码为 `deauth123`。
- 周期性检查目标信道，源码中存在约 15 秒信道重检逻辑。
- UI 提供目标列表、详情、启动/停止状态。

限制：

- 只应对自有或授权网络执行。
- 依赖 ESP32 802.11 raw frame 能力和当前 SDK 支持。

### 6.4 Probe Request Flood

用途：构造 Probe Request 帧，测试 AP、扫描器或监控系统对 Probe 流量的处理。

能力：

- 扫描 AP 后选择目标 SSID/信道。
- 构造 Probe Request 原始帧。
- 使用随机源 MAC。
- 支持 UI 控制启动/停止。

### 6.5 Deauth Detector

用途：监听 Deauth/Disassociation 帧，检测附近异常断连流量。

能力：

- promiscuous 模式监听管理帧。
- 按 AP 统计 Deauth/Disassoc 数量。
- 支持隐藏 SSID 或异常帧提示。
- 高 Deauth 数量时显示告警，源码中阈值表现为超过 5 次时强调。
- 屏幕显示 AP、RSSI、信道和异常状态。

### 6.6 WiFi Scanner

用途：扫描附近 Wi-Fi 网络并提供详细信息。

能力：

- 支持前台主动扫描。
- 设置中 `Auto Scan` 开启时会复用后台 Wi-Fi 扫描缓存。
- 列表显示 SSID、RSSI、信道、加密类型。
- 详情显示 BSSID、信号百分比、估算距离等。
- 可被 Captive Portal、Deauther、Probe Flood 等功能复用 AP 列表。

### 6.7 Captive Portal

用途：在授权测试中创建 SoftAP、DNS 和 WebServer，模拟门户登录流程，记录提交的测试凭据。

能力：

- SoftAP + DNS 劫持 + HTTP WebServer。
- 支持自定义门户 SSID。
- 支持扫描并克隆真实 AP 的 SSID。
- 可选 Evil Twin 流程，对克隆目标结合 Deauth 流量。
- 提供登录表单、凭据列表、导出和删除逻辑。
- 凭据同时写入 EEPROM 和 SD。

数据路径：

- EEPROM 大小：`1440`。
- SD 追加记录：`/captive_portal/captured.csv`。
- EEPROM 导出：`/captive_portal/eeprom_dump.csv`。
- AP 默认 SSID：`ESP32-DIV`。
- Deauth/AP 默认密码：`deauth123`。

限制：

- 只应在明确授权的社工演练或教学实验中使用。
- EEPROM 容量有限，源码提供导出和清空流程。

## 7. Bluetooth 与 BLE 功能

### 7.1 BLE Jammer

用途：使用 3 个 NRF24 模块产生 BLE 或经典 Bluetooth 相关信道干扰流量，用于射频抗扰度测试。

能力：

- 操作 3 个 RF24 radio。
- BLE 信道数组：`{2, 26, 80}`。
- 经典 Bluetooth 信道数组包含 `32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80`。
- RF24 设置：最大 PA、2Mbps、关闭 CRC、恒载波或随机信道跳变。
- UI 支持 BLE/Bluetooth 模式切换和启动/停止。

限制：

- 需要正确接入 3 个 NRF24。
- 属于射频干扰能力，只能在屏蔽或授权测试环境使用。

### 7.2 BLE Spoofer

用途：广播伪造 BLE 广告包，模拟指定厂商或设备类型。

能力：

- 支持 Apple、Samsung、Google 风格广告数据。
- 设备类型编号 `1..21`。
- 已见设备标签包括 AirPods/Beats 类、Galaxy Watch 4/5/6、Google Smart Ctrl。
- 广播类型编号 `1..5`，对应常见 BLE advertising type。
- 启动时生成随机广播地址。
- 可切换设备类型、广播类型和广告启停。

限制：

- 仅模拟广告层数据，不代表真实配对或完整服务实现。

### 7.3 Sour Apple

用途：广播 Apple 风格 BLE proximity 或 popup 触发类 payload，用于研究 Apple 设备对特定广告数据的 UI 响应。

能力：

- 使用 NimBLE 创建 BLE Advertising。
- 设置较高 TX power。
- 构造并刷新 Apple 风格 manufacturer data。
- 使用随机地址和随机字段。

限制：

- 只适合受控实验。公共场所广播可能影响他人设备体验。

### 7.4 Sniffer

用途：BLE 和经典 Bluetooth 设备扫描、跟踪与异常检测。

能力：

- BLE 与 BT 扫描轮换。
- 记录 MAC、RSSI、packet count、last seen、BLE 名称、Service UUID 等。
- 维护设备表并在屏幕上滚动显示。
- 检测逻辑包括：
  - 疑似 jamming 或 flooding。
  - BLE 随机 MAC 或 MAC spoofing。
  - Beacon spoofing。
  - manufacturer data 过长或格式异常。
  - 高包量或强 RSSI 异常。
- 串口命令包括 `FILTER MAC ...`、`FILTER SUSPICIOUS`、`RESET`。

### 7.5 BLE Scanner

用途：扫描附近 BLE 广播设备。

能力：

- active scan。
- 列表显示设备名、MAC、RSSI。
- 详情页显示 Tx Power、Service UUID、manufacturer data、service data 等字段。
- 设置中 `Auto Scan` 开启时，后台 BLE 扫描约每 15 秒更新缓存。
- 前台进入时可以复用后台缓存，也可手动刷新。

### 7.6 BLE Rubber Ducky

可用性：

- v2/v3/v4 支持。
- v1 不支持。v1 进入该菜单会显示 `This feature requires ESP32-S3.`。

用途：ESP32-S3 板型作为 BLE HID Keyboard，连接到目标测试设备后执行 SD 卡中的 DuckyScript。

运行路径：

- 运行时脚本目录：`/ducky`。
- 支持扩展名：`.duck`、`.txt`。
- 随仓示例在 `ducky scripts/`，需要放入 SD 卡 `/ducky` 才会被固件读取。

脚本元数据：

- 前 10 行 `REM` 注释中可解析：
  - `REM Name: ...`
  - `REM Description: ...`
- 如果无 `Name`，使用文件名作为脚本名。

支持命令：

| 命令 | 行为 |
| --- | --- |
| `REM` | 注释。 |
| `DELAY <ms>` | 等待指定毫秒，最大 clamp 到 600000。 |
| `DEFAULT_DELAY <ms>` / `DEFAULTDELAY <ms>` | 设置后续命令默认延迟。 |
| `WAIT_FOR <ms>` | 等待指定毫秒。 |
| `STRING <text>` | 输入 ASCII 字符串。 |
| `ENTER` / `RETURN` | 回车。 |
| `GUI` / `WINDOWS` / `WIN` | 默认发送 Win+R，也可跟随一个按键。 |
| `CTRL` / `CONTROL` / `ALT` / `SHIFT` 组合 | 发送组合键。 |
| `TAB`、`ESC`、`SPACE`、`BACKSPACE`、`DELETE` | 常用单键。 |
| `UP`、`DOWN`、`LEFT`、`RIGHT`、`HOME`、`END`、`PGUP`、`PAGEUP`、`PGDN`、`PAGEDOWN` | 导航键。 |
| `F1` 到 `F12` | 功能键。 |
| `REPEAT <n>` | 重复发送 ENTER n 次。 |

限制：

- 需要 BLE 已连接。
- 需要 SD 卡可挂载。
- ASCII 输入覆盖英文字母、数字和常用标点，不是完整键盘布局/Unicode 输入引擎。

## 8. 2.4GHz NRF24 功能

### 8.1 Scanner

用途：使用 NRF24 RPD 寄存器扫描 2.4GHz 信道活动。

能力：

- 扫描 NRF24 的 128 个信道，约覆盖 2.400 到 2.528 GHz。
- 读取 RPD 以估算信道是否存在能量。
- 执行噪声基线校准。
- 屏幕显示活动信道数、峰值信道、强度柱状图或频谱图。
- 辅助识别 BLE、Wi-Fi Ch1/6/11、RC/Video 等常见区域。

### 8.2 Proto Kill

用途：使用 3 个 NRF24 进行多协议 2.4GHz 干扰/压力测试。

模式与信道：

| 模式 | 信道数组 |
| --- | --- |
| BLE | `{2, 26, 80}` |
| Bluetooth | `{32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80}` |
| WiFi | `{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}` |
| USB Wireless | `{40, 50, 60}` |
| Video | `{70, 75, 80}` |
| RC | `{1, 3, 5, 7}` |
| Zigbee | `{11, 15, 20, 25}` |
| NRF24 | `{76, 78, 79}` |

能力：

- UI 支持模式前进/后退。
- Toggle 启停干扰。
- RF24 使用高功率、关闭 CRC、固定载波或随机信道跳变。

限制：

- 需要 3 个 NRF24 都正确初始化。
- 属于主动射频干扰工具，只能在授权环境中使用。

## 9. Sub-GHz 功能

Sub-GHz 功能基于 CC1101 和 RCSwitch，覆盖 OOK/ASK 类简单遥控信号的接收、重放、保存和干扰测试。

通用频率列表：

```text
300000000, 303875000, 304250000, 310000000, 314000000, 315000000,
318000000, 390000000, 418000000, 433075000, 433420000, 433920000,
434420000, 434775000, 438900000, 868350000, 915000000, 925000000
```

全局默认频点常量：`SUBGHZ_DEFAULT_FREQ = 433920000UL`。

### 9.1 Replay Attack

用途：接收、显示、保存和重放 Sub-GHz OOK 遥控信号。

能力：

- 初始化 CC1101 和 RCSwitch。
- 可手动切换频率，也可启用自动扫频。
- 显示 RSSI、当前频率、接收值、bit length、protocol。
- 捕获到有效 RCSwitch 解码后可直接发送。
- 可给信号命名并保存 profile。
- EEPROM 最多保存 5 个当前 profile，满后可导出到 SD。
- 屏幕提供类似 FFT/活动图形的反馈。

Profile 字段：

- `frequency`：频率，Hz。
- `value`：RCSwitch 解码值。
- `bitLength`：bit 数。
- `protocol`：RCSwitch protocol。
- `name[16]`：用户命名。

### 9.2 Saved Profile

用途：管理和发射已保存的 Sub-GHz profile。

能力：

- 扫描 `/subghz/profiles_current.bin` 和 `/subghz/profiles_*.bin`。
- 合并展示当前 profile 和归档 profile。
- 选择 profile 后设置 CC1101 频率，调用 RCSwitch 发送。
- 可删除选中的 profile，删除当前文件内项时会重写文件。

SD 文件格式：

- 当前文件：`/subghz/profiles_current.bin`。
- 归档文件：`/subghz/profiles_%04u.bin`。
- 文件头 magic：`0x315A4753`。
- version：`1`。
- header 包含 `count`、`profileSize`。

### 9.3 SubGHz Jammer

用途：CC1101 Sub-GHz 频段干扰/抗扰度测试。

能力：

- 可在频率列表间手动切换。
- 可启用自动 sweep。
- 可启动/停止发射。
- 工具栏存在 noise/continuous 类模式切换。
- 默认 target index 在源码中为 5，对应 315 MHz；全局默认频率常量仍为 433.92 MHz。

限制：

- 属于主动射频发射/干扰工具，必须遵守本地法规并仅在授权环境中使用。

## 10. IR 功能

IR 功能基于 IRremoteESP8266 风格接口，支持红外接收、原始波形保存、回放和通用遥控器。

### 10.1 Record

用途：录制红外遥控按键，显示波形，并保存为 SD 文件。

能力：

- 使用 `IR_RX_PIN` 接收，`IR_TX_PIN` 发射。
- 默认载波：38 kHz。
- 支持最多 512 个 raw pulse。
- 解码并显示 protocol、value、bits。
- 显示实时 scope/波形图。
- 可设置 repeat count 并重放当前捕获。
- 可输入名称保存。

保存路径与格式：

- 目录：`/ir`。
- 文件名：`/ir/ir_%04u.bin`。
- magic：`0x31525249`。
- version：`1`。
- header 包含 `khz`、`rawLen`、`decodeType`、`bits`、`value`、`name[16]`。
- header 后跟 `rawLen` 个 `uint16_t` raw pulse。

### 10.2 Saved Profile

用途：浏览、回放、删除已保存 IR 捕获。

能力：

- 扫描 `/ir` 下 `ir_*.bin`。
- 读取 header 并展示名称、协议、bit 数和载波。
- 发射选中 profile。
- 删除时有确认流程。

### 10.3 Universal Controller

用途：使用内置或 SD 导入的遥控器 profile，提供遥控器式 UI。

能力：

- 内置 profile 覆盖常见 Samsung TV、NEC media box、Sony TV、Xbox 360、Epson Projector、JVC VCR、Denon AVR、Panasonic、Pioneer、DISH、GI Cable 等。
- 支持从 SD 导入：
  - 单文件：`/ir_profiles.json`。
  - 多文件目录：`/ir_profiles/*.json`。
- 最大 profile 数：约 500。
- 支持 category、brand、profile 浏览。
- 遥控器按键包括 Power、Mute、Back、VolUp、VolDn、ChUp、ChDn、OK、方向键等。

支持的协议 token：

- `NEC`
- `NECLIKE`
- `SONY` / `SIRC`
- `SAMSUNG` / `SAMSUNG32` / `SAMSUNG36`
- `LG` / `LG2`
- `JVC`
- `DENON`
- `PANASONIC` / `KASEIKYO`
- `RC5` / `RC5X`
- `RC6`
- `PIONEER`
- `DISH`
- `GICABLE`
- `EPSON`

JSON profile 结构要点：

```json
{
  "profiles": [
    {
      "name": "Example TV",
      "protocol": "NEC",
      "category": "TV",
      "brand": "Example",
      "bits": 32,
      "keys": {
        "Power": "0x20DF10EF",
        "VolUp": "0x20DF40BF"
      }
    }
  ]
}
```

## 11. RFID/NFC 功能

RFID/NFC 功能基于 Adafruit PN532 SPI，默认使用 ISO14443A 读卡。进入 RFID/NFC 功能时会探测 PN532，如果未找到会显示 `PN532 not found. Check SPI wiring/pins.`。

支持识别的 tag 类型：

- `MIFARE_CLASSIC`
- `MIFARE_ULTRALIGHT`
- `NTAG`
- `MIFARE_DESFIRE`
- `UNKNOWN`

默认密钥和读取策略：

- Classic 默认尝试 Key A `FF FF FF FF FF FF`，部分流程也尝试 Key B 和全 0 key。
- Ultralight/NTAG 按页读取。
- DESFire 当前只做 UID/类型识别，不实现完整 APDU 操作。

### 11.1 Card Reader

用途：读取并展示标签基本信息。

能力：

- 读取 UID 和 UID 长度。
- 检测 tag 类型。
- Classic：尝试读取 sector 0 block 0。
- Ultralight/NTAG：读取 pages 4 到 7 作为样本。
- DESFire：展示 UID，提示完整 DESFire 操作需要额外 APDU。

### 11.2 Card Clone

用途：授权环境下将支持的标签数据复制到空白/可写标签。

流程：

1. 读取源卡。
2. 分类并读取源数据。
3. 用户确认。
4. 放置目标空白卡并写入。

能力：

- Classic：读取 16 个 sector，每个 sector 的 data blocks 0 到 2，跳过 trailer block。
- Classic 写入时尝试默认 Key A、Key B 和全 0 key。
- Gen2 magic Classic 卡检测到时可尝试 UID block 写入。
- Ultralight：最多处理 16 页。
- NTAG：最多处理 36 页。

限制：

- 不等同于完整高频卡复制器，不处理受保护密钥体系和 DESFire 应用。
- 写 UID 依赖 magic 卡特性。

### 11.3 Erase

用途：擦除受支持 Classic 标签的一小部分数据。

能力：

- 只针对 MIFARE Classic。
- 用默认 Key A 认证 block 4。
- 将 block 4 的 16 字节写为 0。

限制：

- Ultralight/NTAG 不会被此模式擦除。
- 不是全卡擦除。

### 11.4 Dump

用途：读取标签内容并通过串口输出完整 hex。

能力：

- Classic：尝试读取 block 0 到 63。
- Ultralight：最多 16 页。
- NTAG：最多 36 页。
- 屏幕显示进度和摘要。
- 完整 hex 输出在 Serial Monitor。
- 如果标签离场或认证失败，会显示 partial/skipped 信息。

### 11.5 Decode Access

用途：读取 MIFARE Classic sector trailer 并展示 access bits 原始值。

能力：

- 读取 sector 1 trailer，即 block 7。
- 尝试 Key A，不行再尝试 Key B。
- 显示 raw trailer bytes、C1/C2/C3 和组合 access condition。

限制：

- 只针对 Classic sector trailer。
- 源码提示需结合 MFC access-bit tables 进一步解释。

### 11.6 Jam Reader

用途：PN532 target mode 下对外部 reader 交互进行干扰式响应测试。

能力：

- 进入 ISO14443A target mode。
- 等待外部 reader。
- 对 reader 发送快速/异常响应。
- 停止后重新配置 SAM，恢复 reader mode。

### 11.7 Tag Disrupt

用途：授权测试中对 Classic 标签 sector trailer 写入扰动模式，验证读写器或标签恢复能力。

能力：

- 只针对 MIFARE Classic。
- 目标 sector trailer blocks：3、7、11、15、19、23、27、31、35、39、43、47、51、55、59、63。
- 使用默认 Key A 认证。
- 轮换写入 `FF`、`00`、随机等模式。
- 源码明确提示错误 access bits 可能使标签不可用。

### 11.8 Disrupt Emulate

用途：PN532 target mode 下的扰动/模拟响应流程。

能力：

- 进入 target mode。
- 生成 `FF`、`00`、随机 payload 响应。
- 停止后重新配置 SAM。

### 11.9 RFID 超时

| 场景 | 超时 |
| --- | --- |
| 普通标签等待 | 约 20 秒 |
| Clone 阶段 | 约 45 秒 |
| target mode 等待 | 约 120 秒 |
| Tag Disrupt Classic 等待 | 约 12 秒窗口 |

## 12. GPS 功能

GPS 功能使用 UART2，默认波特率 9600，解析 NMEA 句子。支持 `GSV`、`GSA`、`GGA`、`RMC` 等信息源。

### 12.1 Satellite Scanner

用途：显示 GNSS 卫星、信号和定位状态。

能力：

- 解析多星座卫星信息。
- 支持 GPS、GLONASS、Galileo、BeiDou、QZSS 等系统标识。
- 显示 sky plot。
- 显示卫星 SNR 列表。
- 显示 fix、satellites、HDOP、经纬度、高度等定位指标。
- UI 可在 combined、sky、list 等视图间切换。

### 12.2 Wardriver

用途：将 GNSS 位置与 Wi-Fi/BLE 观测数据写入 CSV，用于无线覆盖调查和后续导入分析。

能力：

- 需要 SD 卡。
- 可配置 radio mode：WiFi、BLE、Both。
- 默认扫描间隔：`WARDRIVE_SCAN_INTERVAL_MS = 4200` ms。
- UI 刷新间隔：`WARDRIVE_UI_INTERVAL_MS = 120` ms。
- 默认每次 Wi-Fi 扫描最大 AP 数：`WARDRIVE_MAX_APS_PER_SCAN = 48`。
- 支持前台运行，也可交给后台继续记录。
- 状态栏显示 GPS/Wardriver 运行状态。

CSV 文件：

- 文件名：`/wd_%lu.csv`。
- header：

```csv
epoch_ms,utc,date,lat,lon,alt_m,fix,sats,hdop,radio,ssid,bssid,ch,rssi_dbm,auth
```

WiGLE 上传：

- 配置文件：`/config/wigle.txt`。
- 第 1 行：WiGLE API name。
- 第 2 行：WiGLE token。
- 第 3 行：Wi-Fi SSID，可为空、`-` 或 `auto` 以使用已存 Wi-Fi 信息。
- 第 4 行：Wi-Fi password。
- 临时转换 CSV：`/wd_wigle_upload.csv`。
- 上传目标：`api.wigle.net` 的 `/api/v2/file/upload`。
- User-Agent：`ESP32-DIV-Wardrive/1`。

## 13. Tools 与系统维护功能

### 13.1 Serial Monitor

用途：在 TFT 上显示串口数据，便于现场调试。

能力：

- 支持波特率：9600、19200、38400、57600、115200。
- UI 可切换 baud。
- 可暂停/继续显示。
- 可显示 printable ASCII，处理 CR/LF 和滚动。
- 调整 baud 时调用 `Serial.begin(baudRates[baudIndex])`。

### 13.2 Update Firmware

固件更新提供两种方式。

SD Update：

- 固件文件路径固定为 `/firmware.bin`。
- 使用 Arduino `Update` 写入 stream。
- 写入成功后重启。

Web OTA：

- 先选择 Wi-Fi 网络。
- 使用屏幕键盘输入 Wi-Fi 密码。
- 连接后启动 mDNS：`esp32.local`。
- 启动 80 端口 WebServer。
- Web 登录账号密码：`admin` / `admin`。
- 浏览器上传固件，POST 到 `/update`。

### 13.3 Touch Calibrate

用途：四角触摸校准 XPT2046。

能力：

- 引导点击四个角。
- 生成 raw min/max。
- 保存到 `/config/settings.json` 的 `touch` 节点。
- 保存时带 `board` profile ID，读取时只对匹配板型应用。
- 非 CYD 板存在 Y 轴赋值翻转处理。

### 13.4 SD File Manager

用途：浏览和删除 SD 卡文件。

能力：

- 从 `/` 根目录开始浏览。
- 目录优先排序。
- 支持 `..` 返回上级。
- 打开目录。
- 展示文件信息。
- 删除文件。
- 删除空目录。
- 刷新当前目录。

限制：

- 不支持重命名。
- 不支持新建目录。
- 不支持复制/移动文件。

## 14. Settings 功能

Settings 是全屏设置页，不是普通列表子菜单。

设置项：

| 项 | 取值 | 行为 |
| --- | --- | --- |
| Brightness | `0..255` | 立即调用背光 PWM。 |
| Theme | Dark / Light | 立即更新 UI palette。 |
| Accent | Orange、Green、Red、Cyan、Purple、Yellow、White | 切换强调色。 |
| NeoPixel | bool | 保存设置状态，硬件驱动需按板确认。 |
| Auto Scan | bool | 同步控制 `autoWifiScan` 和 `autoBleScan`。 |

设置文件路径：`/config/settings.json`。

保存结构：

```json
{
  "board": "ESP32_DIV_V2",
  "brightness": 128,
  "theme": 0,
  "accentColor": 0,
  "neopixelEnabled": false,
  "autoWifiScan": true,
  "autoBleScan": true,
  "touch": {
    "xMin": 280,
    "xMax": 3850,
    "yMin": 320,
    "yMax": 3750
  }
}
```

读取规则：

- 启动时先应用当前板型默认触摸参数。
- 如果 SD 不可用，则使用默认值。
- 如果设置文件存在且根节点 `board` 与当前 `TOUCH_PROFILE_ID` 匹配，则使用保存校准。
- 如果 `autoWifiScan` 和 `autoBleScan` 不一致，会合并为同一状态。

## 15. About 功能

About 页显示：

- 项目名：`ESP32-DIV`。
- 固件版本：`v1.7.0`。
- 项目/作者相关信息。
- 当前板型信息可通过启动日志和配置常量识别。

## 16. 数据目录与持久化总表

| 路径 | 写入者 | 内容 |
| --- | --- | --- |
| `/config/settings.json` | Settings、Touch Calibrate | 亮度、主题、强调色、NeoPixel、Auto Scan、触摸校准。 |
| `/config/wigle.txt` | Wardriver WiGLE 设置 | API name、token、可选 Wi-Fi SSID/password。 |
| `/captures/ptm_%04u.pcap` | Packet Monitor | Wi-Fi PCAP 抓包文件。 |
| `/captive_portal/captured.csv` | Captive Portal | 门户表单提交记录。 |
| `/captive_portal/eeprom_dump.csv` | Captive Portal | EEPROM 凭据导出。 |
| `/ducky/*.duck`、`/ducky/*.txt` | BLE Ducky | BLE HID 脚本。 |
| `/subghz/profiles_current.bin` | SubGHz Replay/Saved Profile | 当前 Sub-GHz profile 集合。 |
| `/subghz/profiles_%04u.bin` | SubGHz Replay/Saved Profile | 归档 Sub-GHz profile 集合。 |
| `/ir/ir_%04u.bin` | IR Record | IR raw 捕获与元数据。 |
| `/ir_profiles.json` | IR Universal Controller | SD 遥控器 profile 单文件导入。 |
| `/ir_profiles/*.json` | IR Universal Controller | SD 遥控器 profile 多文件导入。 |
| `/wd_%lu.csv` | Wardriver | GNSS + Wi-Fi/BLE 观测日志。 |
| `/wd_wigle_upload.csv` | Wardriver WiGLE 上传 | WiGLE 上传临时 CSV。 |
| `/firmware.bin` | Update Firmware | SD 固件更新输入文件。 |

## 17. 后台任务与资源协调

后台能力：

- Wi-Fi 后台扫描：设置 `autoWifiScan` 打开且设备空闲时更新缓存。
- BLE 后台扫描：设置 `autoBleScan` 打开时约 15 秒周期更新缓存。
- 状态栏任务：显示电池、SD、Wi-Fi/BLE 数量、GPS/Wardriver 状态。
- Wardriver 可将扫描记录任务交给后台继续执行。

前台资源协调：

- 进入射频类前台功能前通常会暂停后台 radio 任务，避免 Wi-Fi/BLE/NRF24/CC1101 资源冲突。
- SD 挂载逻辑在多个模块内各自实现，通常会优先尝试 `SD_CS`，再尝试兼容旧定义 `SD_CS_PIN`。
- v2/v3/v4 定义 `SD_CD = 38`，部分模块会检测卡插入状态。

## 18. 构建、刷机与库依赖上下文

板型选择：

- IDF 工程板型入口是 `components/esp32_div_board/include/BoardConfig.h`。
- 当前文件显式启用 `BOARD_ESP32_DIV_V3`；构建 v4 时应只启用 `BOARD_ESP32_DIV_V4`。
- 构建 v1/v2 时应只启用对应 `BOARD_ESP32_DIV_V1` 或 `BOARD_ESP32_DIV_V2`，或通过构建 flags 定义对应宏。
- 源码防止同时选择多个板型：`shared.h` 中多个 `BOARD_*` 同时定义会触发编译错误。
- 如果所有 `BOARD_*` 都未定义，当前 IDF 迁移代码会默认到 `BOARD_ESP32_DIV_V3`。

TFT_eSPI：

- v1 使用 `Libraries/User_Setup v1.h`。
- v2 使用 `Libraries/User_Setup v2.h`。
- v3/v4 使用 `components/esp32_div_board/include/tft_setup.h` 作为项目级 TFT_eSPI setup。
- v3 默认 `TFT_PANEL_ILI9341`，启用 `ILI9341_DRIVER`，尺寸 `240x320`。
- v4 默认 `TFT_PANEL_ST7796`，启用 `ST7796_DRIVER`，尺寸 `320x480`。
- 后续不要在 TFT_eSPI 库源码里散落改动；优先保持 `BoardConfig.h` 和 `tft_setup.h` 为板级显示配置入口。

主要 Arduino/ESP32 库：

- `TFT_eSPI`
- `PCF8574`
- `XPT2046_Touchscreen`
- `ArduinoJson`
- `SD`
- `Update`
- `WebServer`
- `DNSServer`
- `ESPmDNS`
- `NimBLE` 兼容层，源码通过 `BleCompat.h` 暴露 `BLEDevice/BLEScan` 等别名。
- `RF24` / `nRF24L01`
- `ELECHOUSE_CC1101_SRC_DRV`
- `RCSwitch`
- `Adafruit_PN532`
- `IRremoteESP8266` 相关接口
- `arduinoFFT`

预编译固件：

- `docs/firmware/ESP32-DIV-v1-v1.7.0.bin`
- `docs/firmware/ESP32-DIV-v1-v1.7.0-merged.bin`
- `docs/firmware/ESP32-DIV-v2-v1.7.0.bin`
- `docs/firmware/ESP32-DIV-v2-v1.7.0-merged.bin`
- `Pre-compiled Bin/ESP32-DIV-v1-v1.7.0.bin`
- `Pre-compiled Bin/ESP32-DIV-v2-v1.7.0.bin`

## 19. 已知差异、限制和后续开发注意事项

- v1/v2/v3/v4 菜单基本一致，但 BLE Rubber Ducky 只在 ESP32-S3 板型 v2/v3/v4 可用。
- README 和历史文档主要以 ESP32-S3/v2 描述项目，v1 实际仍由源码支持，但没有 ESP32-S3 特性。
- v3/v4 的用户指定差异只有 TFT 模组：v3 是 3.2 寸 ILI9341，v4 是 4.0 寸 ST7796；除显示驱动和分辨率外，板级外设逻辑应保持一致。
- `RFID/NFC` 入口源码中有历史命名痕迹，但 PN532 存在时执行的是实际 RFID/NFC 功能。
- PN532、GPS、NRF24、CC1101、IR 等外设必须接线正确；固件层只做部分探测和错误提示。
- `BATTERY_ADC_PIN` 默认 -1，电池电压显示不是默认可靠输入。
- `BUZZER_PIN` 在 `shared.h` 中默认 -1；当前 `BoardConfig.h` 为 v3/v4 硬件上下文启用 GPIO2 active buzzer。
- NeoPixel 有设置项，但后续开发应先确认硬件引脚和驱动代码。
- SD 卡是许多功能的增强或必需条件：PCAP、Captive Portal 导出、IR 保存、Sub-GHz profile、Ducky、Wardriver、固件 SD 更新都依赖 SD。
- Captive Portal、Deauth、Probe Flood、Jammer、Tag Disrupt 等属于主动测试能力，后续 UI 或 AI 自动化流程应默认加入授权确认和范围限制。
- 多个模块各自实现 SD 挂载和目录创建，后续重构可考虑统一 SD 服务，但应谨慎处理 v1 与 v2/v3/v4 不同 CS/CD 引脚。
- 触摸校准保存和读取依赖 `TOUCH_PROFILE_ID`，跨板复用 SD 卡时不会直接套用另一板的触摸数据。
- IR 与 NRF24 部分默认引脚存在重叠注释，硬件扩展组合使用时应按实际板子重新确认引脚。
- SubGHz Jammer 默认目标 index 与全局默认频率常量不同，后续修改 UI 文案时不要误认为所有 SubGHz 默认都是 433.92 MHz。

## 20. ESP32-DIV v1/v2/v3/v4 Memory Bank

本节是给后续 AI 直接读取和复用的项目记忆库，已合并 `C:\projects\ESP32-DIV-main\docs\ESP32-DIV-v1-v2-功能设计文档.md` 中的 v1/v2 memory bank，并补充当前 IDF 工程的 v3/v4 事实。若上下文有限，优先读取本节，再按需要回查前文详细章节和源码。

### 20.1 项目身份

| 记忆项 | 内容 |
| --- | --- |
| 项目类型 | ESP-IDF + Arduino 兼容组件迁移工程；历史来源是 Arduino/ESP32 固件项目。 |
| 当前主代码目录 | `components/esp32_div/`。 |
| 当前板级入口 | `components/esp32_div_board/include/BoardConfig.h`。 |
| 当前主入口 | `components/esp32_div/src/app.cpp`、`main/main.cpp`。 |
| 历史 Arduino 主固件目录 | `ESP32-DIV/`。 |
| 历史 Arduino 主入口 | `ESP32-DIV/ESP32-DIV.ino`。 |
| 当前固件版本 | `ESP32DIV_VERSION = "v1.7.0"`。 |
| 当前 IDF 默认板型 | `BOARD_ESP32_DIV_V3`。 |
| 历史 Arduino 默认板型 | `BOARD_ESP32_DIV_V2`。 |
| 支持板型 | `BOARD_ESP32_DIV_V1`、`BOARD_ESP32_DIV_V2`、`BOARD_ESP32_DIV_V3`、`BOARD_ESP32_DIV_V4`。 |
| 设计定位 | 面向授权环境的多频段无线安全研究、教学和设备维护工具。 |
| 安全边界 | 主动发射、干扰、Deauth、Portal、RFID 写入等能力必须保留授权使用提示和范围限制。 |

### 20.2 权威源码与配置

| 场景 | 当前 IDF 工程优先参考 | 历史 Arduino 参考 |
| --- | --- | --- |
| 板型选择 | `components/esp32_div_board/include/BoardConfig.h` | `ESP32-DIV/BoardConfig.h` |
| 板级名称、引脚、功能开关 | `components/esp32_div/include/shared.h` | `ESP32-DIV/shared.h` |
| TFT_eSPI 项目配置 | `components/esp32_div_board/include/tft_setup.h` | `Libraries/User_Setup v1.h`、`Libraries/User_Setup v2.h` |
| 主菜单、子菜单、启动流程 | `components/esp32_div/src/app.cpp` | `ESP32-DIV/ESP32-DIV.ino` |
| 设置保存和加载 | `components/esp32_div/src/SettingsStore.cpp` | `ESP32-DIV/SettingsStore.*` |
| UI、状态栏、触摸校准、SD 文件管理 | `components/esp32_div/src/utils.cpp` | `ESP32-DIV/utils.cpp` |
| Wi-Fi、Captive Portal、OTA | `components/esp32_div/src/wifi.cpp` | `ESP32-DIV/wifi.cpp` |
| BLE、NRF24、BLE 后台扫描 | `components/esp32_div/src/bluetooth.cpp` | `ESP32-DIV/bluetooth.cpp` |
| Sub-GHz CC1101 | `components/esp32_div/src/subghz.cpp` | `ESP32-DIV/subghz.cpp` |
| IR | `components/esp32_div/src/ir.cpp` | `ESP32-DIV/ir.cpp` |
| RFID/NFC | `components/esp32_div/src/rfid.cpp` | `ESP32-DIV/rfid.cpp` |
| GPS/Wardriver/WiGLE | `components/esp32_div/src/gps.cpp` | `ESP32-DIV/gps.cpp` |
| BLE Rubber Ducky | `components/esp32_div/src/ducky.cpp` | `ESP32-DIV/ducky.cpp` |

### 20.3 四板固定事实

| 维度 | v1 | v2 | v3 | v4 |
| --- | --- | --- | --- | --- |
| 板型宏 | `BOARD_ESP32_DIV_V1` | `BOARD_ESP32_DIV_V2` | `BOARD_ESP32_DIV_V3` | `BOARD_ESP32_DIV_V4` |
| 板名 | `ESP32-DIV V1 ESP32U` | `ESP32-DIV V2` | `ESP32-DIV V3` | `ESP32-DIV V4` |
| 主控能力 | 经典 ESP32，`BOARD_HAS_ESP32S3 = 0` | ESP32-S3，`BOARD_HAS_ESP32S3 = 1` | ESP32-S3，`BOARD_HAS_ESP32S3 = 1` | ESP32-S3，`BOARD_HAS_ESP32S3 = 1` |
| 默认编译 | 否，需显式启用 | 历史 Arduino 默认 | 当前 IDF 默认 | 当前 IDF 可切换目标 |
| BLE Rubber Ducky | 不支持，应提示需要 ESP32-S3 | 支持 | 支持 | 支持 |
| TFT_eSPI 配置 | `Libraries/User_Setup v1.h` | `Libraries/User_Setup v2.h` | `components/esp32_div_board/include/tft_setup.h` | `components/esp32_div_board/include/tft_setup.h` |
| TFT 面板 | ILI9341 | ILI9341 | 3.2 寸 ILI9341，MSP3218 | 4.0 寸 ST7796/ST7796S，MSP4021 |
| TFT 宏尺寸 | 240x320 | 240x320 | 240x320 | 320x480 |
| 触摸 profile | `ESP32_DIV_V1` | `ESP32_DIV_V2` | `ESP32_DIV_V3` | `ESP32_DIV_V4` |
| TFT 旋转 | `TFT_ROTATION 0` | 当前 IDF 为 `TFT_ROTATION 0`，历史文档曾记录 v2 为 `2` | `TFT_ROTATION 0` | `TFT_ROTATION 0` |
| 背光 | GPIO32 | GPIO7 | GPIO7 | GPIO7 |
| SD CS | GPIO5 | GPIO10 | GPIO10 | GPIO10 |
| SD CD | 未定义 | GPIO38 | GPIO38 | GPIO38 |

### 20.4 功能能力矩阵

| 功能域 | v1 | v2 | v3 | v4 | AI 修改提醒 |
| --- | --- | --- | --- | --- | --- |
| Wi-Fi | 支持 | 支持 | 支持 | 支持 | 注意前后台扫描和 promiscuous/raw frame 状态清理。 |
| BLE 工具 | 支持 | 支持 | 支持 | 支持 | v2/v3/v4 额外具备 BLE HID Ducky 能力。 |
| BLE Rubber Ducky | 不支持 | 支持 | 支持 | 支持 | 由 `FEATURE_BLE_DUCKY = BOARD_HAS_ESP32S3` 控制，不能在 v1 上假定可用。 |
| 2.4GHz NRF24 | 支持 | 支持 | 支持 | 支持 | 需要 3 个 NRF24 模块，检查 CE/CSN 映射。 |
| Sub-GHz CC1101 | 支持 | 支持 | 支持 | 支持 | 检查频率、GDO、SPI 引脚和当地法规。 |
| IR | 支持 | 支持 | 支持 | 支持 | IR RX/TX 与部分扩展引脚可能存在组合冲突。 |
| RFID/NFC | 支持 | 支持 | 支持 | 支持 | PN532 不存在时必须保留清晰错误提示。 |
| GPS/Wardriver | 支持 | 支持 | 支持 | 支持 | Wardriver 依赖 SD，WiGLE 凭据来自 `/config/wigle.txt`。 |
| Settings/Tools/About | 支持 | 支持 | 支持 | 支持 | 设置保存必须兼容 `/config/settings.json`。 |

### 20.5 运行时数据和 SD 路径记忆

| 路径 | 记忆 |
| --- | --- |
| `/config/settings.json` | 亮度、主题、强调色、NeoPixel、Auto Scan、触摸校准。 |
| `/config/wigle.txt` | WiGLE API name、token、可选 Wi-Fi SSID/password。 |
| `/captures/ptm_%04u.pcap` | Packet Monitor PCAP。 |
| `/captive_portal/captured.csv` | Captive Portal 表单提交记录。 |
| `/captive_portal/eeprom_dump.csv` | Captive Portal EEPROM 凭据导出。 |
| `/ducky/*.duck`、`/ducky/*.txt` | BLE Ducky 脚本，只有 v2/v3/v4 可执行。 |
| `/subghz/profiles_current.bin` | 当前 Sub-GHz profile 集合。 |
| `/subghz/profiles_%04u.bin` | 归档 Sub-GHz profile 集合。 |
| `/ir/ir_%04u.bin` | IR raw 捕获。 |
| `/ir_profiles.json`、`/ir_profiles/*.json` | 通用遥控器 SD 导入 profile。 |
| `/wd_%lu.csv` | Wardriver GNSS + Wi-Fi/BLE 观测日志。 |
| `/wd_wigle_upload.csv` | Wardriver WiGLE 上传临时 CSV。 |
| `/firmware.bin` | SD 固件更新输入文件。 |

### 20.6 v3/v4 核心约束

- v3 与 v4 是同一套板子逻辑，唯一区别是 TFT 屏幕尺寸和驱动 IC。
- v3 使用 3.2 寸 LCDWiki ILI9341 SPI 模组，资料页：<https://www.lcdwiki.com/zh/3.2inch_SPI_Module_ILI9341_SKU:MSP3218>。
- v4 使用 4.0 寸 LCDWiki ST7796 SPI 模组，资料页：<https://www.lcdwiki.com/zh/4.0inch_SPI_Module_ST7796>。
- v3/v4 都是 ESP32-S3 能力板，`BOARD_HAS_ESP32S3 = 1`，因此 BLE Rubber Ducky 可用。
- v3/v4 都使用 PCF8574 物理按键、XPT2046 触摸、GPIO7 背光、GPIO2 active buzzer、同一套 SD/PN532/GPS/CC1101/NRF24/IR 引脚。
- 后续代码必须保留 `BOARD_ESP32_DIV_V3` 与 `BOARD_ESP32_DIV_V4` 两个独立板型宏；不要把 v4 简化成“v3 + 手动改 TFT_PANEL”。

### 20.7 v3/v4 当前代码事实

| 项目 | v3 | v4 |
| --- | --- | --- |
| 板型宏 | `BOARD_ESP32_DIV_V3` | `BOARD_ESP32_DIV_V4` |
| 板名 | `ESP32-DIV V3` | `ESP32-DIV V4` |
| 触摸 profile | `ESP32_DIV_V3` | `ESP32_DIV_V4` |
| 面板选择 | `TFT_PANEL_ILI9341` | `TFT_PANEL_ST7796` |
| TFT_eSPI 驱动 | `ILI9341_DRIVER` | `ST7796_DRIVER` |
| TFT 宏尺寸 | 240x320 | 320x480 |
| 触摸默认 raw | X `280..3850`，Y `320..3750` | X `280..3850`，Y `320..3750` |
| PCF8574 地址 | `0x20` | `0x20` |
| PCF8574 按键 | UP P3、DOWN P5、LEFT P6、RIGHT P4、SELECT P7 | 与 v3 相同 |
| TFT SPI | MISO 37、MOSI 35、SCLK 36、CS 17、DC 16、RST -1、BL 7 | 与 v3 相同 |
| Touch SPI | CS 18、MOSI 35、MISO 37、CLK 36、IRQ 255 | 与 v3 相同 |
| SD | CS 10、MOSI 11、MISO 13、SCLK 12、CD 38 | 与 v3 相同 |
| PN532 | SCK 12、MISO 11、MOSI 13、SS 4 | 与 v3 相同 |
| GPS UART2 | RX 47、TX 48、9600 baud | 与 v3 相同 |
| CC1101 | SCK 12、MISO 13、MOSI 11、CS 5、GDO0 6、GDO2 3 | 与 v3 相同 |
| NRF24 | #1 15/4、#2 47/48、#3 14/21 | 与 v3 相同 |
| IR | RX 21、TX 14、38 kHz | 与 v3 相同 |
| Buzzer | GPIO2，active buzzer，DC on/off | 与 v3 相同 |

### 20.8 后续 AI 修改守则

1. 修改板级行为前，先确认目标是 v1、v2、v3、v4 还是多板兼容；不要只按当前默认板型推断所有板。
2. 板型选择只允许一个 `BOARD_*` 宏生效。构建 v3 用 `BOARD_ESP32_DIV_V3`，构建 v4 用 `BOARD_ESP32_DIV_V4`。
3. 修改显示或触摸相关代码时，同时检查 `shared.h`、`BoardConfig.h`、`tft_setup.h` 和历史 `Libraries/User_Setup v*.h`。
4. `BoardConfig.h` 应负责板型和默认面板选择；`tft_setup.h` 应负责 TFT_eSPI 驱动、尺寸、SPI 引脚、颜色顺序和频率。
5. 触摸校准必须按 `TOUCH_PROFILE_ID` 独立保存。即使 v3/v4 默认 raw 范围一致，也不要共用同一个 board id。
6. 修改菜单时，同步检查主菜单、子菜单、返回路径、触摸导航提示、按键导航和资源释放。
7. 修改依赖 SD 的功能时，必须处理 SD 不可用、目录不存在、文件打开失败和跨板 SD 卡复用。
8. 修改 Wi-Fi/BLE/NRF24/CC1101 功能时，必须处理前后台 radio 资源冲突，退出时恢复或关闭相关状态。
9. 修改 BLE Ducky 时，必须保留 v1 不支持路径和 ESP32-S3 能力判断。
10. 修改设置项时，必须更新 `SettingsStore.*`、设置页 UI、默认值、JSON 兼容读取和本文档路径说明。
11. 修改主动测试能力时，不要移除授权、实验室、教学、合规测试等限制语义。
12. UI 适配 v4 时先消除固定 240x320 坐标，再增加更大屏幕的信息密度；不要改变功能行为。
13. v3/v4 原理图截图 hash 当前一致，后续若硬件文件更新，应重新核对 `hardware/schematic/v3` 与 `hardware/schematic/v4` 差异，再决定是否扩展本节。
14. Web flasher、预编译固件、manifest、文件名和 About 信息应使用板型宏区分 v3/v4，避免用户把 ILI9341 固件刷到 ST7796 板上。
15. 不要把 README 中偏 v2/ESP32-S3 的描述误当成 v1 的硬件事实；v1 仍由源码支持，但没有 ESP32-S3 特性。

### 20.9 常见误判

- 误判：v1/v2/v3/v4 功能完全相同。事实：菜单大体相同，但 v1 不支持 BLE Rubber Ducky，v4 还需要 ST7796 大屏适配。
- 误判：`BoardConfig.h` 未选板型时会报错。事实：历史 Arduino 源码默认选择 v2，当前 IDF 迁移代码默认选择 v3。
- 误判：TFT_eSPI 只看 `shared.h` 即可。事实：v1/v2 还必须匹配 `Libraries/User_Setup v1.h` 或 `Libraries/User_Setup v2.h`，v3/v4 必须看 `tft_setup.h`。
- 误判：触摸校准可以跨板直接复用。事实：校准按 `TOUCH_PROFILE_ID` 匹配。
- 误判：SD 总是可用。事实：许多功能必须降级或提示 SD 不可用。
- 误判：SubGHz 默认频率文案总是 433.92 MHz。事实：SubGHz Jammer 默认 target index 与全局默认频率常量不同。
- 误判：NeoPixel 已完整硬件化。事实：当前有设置项，但后续开发仍需确认硬件引脚和驱动路径。
- 误判：电池和蜂鸣器默认可用。事实：`BATTERY_ADC_PIN = -1`；蜂鸣器在 `shared.h` 默认 -1，当前 `BoardConfig.h` 为 v3/v4 覆写 GPIO2。

## 21. 给后续 AI 的最小上下文提示

如果后续让 AI 修改这个项目，建议至少提供以下上下文：

1. 当前仓库是 ESP-IDF + Arduino 兼容组件迁移工程，主要代码在 `components/esp32_div/`，板级入口在 `components/esp32_div_board/include/BoardConfig.h`。
2. 当前 IDF 默认板型是 `BOARD_ESP32_DIV_V3`；v4 必须显式启用 `BOARD_ESP32_DIV_V4`。
3. v3/v4 板子逻辑相同，唯一区别是 TFT：v3 为 3.2 寸 ILI9341 240x320 portrait，v4 为 4.0 寸 ST7796 320x480 portrait。
4. v1 不支持 BLE Rubber Ducky；v2/v3/v4 支持。
5. 所有板级引脚以 `components/esp32_div/include/shared.h` 为准；v3/v4 TFT_eSPI 配置以 `components/esp32_div_board/include/tft_setup.h` 为准。
6. 许多功能依赖 SD，运行时路径见本文第 16 节。
7. 设置统一保存在 `/config/settings.json`，触摸校准按 `TOUCH_PROFILE_ID` 匹配。
8. Wi-Fi/BLE 有后台扫描缓存，进入前台射频功能时要暂停或避免资源冲突。
9. 主动发射、干扰、Deauth、Portal、RFID 写入类功能必须保留授权使用语义。
