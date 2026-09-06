# ESP32-DIV

基于 ESP-IDF 的 C 语言手持无线工具工程。支持官方 ESP32-DIV V2、V1，以及 CYD ESP32-2432S028R。应用版本 **2.0.0**，构建基线 **ESP-IDF 6.0.2**。`main/` 与 `components/` 的全部自有源码使用 **C17**；`managed_components/` 中的第三方库保留上游实现，可以包含 C++。

## 构建与烧录

Windows 已安装 ESP-IDF 时，在工程根目录执行：

```powershell
.\tools\build.ps1 -Board all -IdfPath C:\esp\v6.0.2\esp-idf
```

也可以在已激活的 ESP-IDF 终端中执行，支持 Windows / Linux / macOS：

```sh
python tools/firmware.py build --board v2
python tools/firmware.py verify --board v2
python tools/firmware.py flash --board v2 --port COM7
```

将 `v2` 换成 `v1` 或 `cyd` 选择硬件。Linux 串口例如 `/dev/ttyUSB0`。烧录前自动校验固件包，必须显式指定串口；串口日志波特率为 115200。

| 板型 | 芯片 / Flash | 构建目录 | 完整烧录包 |
| --- | --- | --- | --- |
| 官方 V2 | ESP32-S3-WROOM-1U-N16 / 16 MB | `build-v2` | `dist/esp32-div-v2-2.0.0.zip` |
| 官方 V1 | ESP32-WROOM-32U / 4 MB | `build-v1` | `dist/esp32-div-v1-2.0.0.zip` |
| CYD 2432S028R | ESP32 / 4 MB，电阻触摸 | `build-cyd` | `dist/esp32-div-cyd-2.0.0.zip` |

V2 默认不依赖 PSRAM。每个板型分别保存 SDK 配置、分区表、应用标识及产物。V1 和 CYD 虽使用同一芯片，也不能互刷。

使用 IDF 原生工具调试或修改配置：

```sh
idf.py -B build-v2 -D DIV_BOARD=v2 menuconfig
idf.py -B build-v2 -D DIV_BOARD=v2 build
idf.py -B build-v2 -D DIV_BOARD=v2 -p COM7 flash monitor
python tools/firmware.py package --board v2
```

从空目录构建其他板型时，指定对应 `DIV_BOARD`，CMake 会选择正确芯片。更换板型请使用另一构建目录；CMake 会拒绝板型、芯片和已有 SDK 配置不一致的组合。

## 工程结构

```text
main/main.c               app_main、IDF 启动与主循环
main/app/                 菜单调度、生命周期、应用配置和设置
main/features/            无线、GPS、红外、RFID及系统功能页面
main/ui/                  通用界面、主题、键盘、触摸坐标映射和图标
components/div_common/    无硬件依赖的 C 文本、数组和内存工具
components/div_board/     板型引脚、SPI/UART/RMT、总线仲裁、电池和灯带
components/div_peripherals/ PCF8574/按键、NRF24、CC1101、PN532、OOK、红外驱动
components/div_display/   esp_lcd 显示、画布、XPT2046 采样和压力过滤
components/div_gnss/      GNSS UART 接收、NMEA 解析和定位状态
components/div_signal/    DSP 计算与 FFT 工作区
components/div_services/  Wi-Fi、BLE/GATT/HID、控制台、FATFS、NVS、OTA、DNS
managed_components/       Component Manager 管理的第三方依赖
hardware/                 官方原理图、PCB 和物料资料
tests/                    镜像/分区/打包测试、C 协议和内存边界测试
tools/                    构建、验证、打包、烧录、本地烧录页面
sdkconfig.defaults*       通用及板型默认配置
partitions*.csv           16 MB / 4 MB 双 OTA 分区布局
```

入口是 `main/main.c` 的 `app_main()`。组件通过 `idf_component_register()` 声明依赖；Component Manager 从 `main/idf_component.yml` 和 `dependencies.lock` 获取第三方库。`managed_components/`、生成配置、构建目录和固件产物不提交。

外设通信、协议解析和通用服务放在 `components/`；菜单、绘制、用户交互和功能调度放在 `main/`。例如红外驱动位于 `div_peripherals`，遥控页面位于 `main/features/ir.c`。应用头文件仅通过 `PRIV_INCLUDE_DIRS` 提供给 `main`，组件不能引用应用头文件。完整归属和依赖规则见 [工程分层](docs/ARCHITECTURE.md)。

