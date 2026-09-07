# 第三方组件

固件直接依赖由 ESP-IDF Component Manager 管理，声明在 `main/idf_component.yml`，解析版本和内容哈希记录在 `dependencies.lock`。不手工修改 `managed_components/`。

| 组件 | 本次解析版本 | 用途 | 许可证 / 来源 |
| --- | --- | --- | --- |
| ESP-IDF | 5.5.5 | FreeRTOS、驱动、Wi-Fi、NimBLE C API、lwIP、TLS、HTTP、FATFS、NVS、OTA | Apache-2.0 及其各子组件许可证；[Espressif](https://github.com/espressif/esp-idf/tree/v5.5.5) |
| espressif/esp_lcd_ili9341 | 2.0.2 | ILI9341 面板 | Apache-2.0；[esp-bsp](https://github.com/espressif/esp-bsp/tree/master/components/lcd/esp_lcd_ili9341) |
| atanisoft/esp_lcd_touch_xpt2046 | 1.0.6 | XPT2046 采样和坐标解码 | MIT；[atanisoft](https://github.com/atanisoft/esp_lcd_touch_xpt2046) |
| espressif/esp_lcd_touch | 1.2.1 | 触摸框架，传递依赖 | Apache-2.0；[esp-bsp](https://github.com/espressif/esp-bsp/tree/master/components/lcd_touch/esp_lcd_touch) |
| espressif/led_strip | 3.0.3 | WS2812 RMT 输出 | Apache-2.0；[idf-extra-components](https://github.com/espressif/idf-extra-components/tree/master/led_strip) |
| h2zero/esp-nimble-cpp | 2.5.0 | 保留已锁定的上游依赖；自有 BLE 服务直接使用 IDF NimBLE C API | Apache-2.0；[h2zero](https://github.com/h2zero/esp-nimble-cpp) |
| espressif/esp-dsp | 1.8.2 | FFT 和信号计算 | Apache-2.0；[esp-dsp](https://github.com/espressif/esp-dsp) |
| cJSON（IDF 内置 `json`） | 1.7.19 | JSON 解析和保存，由 IDF 5.5 提供 | MIT；[ESP-IDF json](https://github.com/espressif/esp-idf/tree/v5.5.5/components/json) |
| espressif/mdns | 1.11.2 | 本地域名发现 | Apache-2.0；[mDNS](https://github.com/espressif/esp-protocols/tree/master/components/mdns) |
| espressif/cmake_utilities | 0.5.3 | 面板组件构建依赖 | Apache-2.0；[idf-extra-components](https://github.com/espressif/idf-extra-components/tree/master/cmake_utilities) |

PCF8574、CC1101、NRF24、PN532 和红外驱动统一位于 `components/div_peripherals/`，通过 `div_board` 的总线与引脚配置连接官方硬件。`div_display` 使用上述面板和触摸组件；`div_signal` 接入 ESP-DSP；`div_gnss` 提供本地 NMEA 解析和 UART 接收。页面与交互逻辑留在 `main/`，归属见 [工程分层](docs/ARCHITECTURE.md)。

构建及烧录使用 [esptool](https://github.com/espressif/esptool)（GPL-2.0-or-later，独立 Python 工具，不链接进固件）。本地网页使用 [ESP Web Tools](https://github.com/esphome/esp-web-tools)（MIT，通过 CDN 加载）。保留的硬件模型查看器资源使用 [three.js](https://github.com/mrdoob/three.js)（MIT）和 [Draco](https://github.com/google/draco)（Apache-2.0）。

原有应用功能、图标及硬件资料保留来源和作者声明。工程许可证见 [LICENSE](LICENSE)，各第三方组件的完整许可证以下载组件中的 LICENSE / SPDX 标记为准。
