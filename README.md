# WiFi-Deck

![Android CI](https://github.com/xiangwen-ctrl/WiFi-Deck/actions/workflows/android.yml/badge.svg)
![Platform](https://img.shields.io/badge/Platform-RTL8720DN%20(BW16)-blue)
![Band](https://img.shields.io/badge/Wi--Fi-2.4GHz%20%26%205GHz-crimson)
![Control](https://img.shields.io/badge/Control-UART%20%7C%20WebUI%20%7C%20BLE%205.0-success)
![Android](https://img.shields.io/badge/Android-Kotlin%20%2B%20Compose-7F52FF)
![License](https://img.shields.io/badge/License-MIT-yellow)

基于 [Evil-BW16](https://github.com/7h30th3r0n3/Evil-BW16) 深度改造的便携式双频（2.4GHz / 5GHz）Wi-Fi 安全测试与协议分析工具，硬件平台为 **BW16 (Realtek RTL8720DN)**。

支持 **三通道控制**，上电即用，可随时切换或并行使用：

| 通道 | 入口 | 适用场景 |
|------|------|---------|
| 🖥️ **UART 串口** | `115200` baud | 桌面端调试、自动化脚本控制 |
| 🌐 **Wi-Fi WebUI** | 热点 `WiFi-Deck`（密码 `12345678`）→ `http://192.168.1.1` | 跨平台手机/电脑浏览器免安装控制 |
| 📱 **BLE 5.0 App** | 原生 Android 客户端（自动发现 `WiFi-Deck` 并连接） | 移动端低功耗便携控制、实时抓包终端（不占手机 Wi-Fi） |

> ⚠️ **免责与法律声明**
> 本项目仅用于无线网络安全教学、802.11 协议研究及经明确书面授权的渗透测试。严禁将本工具用于未授权的网络干扰或非法用途，使用者须自行承担因违规使用造成的一切法律责任。

---

## 一、三通道系统架构

```text
                     ┌──────────────────────────────────────┐
                     │            BW16 (RTL8720DN)          │
                     │                                      │
    UART (115200) ──►│      handleCommand() 统一指令解析      │◄── Wi-Fi WebUI (192.168.1.1)
                     │                  ▲                   │
                     │                  │                   │
    BLE 5.0 NUS   ──►│   cmdRing 环形队列 + JSON Lines 引擎   │◄── Android App (Kotlin Compose)
                     │                  │                   │
                     │                  ▼                   │
                     │   802.11 双频裸帧注入 / 混杂模式嗅探器   │
                     └──────────────────────────────────────┘
```

- **指令集完全统一**：UART 串口、HTTP WebUI 与 BLE 5.0 客户端共用同一套 `handleCommand()` 指令解析器，行为一致。
- **跨线程无锁解耦**：BLE GATT 写回调与 Wi-Fi 混杂模式抓包回调均通过环形队列（`cmdRing` / `sniffLogRing`）与主循环 `loop()` 解耦，彻底避免跨 FreeRTOS 任务竞态与栈溢出。

---

## 二、核心特性与改造亮点

在原版 Evil-BW16 v1.0.2 / v1.0.3 基础上进行了全方位的工程化重构与扩展：

### 1. 完整 LwIP + DHCP 的非隐藏 WebUI 热点
- 原版仅开启隐藏 AP 且未完整初始化 DHCP 服务；本项目使用 `WiFi.apbegin()` 完整拉起 LwIP 协议栈与 DHCP Server，手机连接热点 `WiFi-Deck`（密码 `12345678`）后即可直接访问 `http://192.168.1.1`。
- 混杂模式（Promiscuous Mode）嗅探结束后，自动通过 `wifi_off()` + `startSoftAP()` 完整恢复 AP 热点与 Web 服务。

### 2. 内置响应式暗色 WebUI 与 RESTful JSON API
- 固件内嵌响应式单页 Web 应用（扫描、攻击、嗅探、设置），支持双频信道标识、RSSI 信号强度条、多目标勾选与实时状态轮询。
- 提供完整的 HTTP JSON 接口：

| 路由 | 方法 | 功能 |
|------|------|------|
| `/` | `GET` | 返回内置暗色主题 WebUI 页面 |
| `/api/scan` | `GET` | 获取当前缓存的 Wi-Fi 扫描结果 JSON |
| `/api/scan` | `POST` | 触发双频 Wi-Fi 扫描并返回最新结果 |
| `/api/status` | `GET` | 获取当前所有攻击/嗅探状态、信道及 TX 统计 |
| `/api/info` | `GET` | 获取当前周期、帧数、起始信道、LED 等配置参数 |
| `/api/command` | `POST` | 执行任意控制指令（Body: `cmd=xxx`） |
| `/api/target` | `POST` | 设置多选目标索引（Query: `indices=0,2,5`） |
| `/api/config` | `POST` | 批量更新运行参数（Body: `key=val&key=val`） |

### 3. 全面的 802.11 双频测试与嗅探能力
- **目标与全量测试**：支持多目标定向测试（Deauth / Disassoc）、全信道轮询、单 AP 快速测试（`single_ap`）、随机信道测试（`random_attack`）及定时自动停止（`attack_time <ms>`）。
- **Flood / Jam / 克隆模式**：支持 `beacon_flood`、`probe_flood`、`signal_jam`（全频段快速跳频）以及 `deauth_sniff`（嗅探空中 Deauth 帧并自动提取克隆源 MAC）。
- **混杂模式协议嗅探**：支持捕获 `ALL`、`BEACON`、`PROBE`、`DEAUTH`、`EAPOL`（WPA/WPA2 握手帧）、`PWNAGOTCHI` 帧，支持 2.4GHz/5GHz 自动跳频（`hop on/off`）。
- **底层内存与协议安全加固**：全面修复原版 IE 变长字段越界、802.11 QoS/HTC/4-Addr 头部偏移计算、序列号（Sequence Number）递增与 Duration 字段规范。

### 4. BLE 5.0 (Nordic UART Service) + 原生 Android 客户端
- **不占手机 Wi-Fi**：手机保持正常移动网络或日常 Wi-Fi 连接的同时，通过 BLE 5.0 即可全功能操控开发板。
- **开箱即连**：开发板上电永久广播 `WiFi-Deck`，打开 Android App 自动按 UUID 过滤扫描、协商 `MTU = 247`、订阅 Notify 通道，并自动拉取板端已有扫描结果与配置。
- **四大功能标签页**：
  - **扫描页**：展示 SSID、BSSID、信道、RSSI 与 `2.4G`/`5G` 频段徽章，支持多选目标。
  - **控制页**：涵盖 Deauth、Disassoc、Beacon/Probe Flood、Signal Jam、嗅探克隆、10s/30s/60s 定时测试与一键急停，**运行中项目实时绿色高亮**。
  - **抓包页**：实时滚动展示空中捕获的 802.11 帧日志，顶部支持一键切换过滤模式（`ALL` / `Beacon` / `Probe` / `Deauth` / `EAPOL`）及信道跳频开关。
  - **设置页**：连接状态管理，以及 LED 指示灯、每次发送帧数、循环间隔、扫描时长、起始信道、攻击间隔重扫的**双向实时同步配置面板**。

---

## 三、BLE JSON Lines 通信协议规范

**GATT 服务与特征值（Nordic UART Service）**：

| 角色 | UUID | 属性 |
|------|------|------|
| **Service** | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Primary Service |
| **RX（手机 → 板子）** | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | `Write` / `Write Without Response` |
| **TX（板子 → 手机）** | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | `Notify` |

**上行消息（板子 → 手机，每行一条紧凑 JSON，以 `\n` 结尾）**：

| `t` 类型 | 触发时机 | 关键字段说明 |
|---------|---------|-------------|
| `scan_begin` | 扫描开始或同步列表前 | `{"t":"scan_begin"}` |
| `ap` | 逐条推送热点信息 | `i`: 索引, `s`: SSID, `b`: BSSID, `c`: 信道, `r`: RSSI |
| `scan_end` | 热点列表推送完成 | `n`: 热点总数 |
| `st` | 每 1 秒状态心跳 | `atk`/`dis`/`bcn`/`prb`/`jam`/`snf`: 攻击态, `snfOn`: 嗅探态, `hop`: 跳频态, `ch`: 当前信道, `ok`/`fail`: TX 计数 |
| `cfg` | 连接同步或参数修改后 | `delay`: 循环间隔, `scan`: 扫描时长, `frames`: 帧数, `ch`: 起始信道, `sc`: 间隔重扫, `led`: LED 开关 |
| `log` | 混杂模式捕获到报文时 | `m`: 抓包日志文本（25ms 节流推送 + 安全长度裁剪） |

**下行指令（手机 → 板子，UTF-8 文本以 `\n` 结尾，与串口完全相同）**：

```text
# 扫描与查询
scan                  results               info                  help
# 攻击控制
start deauther        disassoc              random_attack         single_ap <index>
attack_time <ms>      stop
# Flood / Jam / 嗅探克隆
beacon_flood          probe_flood           signal_jam            deauth_sniff
# 混杂模式嗅探与跳频
start sniff           sniff all|beacon|probe|deauth|eapol|pwnagotchi
stop sniff            hop on                hop off               set ch <channel>
# 参数配置（修改后自动回推最新 cfg）
set target <i,j,k>    set num_frames <n>    set cycle_delay <ms>  set scan_time <ms>
set start_channel <c> set scan_cycles on|off                      set led on|off
# 统计管理
tx_stats              reset_txstats
```

---

## 四、快速开始

### 1. 编译与烧录固件（BW16）
1. 在 Arduino IDE 的「首选项 → 附加开发板管理器网址」中添加 Realtek AmebaD 官方源：
   ```text
   https://github.com/ambiot/ambd_arduino/raw/main/Arduino_package/package_realtek_amebad_index.json
   ```
2. 在「开发板管理器」中搜索并安装 **Realtek Ameba Boards (32-bits ARM Cortex-M33 @200MHz)**（推荐版本 `3.1.7`）。
3. 选择开发板 **`RTL8720DN(BW16)`**，打开 `WiFi-Deck.ino` 编译并上传。

### 2. 编译与安装 Android 客户端
- **环境要求**：Android Studio、JDK 17+、Android SDK 36（支持 `minSdk 26`，即 Android 8.0 及以上系统）。
- **构建步骤**：
  1. 使用 Android Studio 打开项目下的 `AndroidApp/` 目录，等待 Gradle 同步完成。
  2. 连接 Android 手机并直接点击 **Run**，或执行 `:app:assembleDebug` 构建安装包（产物位于 `AndroidApp/app/build/outputs/apk/debug/app-debug.apk`）。

---

## 五、项目目录结构

```text
WiFi-Deck/
├── WiFi-Deck.ino              # RTL8720DN 主固件（802.11 注入/嗅探 + BLE NUS 服务 + WebUI + 串口）
├── AndroidApp/                # Android 原生客户端 (Kotlin + Jetpack Compose + MVVM)
│   └── app/src/main/java/com/example/wifideck/
│       ├── data/
│       │   ├── BleManager.kt  # BLE 扫描/连接/MTU 协商/串行发送队列/JSON Lines 分帧器
│       │   └── Models.kt      # ApItem / DeviceStatus / DeviceConfig 数据模型
│       ├── ui/
│       │   ├── MainViewModel.kt # 状态管理与指令封装
│       │   └── MainScreen.kt    # Compose 暗色主题四标签页 UI
│       └── MainActivity.kt    # 动态权限申请与自动扫描入口
├── LICENSE                    # MIT 开源许可证
└── README.md
```

---

## 六、版本演进记录

- **v1.0**: 初版发布，基于 Evil-BW16 v1.0.2 新增 5GHz WebUI 控制端。
- **v1.1**: 修复 AP 参数顺序、函数前向声明、前端脚本加载与 HTTP 状态码规范。
- **v1.2**: 完成 11 项系统级健壮性与内存安全加固（混杂模式退出后完整重建 LwIP/DHCP AP、扫描竞态同步、802.11 变长头与 IE 边界校验、HTTP 分块传输与防粘包、XSS 转义）。
- **v1.3**: 合并上游 v1.0.3 特性（Sequence Number 轮换、Duration 修正、TX 统计、Beacon/Probe Flood、Signal Jam、Deauth Sniff-and-Clone）。
- **v1.4**: 新增 BLE 5.0 Nordic UART Service 通道与 Android 原生 Jetpack Compose 客户端，确立三通道统一控制架构。
- **v1.5**: 完善 BLE 8 槽位命令环形队列、永久广播标志、Android 8.0~13+ 全版本兼容、40ms 串行 GATT 写队列、扫描/连接超时协程管控，并补齐 App 控制页运行态高亮、抓包页过滤/跳频/自动滚屏及设置页双向实时调参面板。

---

## 七、致谢

- [7h30th3r0n3/Evil-BW16](https://github.com/7h30th3r0n3/Evil-BW16) — 原版固件基础
- [tesa-klebeband/RTL8720dn-Deauther](https://github.com/tesa-klebeband/RTL8720dn-Deauther) — RTL8720DN 底层 802.11 裸帧注入研究
