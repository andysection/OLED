# ESP32 经典蓝牙 PANU 实现指南

本文说明如何让 ESP32-WROOM-32 作为经典蓝牙 PAN User（PANU），连接
iPhone 的 Network Access Point（NAP），通过个人热点获得互联网访问能力。
内容以本项目已经实机验证通过的实现为准。

## 1. 最终效果

```text
ESP32 应用
   | HTTP / SNTP / Ping
   v
lwIP + esp-netif
   | Ethernet frame
   v
Bluetooth BNEP / PANU
   | Classic Bluetooth
   v
iPhone NAP / Personal Hotspot
   | Cellular network
   v
Internet
```

联网后，本项目会：

- 通过 DHCP 获取 iPhone 分配的 IPv4 地址，同时支持 IPv6 SLAAC。
- 每 30 秒 Ping 一次公网地址，输出网络状态。
- 向 `httpbin.org` 分别发送 HTTP GET 和 JSON POST 请求。
- 通过 SNTP 同步时间，并每 30 秒打印一次中国标准时间。
- 连接失败或断开后等待 15 秒，再自动连接。

## 2. 硬件与软件要求

| 项目 | 要求 |
| --- | --- |
| 芯片 | 原版 ESP32，例如 ESP32-WROOM-32 |
| 不适用芯片 | ESP32-S2、S3、C3、C6 等没有经典蓝牙的型号 |
| PlatformIO board | `esp32dev` |
| Framework | ESP-IDF |
| 蓝牙角色 | ESP32 为 PANU，iPhone 为 NAP |
| 串口 | 115200 baud |

公开的 `esp_pan_*` API 晚于 ESP-IDF 5.5。本项目在
[`platformio.ini`](platformio.ini) 中固定了 PlatformIO 平台版本，并指向本机缓存的
ESP-IDF PAN 版本。复制项目到另一台电脑时，需要安装同一 ESP-IDF 提交，或者修改
`platform_packages` 的路径。

## 3. iPhone 设置

1. 打开 iPhone 的蜂窝数据，并先确认 iPhone 自己能够访问互联网。
2. 打开“设置 > 个人热点 > 允许其他人加入”。
3. 打开蓝牙。
4. 初次配对时停留在“个人热点”或“蓝牙”页面。
5. 核对串口与 iPhone 显示的配对数字，然后在 iPhone 上接受配对。

当前演示代码会在 ESP32 端自动确认 SSP 数字，适合开发测试。产品代码应改为由按键或
其他可信用户交互确认，避免自动接受未知设备的配对请求。

程序使用 iPhone 的设备名称进行首次搜索。名称必须与“设置 > 通用 > 关于本机 >
名称”完全一致，包括大小写。

```ini
CONFIG_PAN_PEER_DEVICE_NAME="section"
```

首次配对完成后，配对密钥保存在 NVS。若只有一个已配对的经典蓝牙设备，程序会直接
使用其地址重连，不再依赖 iPhone 处于可发现状态。

## 4. 模块划分

| 文件 | 职责 |
| --- | --- |
| `src/main.c` | 初始化经典蓝牙、扫描、配对、连接 iPhone NAP、断线重试 |
| `src/pan_netif.c` | BNEP 与 `esp-netif` 之间的以太网帧适配、DHCP、IPv6、Ping |
| `src/internet_http.c` | HTTP GET/POST 外网验证 |
| `src/internet_time.c` | SNTP 校时和 30 秒时间输出 |
| `src/Kconfig.projbuild` | PANU 参数的 menuconfig 定义 |
| `sdkconfig.defaults` | 本项目的默认配置 |

## 5. 初始化顺序

初始化顺序必须保持如下关系：

```text
NVS
 -> esp-netif
 -> default event loop
 -> release BLE memory
 -> Classic BT controller
 -> Bluedroid
 -> GAP/PAN callbacks
 -> PAN esp-netif
 -> esp_pan_init(PANU)
 -> scan or reconnect
```

核心初始化逻辑位于 `app_main()`：

```c
ESP_ERROR_CHECK(esp_netif_init());
ESP_ERROR_CHECK(esp_event_loop_create_default());
ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

ESP_ERROR_CHECK(esp_bt_controller_init(&controller_config));
ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_config));
ESP_ERROR_CHECK(esp_bluedroid_enable());

ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_callback));
ESP_ERROR_CHECK(esp_pan_register_callback(pan_callback));

ESP_ERROR_CHECK(pan_netif_init(s_local_mac));

esp_pan_cfg_t pan_config = ESP_PAN_DEFAULT_CONFIG();
pan_config.role = ESP_PAN_ROLE_PANU;
ESP_ERROR_CHECK(esp_pan_init(&pan_config));
```

## 6. iPhone 兼容性的关键点：MAC 必须等于 BD_ADDR

这是本项目最关键的实现细节，也是此前“PAN 已连接，但没有 DHCP Offer”的根因。

