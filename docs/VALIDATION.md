# 验证记录

日期：2026-09-07。环境：Windows PowerShell 5.1，ESP-IDF **v5.5.5**，IDF Python **3.11.15**，Espressif Xtensa GCC **14.2.0**，Ninja **1.12.1**，esptool **4.12.0**。应用版本：**2.0.0**。

## 三板型构建

执行 `python tools/firmware.py build --board all`，三个独立构建目录均完成编译、链接、镜像检查和打包。

另在 Windows PowerShell 5.1 中执行 `./tools/build.ps1 -Board cyd -IdfPath C:/esp/v5.5.5/esp-idf`，完成环境激活、增量构建和重新打包，验证 Windows 构建入口可用。

| 板型 | 芯片 | Flash | 应用字节数 | 每个 OTA 分区容量 | 每个 OTA 分区剩余 |
| --- | --- | --- | ---: | ---: | ---: |
| V2 | ESP32-S3 | 16 MB | 1,747,520 | 4,194,304 | 2,446,784 |
| V1 | ESP32 | 4 MB | 1,707,712 | 2,031,616 | 323,904 |
| CYD | ESP32 | 4 MB | 1,695,072 | 2,031,616 | 336,544 |

IDF size 报告中的主要内部内存区域：

| 板型 | 区域 | 已用字节 | 区域容量 | 剩余字节 |
| --- | --- | ---: | ---: | ---: |
| V2 | DIRAM | 246,503 | 341,760 | 95,257 |
| V1 | DRAM | 121,476 | 124,580 | 3,104 |
| V1 | IRAM | 127,175 | 131,072 | 3,897 |
| CYD | DRAM | 121,556 | 124,580 | 3,024 |
| CYD | IRAM | 127,051 | 131,072 | 4,021 |

这里是链接区域容量，**不是启动后的可用堆**。V1/CYD 的静态 DRAM 余量较小，后续添加全局数据应重新检查链接报告。前台功能共用 20,480 字节文本空间，相比各功能独立分配减少 106,240 字节静态占用；后台扫描和 GPS 使用独立存储。BLE 详细扫描列表、脉冲接收缓存按需申请。主任务栈默认 32 KiB，门户 HTTP 任务栈 12 KiB，构建生成 `.su` 栈报告。运行时最小堆、任务栈余量和连续切换功能仍需实板测量。

本次使用 `C:\esp\v5.5.5\esp-idf` 中的 SDK。原构建目录已归档，三个板型重新生成 SDK 配置。CMake 和组件清单限制为 IDF 5.5 系列；额外验证了 CMake 会拒绝 IDF 6.0.2。官方板型引脚、Flash 参数和双 OTA 分区布局保持原样。

