# ESP32-C3 OLED 引脚调查

本工程把只读调查、受限开漏负载测试和一次性地址 ACK 测试分成三个独立的
PlatformIO 环境。默认环境始终是只读锁定版；任何主动测试都必须显式选择对应
环境。

> **当前安全状态（2026-09-24）**：以下针脚映射、P6/P13 电源假设、接线图和主动
> 测试环境均为早期实验记录，已经被最新断电照片/视频证据降级为未确认，暂不执行。
> 不要按下文给裸屏上电、运行 `open_drain_test`、`address_ack_test` 或 U8g2。
> 三片样品保持断电、P1-P16 全部悬空；最新判断和下一步以仓库根目录的
> [`分析.md`](../分析.md) 为准。固件文件保留，只用于历史复现。

## 历史候选映射（已暂停）

| OLED 脚 | ESP32-C3 GPIO | 开发板物理排针 | 候选功能 |
|---:|---:|---:|---|
| 8 | GPIO0 | 左侧 02 | D2 / SDAOUT |
| 9 | GPIO1 | 左侧 03 | D1 / SDAIN |
| 10 | GPIO3 | 右侧 20 | D0 / SCL |
| 11 | GPIO4 | 右侧 28 | D/C# / SA0 |
| 12 | GPIO5 | 右侧 27 | RES# |

另外：P13 是早期记录中的 VSS/VLL 参考地候选，P6 是外部 VDD 候选，P7 是内部 VLH 候选。
P1、P14、P15 仍是未解决的模拟节点。只有收到可信 ACK 后，P8-P12 的数字功能
才算获得功能层面的支持；ACK 也不能单独确定具体控制器型号。

## 历史接线（2.2k 电源串阻版本，已暂停）

所有改线必须在 OLED、实验电源和 ESP32-C3 都完全断电时完成：

```text
实验电源正极 3.01 V -- 2.2k -- OLED P6
实验电源负极 ------------------ OLED P13
ESP32-C3 GND ------------------ OLED P13

OLED P8  -- 51k -- OLED P6
OLED P8  -- 10k -- GPIO0

OLED P9  -- 51k -- OLED P6
OLED P9  -- 10k -- GPIO1

OLED P10 -- 51k -- OLED P6
OLED P10 -- 10k -- GPIO3

OLED P11 -- 51k -- OLED P13
OLED P11 -- 10k -- GPIO4

OLED P12 -- 51k -- OLED P6
OLED P12 -- 10k -- GPIO5

OLED P7、P1、P14、P15 悬空
```

每个 `51k` 和 `10k` 都是独立电阻。上拉只能接 OLED P6，不能接 ESP32-C3 的
`3V3`；P8 与 P9 暂时不要短接。

## 历史电源门槛（仅记录）

实验电源 3.01 V、P6 串联 2.2k，电源保持 CV、读数稳定且无异常：

```text
冷启动 P6                         2.99 V
P12 单线拉低期间 P6               2.83 V
P10 单线拉低期间 P6               2.83 V
P9 单线拉低期间 P6                2.83 V
P9+P10 同时拉低期间最低 P6         2.74 V
P9+P10 释放后立即/30秒/60秒 P6     2.93 V
```

原 4.7k 版本在 P9+P10 同时拉低时降至 2.48 V，并在释放后丢失活动状态，因此不
再用于 ACK。2.2k 版本已通过静态最坏组合，但这仍不是完整的器件额定值证明。

## 固件环境（保留用于历史复现）

### 1. 默认只读环境

```sh
pio run -e airm2m_core_esp32c3
```

只接受 `PINS`、`STATUS`、`HELP`，没有主动拉低、ACK 或扫描能力。

### 2. 受限开漏环境

```sh
pio run -e open_drain_test
```

保留已经使用过的单线 5 秒拉低和唯一的 P9+P10 双线测试。它没有 ACK、扫描、
高电平输出或自动重复功能。

### 3. 固定地址 ACK 环境

```sh
pio run -e address_ack_test
```

该环境的硬限制：

- 不使用 `Wire`，不扫描地址，不重试。
- P11/SA0 必须由外部 51k 保持 LOW；唯一地址是 7 位 `0x3C`，总线上只发送写
  地址字节 `0x78`。
- 唯一总线流量是 `START -> 0x78 -> 一个 ACK 时钟 -> STOP`；不发送控制字节、
  命令或显示数据。
- P8/SDAOUT 与 P11/SA0 永远只作输入。
- P9/SDAIN 与 P10/SCL 只能开漏拉低或恢复无内部上下拉的高阻输入。
- P12/RES# 只有在单独的双确认 RESET 阶段才允许拉低一次，其他时间为高阻输入。
- RESET 和 ACK 都先检查空闲电平必须稳定为 `P8..P12 = H,H,H,L,H`。
- 释放线路时会等待实际读到 HIGH，并设有硬超时；START 后即使中途失败也会尝试
  有界 STOP，随后无条件恢复全部输入。
- 活动状态保存在 RTC no-init 区。RESET 后 MCU 重启、ACK 已执行、API 失败或保护
  cookie 异常都会闭锁；没有串口解锁命令。

## 旧刷写与测试顺序（当前禁止执行）

Windows / VS Code PlatformIO 可在工程终端使用：

```sh
pio run -e address_ack_test -t upload --upload-port COM5
pio device monitor -p COM5 -b 115200
```

刷写完成后，必须让 OLED 实验电源和 ESP32-C3（包括 USB）同时完全断电，再共同
上电；否则持久保护可能保持 `DONE` 或进入 `FAULT`，这是预期的失败闭锁。

上电后逐条发送，不能一次粘贴多行；每次 `ARM` 后至少等 1 秒、且必须在 15 秒
内输入对应 `CONFIRM`：

```text
PINS

ARM RESET
CONFIRM RESET

ARM ACK 0X3C
CONFIRM ACK 0X3C
```

RESET 成功后不得断电、复位或改线，直接继续 ACK。ACK 完成后复制完整串口输出，
然后让 OLED 和 ESP32-C3 同时断电。任何阶段若出现 `FAULT`、`UNVERIFIED`、CC、
发热、异味、亮点或持续掉压，应立即断电。

可能的 ACK 结果：

- `ACK ON P8/SDAOUT`：当前固定分离 SDA 映射观察到 ACK；P9 只作为附带诊断。
- `NO ACK ON P8/SDAOUT`：当前固定映射没有观察到响应；即使 P9 为 LOW 也只记录
  为替代拓扑线索，不能当作本测试的 ACK。
- `INDETERMINATE` / `INVALID`：不作功能确认。

51k 上拉的上升时间很可能不满足控制器数据手册的高速指标；本固件通过极低速、
实际 HIGH 检测和超时降低测试不确定性，但 NACK 仍可能来自慢沿、接口模式、映射
或 SDAOUT 压降，而不一定是坏屏。