BNEP 将 Bluetooth Device Address（BD_ADDR）作为 PAN 端点的以太网 MAC。ESP32 的
PAN 网卡必须直接使用原始蓝牙地址：

```c
static void bdaddr_to_eth_mac(const uint8_t *bda, uint8_t *mac)
{
    memcpy(mac, bda, 6);
}
```

不要按普通本地管理以太网地址的做法修改 U/L 位：

```c
// 错误：08:b6:... 会变成 0a:b6:...
mac[0] = (mac[0] | 0x02) & 0xfe;
```

使用 `0a:b6:...` 时，iPhone 虽然允许 PAN 建链并发送 IPv6 RA，但只会反射 ESP32 的
广播帧，不会为该客户端建立正常的 DHCP 和外网转发状态。改回原始
`08:b6:...` 后，iPhone 立即返回 DHCP Offer，随后 IPv4、IPv6、Ping、HTTP 和 SNTP
全部恢复。

当前 Espressif 新增的 PANU 示例中也存在翻转 U/L 位的写法，直接复制示例时必须特别
检查这一点。

## 7. 建立 PANU 到 NAP 的连接

首次连接时，程序通过 GAP inquiry 搜索名称匹配的 iPhone，然后调用：

```c
esp_pan_connect(iphone_address, ESP_PAN_ROLE_PANU, ESP_PAN_ROLE_NAP);
```

主要事件如下：

| 事件 | 处理 |
| --- | --- |
| `ESP_PAN_INIT_EVT` | 设置本机名称，开始扫描或连接已配对设备 |
| `ESP_PAN_OPEN_EVT` | 保存 PAN handle，启动 `esp-netif`、DHCP 和 SLAAC |
| `ESP_PAN_DATA_IND_EVT` | 将 BNEP 数据重建为以太网帧并送入 lwIP |
| `ESP_PAN_CONG_EVT` | 暂停或恢复发送 |
| `ESP_PAN_CLOSE_EVT` | 关闭网络接口，15 秒后重连 |

## 8. BNEP 与 esp-netif 适配

### 发送方向

lwIP 交给驱动的是完整以太网帧。驱动取出目的 MAC、源 MAC、EtherType 和 L3 payload：

```c
esp_pan_write(handle,
              frame,       // destination MAC
              frame + 6,   // source MAC
              protocol,
              payload_len,
              frame + 14,
              false);
```

目的地址与源地址的顺序不能颠倒。

### 接收方向

`ESP_PAN_DATA_IND_EVT` 提供分离的目的 MAC、源 MAC、协议和 payload。程序重新构造
14 字节以太网头，再调用：

```c
esp_netif_receive(pan_netif, frame, frame_len, NULL);
```

调用后 frame 的所有权交给 `esp-netif`，不能由调用方再次释放。

iPhone NAP 可能把 ESP32 发出的广播或组播帧再转发回来。接收路径必须丢弃
`source MAC == local PAN MAC` 的反射帧，否则 lwIP 可能把自己的 IPv6 DAD 探测当作
地址冲突：

```c
if (memcmp(src, local_mac, 6) == 0) {
    return ESP_OK;
}
```

## 9. IP 地址和网络状态

PAN 连接成功后调用：

```c
esp_netif_set_default_netif(pan_netif);
esp_netif_action_start(pan_netif, 0, 0, 0);
esp_netif_action_connected(pan_netif, 0, 0, 0);
esp_netif_create_ip6_linklocal(pan_netif);
```

`ESP_NETIF_INHERENT_DEFAULT_ETH()` 会启动 DHCP client。iPhone 实测分配：

```text
IP      172.20.10.13
Mask    255.255.255.240
Gateway 172.20.10.1
```

具体客户端地址可能变化，不能在代码中假定固定地址。静态 IPv4 回退默认关闭，因为
手动设置 `172.20.10.2` 不能代替 iPhone 的 DHCP 客户端授权，还可能造成地址冲突。

如果 30 秒内既没有 IPv4，也没有可路由 IPv6，程序断开 PAN，输出个人热点提示，等待
15 秒后重试。

## 10. 外网、HTTP 和时间验证

取得可用地址后，程序执行三类测试。HTTP GET/POST 在网络首次可用时执行；如果先取得
IPv6、随后又取得 IPv4，则再排队执行一次。Ping 和时间打印才是每 30 秒运行：

| 测试 | 当前配置 | 成功标志 |
| --- | --- | --- |
| IPv4 Ping | `8.8.8.8` | `Internet test PASSED` |
| HTTP GET | `http://httpbin.org/get` | `HTTP 200` |
| HTTP POST | `http://httpbin.org/post` | `HTTP 200` |
| SNTP | `2606:4700:f1::123` | `time synchronized` |

POST body 为：

```json
{"source":"esp32-bluetooth-pan"}
```

时区使用 POSIX 格式 `CST-8`，表示 UTC+8。同步成功后立即打印一次时间，随后周期定时器
每 30 秒打印：

```text
I (...) INTERNET_TIME: current time: 2026-09-24 10:58:08 CST
```

