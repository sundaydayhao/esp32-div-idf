# 功能实现与运行条件

功能参考原工程的菜单和流程，应用及本地组件均使用 C17，通过 IDF 组件连接资源。进入功能时暂停普通后台扫描并重置共享文本缓存；退出时停止收发、HTTP 和抓包，再释放无线 SPI 设备。主动保留的 GPS 后台记录单独管理。

| 功能组 | 实现与运行条件 |
| --- | --- |
| Wi-Fi、隐藏 SSID、WPS | IDF 扫描和混杂接收；WPS 展示广播信息元素，隐藏 SSID 依赖捕获相关交换 |
| ARP 扫描 | lwIP 网络接口，需要连接局域网 |
| PCAP | SD 保存 Radiotap + 802.11；V2 每帧最多 2324 字节，V1/CYD 为 512 字节，分别保存截断长度与原长度；缓冲不足计入丢包 |
| Beacon、Probe、Deauth | 公开 `esp_wifi_80211_tx`，SDK 可拒绝部分管理帧；不修改 Wi-Fi 二进制库。SDK 接受发送不代表远端收到 |
| 门户 / Karma | IDF HTTP Server 和 UDP DNS；表单记录写入 NVS / SD |
| BLE、AirTag、Skimmer | NimBLE 广播扫描及解析；特征匹配不能证明设备身份；扫描回调通过队列交给界面线程 |
| BLE HID | IDF NimBLE C API 的 HID GATT 和 SD 脚本，需要目标设备配对连接；脚本按完整逻辑行读取，长 `STRING` 内容动态分配，其余命令超过 255 字节时报告错误 |
| NRF24 / ESB | 外接 NRF24L01+；覆盖实现的 ESB 参数，不是任意 2.4 GHz 协议解码器 |
| CC1101 / Sub-GHz | 原生 SPI 和 RMT/OOK，需要对应频段模块与天线；固定码识别受时序和信号质量影响 |
| IR | RMT 原始脉冲记录和重放，支持已实现的协议及 SD 格式 |
| RFID | 外接 PN532 SPI；写入需标签类型、认证密钥和权限允许，不能保证任意 UID 可写 |
| GPS / Wardriving | 9600 波特率 NMEA GPS，可记录 Wi-Fi/BLE 与位置；未定位时不伪造坐标 |
| WiGLE | IDF HTTPS Client、证书包和时间同步，需要网络及有效 API 配置 |
| 设置 / 升级 | cJSON、FATFS、NVS；SD / HTTP OTA 校验当前板型应用镜像 |

已完成软件接入及编译验证。无线时序、触摸校准、外设兼容和长时间稳定性仍需实板验收。连接要求见 [HARDWARE.md](HARDWARE.md)。