驱动和服务使用 C 结构体及函数接口，动态缓冲区显式管理所有权。前台功能共用 20 KiB 文本缓存，切换功能时重置；后台任务使用独立存储。主任务栈默认 32 KiB，门户 HTTP 任务栈 12 KiB。设置结构保留 14 字节的持久化布局。构建与 CI 检查自有代码的 C17 编译边界，允许第三方依赖使用 C++。

## 功能

| 分类 | 已接入的功能 |
| --- | --- |
| Wi-Fi | 扫描、频谱、PCAP/Radiotap、Deauth 检测、Beacon/Probe 测试、隐藏 SSID、WPS 信息、ARP 扫描、门户及 Karma |
| BLE | 扫描、广播监测、设备分析、广播测试、AirTag 广播/发现、Skimmer 特征检查、HID 脚本 |
| 2.4 GHz | NRF24 频谱、通道测试、ESB 接收与重放、MouseJack 实验功能 |
| Sub-GHz | CC1101 收发、OOK 编解码、记录/重放、配置文件、SubBrute、信号检测 |
| 红外 | RMT 收发、学习、重放、SD 配置及收藏 |
| RFID / NFC | PN532 标签识别、支持标签的读写、数据导入导出及卡片操作 |
| GPS | NMEA 卫星/定位、轨迹、Wi-Fi/BLE Wardriving、WiGLE HTTPS 上传 |
| 系统 | SD 文件浏览、触摸校准、主题/亮度、NVS 记录、SD/HTTP OTA、串口控制、可选 UI 自检 |

[功能及约束](docs/FEATURES.md) 说明各项运行条件。无线发送遵循 IDF 驱动支持范围，统计以驱动返回结果为准。

显示、触摸、灯带、DSP、JSON、mDNS 使用第三方组件；BLE 使用 IDF NimBLE 的 C API，网络、TLS、FATFS、NVS 和 OTA 使用 IDF 自带组件。PCF8574、NRF24、CC1101、PN532 通过原生 I2C / SPI 与板级总线仲裁接入。依赖来源见 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 固件与升级

`dist/<board>/` 包含 Bootloader、分区表、OTA 初始数据、应用镜像、合并镜像、ESP Web Tools manifest、SHA-256 校验表、构建信息及独立烧录脚本。构建工具同时生成 ZIP 包。

- 首次烧录：使用完整包，或从 **0x0** 烧录 `esp32-div-<board>-merged.bin`。
- SD / HTTP OTA：只使用 **`esp32_div_<board>.bin`** 应用镜像，不上传合并镜像。
- OTA 写入前校验芯片和板型，完成校验后切换启动分区。启用 IDF 启动回滚，新固件完成初始化后标记为有效。
- HTTP OTA 的校验码显示在屏幕上，网页通过 `X-Update-Token` 请求头提交。
- 完整包会写入分区表和 OTA 初始数据，适合首次安装或恢复。保留当前分区布局的日常升级请使用 OTA。

本地网页烧录：

```sh
python tools/serve_flasher.py
```

打开打印的本地网址。网页只提供已构建且验证通过的板型，需要支持 Web Serial 的浏览器；命令行不依赖网页或 CDN。

## 验证

```sh
python tools/check_c_project.py --build build-v2
python -m unittest discover -s tests -p "test_*.py" -v
python tools/firmware.py verify --board all
```

主机端 C 测试可使用 GCC / Clang 执行（Windows 也可使用 `zig cc`）：

```sh
cc -std=gnu17 -Wall -Wextra -Werror -Icomponents/div_common/include -Icomponents/div_services/include tests/test_c_core.c -o test_c_core
./test_c_core
cc -std=gnu17 -Wall -Wextra -Werror -Icomponents/div_common/include -Icomponents/div_gnss/include tests/test_gnss.c components/div_gnss/src/gnss_parser.c -lm -o test_gnss
./test_gnss
```

[验证记录](docs/VALIDATION.md) 记录真实构建结果及镜像大小。[硬件接线](docs/HARDWARE.md) 说明背光、触摸、Flash 和扩展板复用引脚。

本次工作环境没有连接开发板，已完成的是构建、镜像与配置验证，尚未完成实际烧录、上电和所有外设验收。

项目采用 MIT 许可证并保留原作者信息，见 [LICENSE](LICENSE)。
