# STM32 Bootloader + BMS APP

基于 STM32F407 的汽车 BMS 诊断仪表项目，支持 UDS (ISO 14229) over CAN + DoIP (ISO 13400) over Ethernet 双通道诊断通信。

## 版本

| 版本 | 文件 | 说明 |
|------|------|------|
| **v2.0.5** | `BMS_version_2.0.5(完整版).bin` | 当前版本 — DoIP 完善 + NM 网络管理 + DTC 管理 |
| v2.0.1 | `BMS_version_2.0.1(小猫跳舞).bin` | LVGL GUI 仪表盘 |
| v2.0.0 | `BMS_version_2.0.0(小猫跳舞).bin` | LVGL GUI 初版 |
| v1.0.2 | `BMS_vesion_1.0.2(UDS+DOIP).bin` | UDS + DoIP 双通道 |
| v1.0.0 | `BMS_vesion_1.0.0(UDS).bin` | 仅 UDS over CAN |

| Bootloader | 文件 | 说明 |
|------|------|------|
| **V1.0.2** | `Bootloader_V1.0.2(完整版).hex` | 签名验证 + 安全阀 |
| V1.0.0 | `Bootloader_V1.0.0(无签名验证).hex` | 初版（有成砖BUG） |

## 功能

- **UDS 诊断**: 0x10/0x11/0x14/0x19/0x22/0x27/0x28/0x2E/0x31/0x3E/0x85
- **DoIP 诊断**: ISO 13400-2 TCP:13400 + UDP 车辆公告
- **固件刷写**: Bootloader (CAN only), 签名验证 + 安全阀
- **BMS 仪表盘**: LVGL v8.1.1, SOC 圆弧, 电压/电流/温度/单体数据
- **CAN NM**: 轻量级网络管理, 心跳 + 离线检测 + 休眠协商
- **SeedKey.dll**: 安全访问解锁工具

## 通信参数

| 参数 | CAN | DoIP |
|------|-----|------|
| 速率 | 500 kbps | 100 Mbps |
| 请求 ID | 0x7E0 | TCP 172.16.1.10:13400 |
| 响应 ID | 0x7E8 | TCP 13400 |
| 传输层 | ISO-TP | DoIP Header |

## 测试工具

```bash
# 命令行测试 (12 项)
node doip_tester.js [IP] [PORT]

# 图形化测试 (15 项, 自动打开浏览器)
node doip_gui.js
```

## 硬件

- MCU: STM32F407VET6
- LCD: 800x480 (FSMC)
- PHY: YT8512C (RMII)
- CAN: SN65HVD230