V1 / CYD 启用 `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y`，使用 IDF 提供的非中断函数 Flash 放置规则，解决 IDF 5.5 下 ESP32 的 IRAM 链接超限。中断函数仍在 IRAM；没有关闭 Wi-Fi 或 RMT 的 IRAM 配置。相关规则见 [IDF 5.5.5 FreeRTOS Kconfig](https://github.com/espressif/esp-idf/blob/v5.5.5/components/freertos/Kconfig)。

JSON 改用 IDF 5.5 内置 `json` 组件；应用补充显式日志头文件，并为 `isdigit` 参数使用 `unsigned char` 转换以适配该工具链的 newlib。自有组件和应用继续使用 C17。

构建未关闭编译器警告；已有功能中仍有未使用变量/辅助函数警告。本次构建没有编译、链接或镜像容量错误。CI 的 V2 / V1 / CYD 矩阵已配置，本次没有运行远端 CI。

## 烧录布局

| 镜像 | V2 偏移 | V1 / CYD 偏移 |
| --- | --- | --- |
| Bootloader | 0x0 | 0x1000 |
| 分区表 | 0x8000 | 0x8000 |
| OTA 初始数据 | 0xE000 | 0xE000 |
| 应用 ota_0 | 0x10000 | 0x10000 |
| ota_1 | 0x410000 | 0x200000 |
| 完整合并镜像 | **0x0** | **0x0** |

Bootloader 镜像头已检查为 DIO / 40 MHz 和对应 16 MB / 4 MB 容量，因此网页烧录无需在运行时修补这些参数。应用描述符分别为 `esp32_div_v2`、`esp32_div_v1`、`esp32_div_cyd`。

## 自动验证

`python -m unittest discover -s tests -p "test_*.py" -v`：**18 项通过**（分别在 esptool 4.12.0 和 5.3.0 的 Python 环境执行），覆盖：

- 正常镜像，以及错误芯片、板型、Flash 容量、Bootloader Flash 参数。
- esptool 4 / 5 的烧录命令和参数拼写转换，以及不支持版本的拒绝。
- 镜像数据校验和、SHA-256 摘要损坏、描述符截断。
- 分区重叠、缺失 OTA 槽、槽容量不足、应用偏移错误。
- 路径越界、打包后篡改，以及网页 manifest 中错误的烧录偏移。

`tests/test_c_core.c` 使用 Zig 0.16 的 Clang 前端，以 `-std=gnu17 -Wall -Wextra -Werror` 编译并在 Windows 主机执行通过，覆盖：

- OTA 前缀长度、芯片和完整板型标识匹配。
- DNS A/AAAA、事务 ID、应答地址、压缩指针拒绝、截断数据和容量边界，以及 10,000 组畸形输入的越界写检查。
- 文本终止符、格式化和截断；动态数组增长、所有权移动、不同元素宽度拒绝及容量溢出。
- 4,096 字节完整逻辑行、空行和文件末尾，避免固定长度文本把长脚本行拆成多条命令。

`tests/test_gnss.c` 与组件中的 `gnss_parser.c` 使用相同的主机编译选项构建并执行通过，覆盖分段输入、GGA 定位、南/西半球 RMC 坐标与日期、GSV 卫星列表、GSA 解算标记、多解析器状态隔离、超长行丢弃、短句和状态重置。解析器不包含应用或 UART 头文件。

三个构建目录均通过 `python tools/check_c_project.py --build build-<board>`：`main/` 和 `components/` 共 **48 个 C 源文件**，全部由 C 编译器按 C17 编译，自有目录没有 C++ 源文件。每个构建仍包含 **33 个 managed_components C++ 编译条目**，属于保留的上游依赖。已有设置记录的 14 字节结构布局由 C 静态断言检查。

组件分层检查还验证：本地组件依赖无环、组件源码不包含应用头文件、组件编译命令不暴露 `main` 的头文件目录。应用的头文件和依赖均使用 CMake 的私有声明。红外、PCF8574 按键管理、触摸原始采样、GNSS 接收解析和 DSP 计算归入对应组件；终端、设置、文件管理、校准页面分别有独立应用源文件。目录归属见 [ARCHITECTURE.md](ARCHITECTURE.md)。

三套实际产物额外完成：

- 使用 esptool 检查 Bootloader / 应用镜像校验和与摘要；验证分区边界、双 OTA 容量和合并镜像内容，确认三个应用描述符的 IDF 版本均为 `v5.5.5`。
- 生成的烧录命令分别通过 esptool 4.12.0 和 5.3.0 的 CLI 帮助参数解析检查；该检查在打开串口之前退出。
- ZIP 解压至临时目录，执行包内独立 `flash_tool.py verify`，三个板型均通过。
- 三套产物也在 esptool 5.3.0 环境下通过验证；包内脚本与工程工具一致，依赖声明允许 esptool 4.12 / 5.x。
- 本地 HTTP 服务逐一读取三个板型 manifest 和合并镜像，检查下载字节与磁盘一致；工程源码及越界路径返回 404。
- `node --check docs/main.js` 通过；本地烧录页面 HTTP 请求返回成功。本次 SDK 版本调整没有重新进行浏览器截图验收。
- 项目自有源码、工具和文档的旧框架依赖扫描未发现残留；`git diff --check` 通过。

构建包内的 `build-info.json` 和 `checksums.json` 分别记录镜像信息与文件哈希；后续重新构建以新产物为准。SHA-256 用于完整性检查，不是发布者数字签名。

## 尚未完成的实板验收

本次串口枚举未发现连接设备，没有实际烧录，不能把构建成功等同于所有硬件功能通过。接入对应官方板后应完成：

1. 首次完整烧录、冷启动、重启和 115200 串口启动日志。
2. TFT、背光、触摸校准、五向按键；无 SD 时仍能进入菜单。
3. SD 读写及与 NRF24、CC1101、PN532 切换；按 [HARDWARE.md](HARDWARE.md) 避免物理复用冲突。
4. Wi-Fi / BLE 扫描及恢复、GPS 定位、实际 RF / IR 收发。
5. 同板 OTA 成功、错误板型 OTA 拒绝、升级中断及启动回滚。
6. V1/CYD 的长时间运行、最小可用堆、任务栈余量和反复进出功能。