默认 SNTP server 是数字 IPv6 地址，用于避免依赖 DNS64。若部署环境只有 IPv4，需要把
`PAN_SNTP_SERVER` 改为可达的 IPv4 地址或域名。

## 11. 关键配置

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| `PAN_PEER_DEVICE_NAME` | `section` | iPhone 名称 |
| `PAN_LOCAL_DEVICE_NAME` | `ESP32_PANU` | ESP32 蓝牙名称 |
| `PAN_RETRY_DELAY_SECONDS` | `15` | 连接失败后的重试间隔 |
| `PAN_DHCP_TIMEOUT_SECONDS` | `30` | 等待可用地址的时间 |
| `PAN_NETWORK_TEST_INTERVAL_SECONDS` | `30` | Ping 测试间隔 |
| `PAN_HTTP_GET_URL` | `http://httpbin.org/get` | GET 测试地址 |
| `PAN_HTTP_POST_URL` | `http://httpbin.org/post` | POST 测试地址 |
| `PAN_SNTP_SERVER` | `2606:4700:f1::123` | 数字 IPv6 NTP 地址 |
| `PAN_TIME_ZONE` | `CST-8` | 中国标准时间 |
| `PAN_TIME_PRINT_INTERVAL_SECONDS` | `30` | 时间打印间隔 |
| `PAN_STATIC_IPV4_FALLBACK` | `n` | 静态 IPv4 诊断回退，正常使用不要开启 |

可通过以下命令修改配置：

```bash
pio run -t menuconfig
```

IPv6 SLAAC 还依赖 `CONFIG_LWIP_IPV6_AUTOCONFIG=y`。本项目已经在
`sdkconfig.defaults` 中启用，并保留两个 RDNSS DNS server slot。

## 12. 编译、烧录和监视

ESP32-WROOM-32 使用 `esp32dev` board。建议明确指定串口，避免 PlatformIO 误选系统中的
其他串口：

```bash
cd /Users/liyao/Documents/GitHub/OLED/headphone/ASHOLE
pio run
pio run -t upload --upload-port /dev/cu.usbserial-0001
pio device monitor --port /dev/cu.usbserial-0001 --baud 115200
```

正常启动时的关键日志为：

```text
PAN interface MAC 08:b6:1f:3b:30:ee
connected to iPhone PAN
PAN RX ... DHCP UDP 67 -> 68
iPhone assigned IP 172.20.10.13, mask 255.255.255.240, gateway 172.20.10.1
GET API succeeded: HTTP 200
POST API succeeded: HTTP 200
time synchronized from 2606:4700:f1::123
Internet test PASSED
```

## 13. 常见问题

| 现象 | 检查重点 |
| --- | --- |
| 找不到 iPhone | 名称必须完全一致；首次连接保持热点或蓝牙设置页面打开 |
| `PAN connection failed` | 打开“允许其他人加入”；删除旧配对后重新配对 |
| PAN 已连接但只有 DHCP Discover | 首先确认 PAN MAC 与原始 BD_ADDR 完全相同，不能翻转 U/L 位 |
| 有 DHCP 但不能上网 | 确认 iPhone 蜂窝数据可用、套餐允许个人热点、VPN 未阻断共享 |
| GET/POST DNS 失败 | 先看是否取得 DHCP 地址和网关，再检查 DNS，不要直接设置静态地址掩盖问题 |
| 多个已配对设备 | 程序会改为按名称扫描；可清除无关配对记录 |
| `BT_HCI mode change/sniff` | 蓝牙省电模式切换，不表示 PAN 失败 |

测试 API 使用明文 HTTP，只适合连通性验证，不能发送密码、Token 或其他敏感数据。
产品环境应改用 HTTPS，并配置服务端证书校验。

需要完全清除 ESP32 的配对记录时，可以擦除整片 Flash 后重新烧录。该操作也会删除 NVS
中的其他数据：

```bash
pio run -t erase --upload-port /dev/cu.usbserial-0001
pio run -t upload --upload-port /dev/cu.usbserial-0001
```

## 14. 实现检查清单

- 使用支持经典蓝牙的原版 ESP32。
- ESP32 注册 PANU，远端角色指定为 NAP。
- PAN netif MAC 必须原样复制 Bluetooth BD_ADDR。
- TX 使用 `dst, src, protocol, payload` 的正确参数顺序。
- RX 补回标准 14 字节以太网头后再交给 `esp-netif`。
- 丢弃源 MAC 等于本机 PAN MAC 的回送帧，避免破坏 IPv6 DAD。
- PAN OPEN 后启动 netif、DHCP 和 IPv6 link-local。
- PAN CLOSE 后停止网络任务并定时重连。
- 以 DHCP 结果为准，不把静态 `172.20.10.x` 当作正式方案。
- 分别验证 Ping、HTTP GET、HTTP POST 和 SNTP。
- 时间同步成功后用周期定时器每 30 秒打印一次。
