# 硬件配置

引脚依据仓库内的官方原理图核对：

- [V2 主板](../hardware/schematic/v2/Main-Schematic.jpg)、[V2 扩展板](../hardware/schematic/v2/Shield-Schematic.jpg)
- [V1 主板](../hardware/schematic/v1/ESP32DIV.jpg)、[V1 扩展板](../hardware/schematic/v1/ESP32DIV-SHIELD.jpg)

定义位于 `components/div_board/include/board_pins.h`，板型选择位于 `main/Kconfig.projbuild`。

| 项目 | 官方 V2 | 官方 V1 |
| --- | --- | --- |
| 芯片 | ESP32-S3-WROOM-1U-N16 | ESP32-WROOM-32U |
| Flash | 16 MB，DIO，40 MHz | 4 MB，DIO，40 MHz |
| PSRAM | 不启用 | 不启用 |
| TFT SCLK / MOSI / MISO | 36 / 35 / 37 | 14 / 13 / 12 |
| TFT CS / DC / RESET | 17 / 16 / EN | 15 / 2 / EN |
| 背光 | GPIO7，LEDC | **PCF8574 P0**，开关 |
| 触摸 SCLK / MOSI / MISO | 36 / 35 / 37 | 25 / 32 / 35 |
| 触摸 CS / IRQ | 18 / 无 | 33 / 34 |
| SD SCLK / MOSI / MISO / CS | 12 / 11 / 13 / 10 | 18 / 23 / 19 / 5 |
| SD 检测 | 38 | 无专用检测 |
| PCF8574 SDA / SCL | 8 / 9 | 21 / 22 |
| 按键上 / 下 / 左 / 右 / 确认 | P7 / P5 / P3 / P4 / P6 | P6 / P3 / P4 / P5 / P7 |
| 电池 ADC | GPIO2，等值 100 kΩ 分压 | GPIO36，等值 100 kΩ 分压 |
| WS2812 | GPIO1，4 颗 | 无 |
| CC1101 CS / GDO2 RX / GDO0 TX | 5 / 3 / 6 | 27 / 16 / 26 |
| NRF24 #1 CE / CS | 15 / 4 | 4 / 5 |
| NRF24 #2 CE / CS | 47 / 48 | 26 / 27 |
| NRF24 #3 CE / CS | 14 / 21 | 16 / 17 |
| IR RX / TX | 21 / 14 | 4 / 5 |
| GPS RX / TX 接线 | 47 / 48 | 3 / 1 |

TFT RESET 接 EN，由硬件复位，驱动使用 `-1`。默认旋转 V2 为 2、V1 为 0。PCF8574 使用 100 kHz I2C，初始化扫描 0x20～0x27。V1 背光只有开关能力。

## 共享接口

V2 显示和触摸共用 SPI2；SD、CC1101、NRF24 和外接 PN532 共用 SPI3。设备在完整命令期间占有总线，不在功能切换时重配整个 SPI 主机。

V1/CYD 有三组独立 SPI 接线、两个可用主机，因此触摸使用专用 GPIO 传输，复用第三方 XPT2046 解码组件；显示和 SD 使用硬件 SPI。

扩展板还存在物理引脚复用，总线锁不能消除同一 GPIO 的接线冲突：

- V2：GPS 与 NRF24 #2 共用 47/48；IR 与 NRF24 #3 共用 14/21。使用 GPS / IR 时拔下对应 NRF24。
- V1：NRF24 #1 的 CS 与 SD 的 CS 同为 GPIO5。固件禁用这个 NRF24 槽位，使用 SD 时应拔下该模块，避免两个器件同时驱动 MISO。
- V1：IR TX 也使用 GPIO5。发送期间固件占用 SD 总线，阻止 SD 时钟；拔下共用 GPIO5 的 NRF24 #1。
- V1：CC1101 与 NRF24 #2/#3 共用部分 CS / GDO / CE，使用 CC1101 时拔下冲突模块。
- V1：GPS RX 接 GPIO3，与 USB 串口发送端相连。二者不能同时向该线输出数据，需要隔离接线。GPS 只接收 NMEA，不重新分配 GPIO1 的日志输出。
- PN532 为外接 SPI 模块。默认 CS 为 V2 GPIO4、V1 GPIO17，使用前拔下共用 CS 的 NRF24；可在 menuconfig 修改 CS。
- V2 GPIO2 也关联可选蜂鸣器，默认禁用蜂鸣器，以保留电池测量。

## CYD

适配 **ESP32-2432S028R、ILI9341、电阻触摸、4 MB Flash**。背光 GPIO21，触摸 MISO39 / MOSI32 / SCLK25 / CS33 / IRQ36，SD 使用 18/23/19/5。无 PCF8574 和电池 ADC，电压显示不可用。

第三 NRF24 CS25 与触摸时钟冲突，固件禁用该槽位，默认扫描使用 CE16 / CS17 的第一槽位。其他屏幕控制器、电容触摸和不同 Flash 容量的板不属于此配置。

## 首次烧录

1. 依据 PCB 丝印和模组选择 V1 / V2 / CYD，使用对应完整包。
2. 连接 USB 数据线，运行烧录脚本并指定实际串口。
3. 自动复位不能进入下载模式时，按住 BOOT、点按 RESET，再释放 BOOT 后重试。
4. 115200 波特率查看日志。屏幕、SD、BLE、菜单和触摸初始化后应输出 `ready`。
5. 先检查屏幕及按键，再按复用关系接入外设，验证 SD 与功能切换。

本次未连接硬件，验证状态见 [VALIDATION.md](VALIDATION.md)。
