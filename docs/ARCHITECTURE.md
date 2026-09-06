# 工程分层与文件归属

应用和本地组件全部使用 C17。目录按职责划分：设备访问与协议实现放在 `components/`，功能页面和交互流程放在 `main/`。第三方依赖由 Component Manager 管理，保留在 `managed_components/`。

## 应用层

| 目录 | 职责 | 代表文件 |
| --- | --- | --- |
| `main/main.c` | 初始化 IDF、启动应用、运行主循环 | `app_main()` |
| `main/app/` | 菜单与功能生命周期、应用状态、配置持久化、前台缓存所有权 | `app.c`、`settings_store.c`、`feature_text.c` |
| `main/features/` | 各功能的页面和操作流程 | `wifi.c`、`bluetooth.c`、`gps.c`、`ir.c`、`rfid.c`、`subghz.c`、`ducky.c` |
| `main/features/` | 系统功能页面 | `terminal.c`、`settings_page.c`、`file_manager.c`、`touch_calibration.c` |
| `main/ui/` | 通知、状态栏、主题、屏幕键盘、触摸坐标映射及 UI 自检 | `ui_utils.c`、`theme.c`、`keyboard_ui.c`、`touch_ui.c` |
| `main/ui/assets/` | 应用图标资源 | `icon.h` |

应用的头文件与实现按相同职责放置。`main/CMakeLists.txt` 使用 `PRIV_INCLUDE_DIRS` 和 `PRIV_REQUIRES`；应用头文件不作为组件公共接口导出。

`settings_store.c` 留在应用层，因为亮度、主题、菜单行为和校准参数属于本应用的设置。实际 NVS、文件系统及设备访问由组件提供。`touch_ui.c` 使用这些设置把原始触摸坐标映射到屏幕，并支持 UI 测试注入；原始采样和过滤由显示组件负责。

## 组件层

各编译组件统一使用 `src/` 放实现、`include/` 放公共 C 接口；显示内部资源放在 `private_include/`。`div_common` 为纯头文件组件。

| 组件 | 内容 | 本地依赖 |
| --- | --- | --- |
| `div_common` | C 文本、动态数组、边界复制和完整行读取；无 IDF/硬件依赖 | 无 |
| `div_board` | 官方板型及引脚、SPI 仲裁与设备占用、UART、RMT 脉冲 IO、电池 ADC、状态灯及系统时间工具 | `div_common` |
| `div_peripherals` | PCF8574 与按键管理、CC1101、NRF24、PN532、OOK 编解码、红外收发与协议 | `div_board`、`div_common` |
| `div_display` | ILI9341 显示和画布、XPT2046 通信、原始采样与压力/稳定性过滤 | `div_board`、`div_common` |
| `div_gnss` | UART 接收适配、分段字节流组帧、GSV/GSA/GGA/RMC 解码、卫星和定位状态 | `div_board`、`div_common` |
| `div_signal` | ESP-DSP 接入、FFT 计算及固定工作区 | `div_common` |
| `div_services` | Wi-Fi、BLE/HID、控制台、SD/FATFS、NVS、OTA 和 DNS | `div_board`、`div_common` |

底层设备实例由对应组件管理，例如 PCF8574 由 `button_input.c` 持有，V1 背光和按键共用该实例。通用 UART 生命周期位于 `div_board`，控制台和 GNSS 不互相依赖。

NMEA 解析器接收调用方提供的字节和时间戳，使用显式 `gnss_parser_t` 保存状态，可独立进行主机测试。GNSS 接收适配负责调用 UART，不包含星图、SD 日志或 WiGLE 逻辑。FFT 的容量和工作区由 `div_signal` 定义，不引用应用的 `shared.h`。

## 同一设备在两层中的职责

| 设备/功能 | 组件实现 | 应用实现 |
| --- | --- | --- |
| 红外 | `div_peripherals/src/ir_driver.c`、`include/ir_*.h`；RMT IO 由 `div_board` 提供 | `main/features/ir.c`：学习、遥控、配置浏览页面 |
| 触摸 | `div_display/src/touch_driver.c`、`touch_input.c`：通信、压力与稳定性过滤 | `main/ui/touch_ui.c`：校准映射；`main/features/touch_calibration.c`：校准交互 |
| GPS | `div_gnss/src/gnss_parser.c`、`gnss_receiver.c`：NMEA 与串口接收 | `main/features/gps.c`：星图、轨迹、记录和上传流程 |
| PCF8574 | `div_peripherals/src/pcf8574.c`、`button_input.c`：I2C、探测、物理按键和诊断 | `main/app/app.c`：合并物理按键与屏幕按钮的导航事件 |
| CC1101 / NRF24 / PN532 | `div_peripherals/src/` 中对应芯片驱动 | `main/features/subghz.c`、`bluetooth.c`、`rfid.c` 中的操作流程 |
| SD | `div_services/src/storage.c`、`storage_file.c`：挂载和文件 IO | `main/features/file_manager.c`：文件浏览与确认页面；UI 状态栏显示挂载状态 |
| FFT | `div_signal/src/signal_dsp.c`：信号计算 | Wi-Fi 和 Sub-GHz 功能负责采集、调用计算与绘图 |

## 新增文件与依赖规则

1. 芯片寄存器、总线通信、协议解析放入对应组件；增加一种设备时，先判断它属于已有外设、显示或 GNSS 组件，职责独立时再新增组件。
2. 界面、菜单、用户确认、应用设置和会话调度放入 `main` 的对应子目录。一个完整页面应有自己的源文件，通用 UI 工具不再承载完整功能页面。
3. 组件通过参数、状态结构和回调与上层交换数据，不包含 `main` 头文件，不访问菜单状态或应用设置全局变量。
4. 公共头文件所需的依赖使用 `REQUIRES`；实现内部依赖使用 `PRIV_REQUIRES`。本地组件依赖必须无环，不向下层加入应用的 include 路径。
5. `managed_components/` 保留上游实现，不手工移动或修改其中的 C/C++ 文件。

`python tools/check_c_project.py` 检查组件引用应用头文件及依赖环。加上 `--build build-v2`（或 `build-v1`、`build-cyd`）后，还检查每个自有源文件按 C17 编译，以及组件编译命令没有暴露 `main` 的头文件目录。CI 对三个板型执行这些检查。
