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

- **指令集完全统一**：UART 串口、HTTP WebUI 与 BLE 5.0 客户端共用同一套 `executeCommand()` / `handleCommand()` 指令解析器，行为一致。
- **跨线程无锁解耦与持久化安全**：BLE GATT 写回调与 Wi-Fi 混杂模式抓包回调分别通过 `16` 槽位 `cmdRing`（带跨通道急停优先抢占）与 `24` 槽位 `sniffLogRing`（UTF-8 安全截断与丢帧计数）和主循环 `loop()` 解耦；`scan_results` 由 `scanMutex` 互斥快照保护；配置持久化采用 `Magic + CRC32` 校验与变更比对免冗余 Flash 擦写。

---

## 二、核心特性与改造亮点

在原版 Evil-BW16 v1.0.2 / v1.0.3 基础上进行了全方位的工程化重构与扩展：

### 1. 完整 LwIP + DHCP 的非隐藏 WebUI 热点
- 原版仅开启隐藏 AP 且未完整初始化 DHCP 服务；本项目使用 `WiFi.apbegin()` 完整拉起 LwIP 协议栈与 DHCP Server，手机连接热点 `WiFi-Deck`（密码 `12345678`）后即可直接访问 `http://192.168.1.1`。
- 混杂模式（Promiscuous Mode）嗅探结束后，自动通过 `wifi_off()` + `startSoftAP()` 完整恢复 AP 热点与 Web 服务。

### 2. 内置响应式暗色 WebUI 与 RESTful JSON API
- 固件内嵌响应式单页 Web 应用（扫描、控制、抓包、设置），支持双频信道标识、RSSI 信号强度条、多目标勾选、实时状态轮询与请求超时中止（`AbortController`）。
- HTTP 接口采用同源访问（移除宽松 `Access-Control-Allow-Origin: *`）、严格 `Content-Length` 请求体解析及分块写回控制，保障大体积响应完整下发：

| 路由 | 方法 | 功能 |
|------|------|------|
| `/` | `GET` | 返回内置暗色主题 WebUI 页面（4 标签页：扫描、控制、抓包、设置） |
| `/api/scan` | `GET` | 获取当前缓存的 Wi-Fi 扫描结果 JSON |
| `/api/scan` | `POST` | 触发双频 Wi-Fi 扫描并返回最新结果 |
| `/api/status` | `GET` | 获取当前所有攻击/嗅探状态、信道、目标数、跳频与 TX 统计 |
| `/api/info` | `GET` | 获取当前周期、帧数、起始信道、LED、热点 SSID 与 BLE 名称等配置（`pass` 字段脱敏返回空串 `""`，留空保存维持原密码） |
| `/api/logs` | `GET` | 获取最近捕获的 802.11 混杂模式抓包历史记录 JSON |
| `/api/logs/clear` | `POST` | 清空抓包历史（与 `clear_logs` 对齐，同步清空 WebUI 历史缓冲、BLE 日志环与丢包统计） |
| `/api/command` | `POST` | 执行控制指令（Body: `cmd=xxx`；禁止通过 WebUI 执行 `ap off` 并返回 `403`；抓包/嗅探克隆前弹窗确认热点暂停，固件先发回 HTTP `200` 再切混杂模式，纯 Web 触发自带 `15s` 自动恢复与页面倒计时自动重连回读） |
| `/api/target` | `POST` | 设置多选目标索引（Body/Query: `indices=0,2,5` / `all` / 空值清空，严格校验索引合法性） |
| `/api/config` | `POST` | 批量更新运行参数与设备身份（Body: `key=val&key=val`，校验通过后写入 Flash） |

### 3. 全面的 802.11 双频测试与嗅探能力
- **目标与全量测试（带防误攻保护）**：支持多目标定向测试（Deauth / Disassoc）、全信道轮询、单 AP 快速测试（`single_ap`）、随机信道测试（`random_attack`）及定时自动停止（`attack_time <ms>`）。**禁止无目标时回退全攻**——App 与 WebUI 优先下发带目标索引的原子指令（`<i,j,k>` 或显式 `all`），未选目标时固件直接拒绝执行。
- **Flood / Jam / 克隆模式**：支持 `beacon_flood`、`probe_flood`、`signal_jam`（全频段快速跳频）以及 `deauth_sniff`（嗅探空中 Deauth 帧并自动提取克隆源 MAC）。
- **混杂模式协议嗅探**：支持捕获 `ALL`、`BEACON`、`PROBE`、`DEAUTH`、`EAPOL`（WPA/WPA2 握手帧）、`PWNAGOTCHI` 帧，支持 2.4GHz/5GHz 自动跳频（`hop on/off`）。
- **底层内存与协议安全加固**：全面修复空指针先解引用、SSID 栈边界、IE 变长字段越界、802.11 QoS/HTC/4-Addr 头部偏移计算、序列号（Sequence Number）递增与 Duration 字段规范。

### 4. BLE 5.0 (Nordic UART Service) + 原生 Android 客户端
- **不占手机 Wi-Fi**：手机保持正常移动网络或日常 Wi-Fi 连接的同时，通过 BLE 5.0 即可全功能操控开发板。
- **完善前置门禁与可靠链路**：按系统版本校验蓝牙与定位前置条件（Android 12+ `BLUETOOTH_SCAN`/`BLUETOOTH_CONNECT`，Android 8–11 精确位置权限及 `LocationManagerCompat` 定位总开关检测），提供权限说明弹窗、永久拒绝跳转设置、系统蓝牙开关拉起与状态广播自动重连；协商 `MTU = 247` 并以有效 ATT MTU 全程限制指令载荷、串行背压重试及字节级 UTF-8 流式 `\n` 分帧，支持配置变更不断连与 Edge-to-Edge 界面适配。
- **四大功能标签页**：
  - **频谱扫描页**：展示 SSID、BSSID、信道、RSSI 与 `2.4G`/`5G` 频段徽章，支持搜索、频段筛选、排序、多选/全选及按索引去重。
  - **射频控制台**：涵盖 Deauth、Disassoc、单目标点射、随机抽样、Beacon/Probe Flood、Signal Jam、嗅探克隆、10s/30s/60s 定时测试与一键急停；具备 0 目标防误攻拦截与**运行态实时绿色高亮**。
  - **协议抓包页**：采用 `ArrayDeque` 与 75ms 合批渲染高频空中 802.11 帧日志，顶部支持一键切换过滤模式（`ALL` / `EAPOL` / `Deauth` / `Beacon` / `Probe` / `Pwnagotchi`）、跳频开关及常用双频信道锁频，支持滚屏锁、复制、系统分享导出及双端同步清空（已连 BLE 时自动下发 `clear_logs`）。
  - **设备参数页**：连接状态管理、**设备身份伪装（自定义 BLE 蓝牙广播名称、Wi-Fi 热点 SSID 与 WPA2 密码，支持 `<FlashMemory.h>` 掉电记忆与一键恢复默认）**、`ap off` 隐身模式开关（仅 BLE/串口允许）、需 `cfg`+`id` 双帧同步后才开放的双向防抖调参面板（切 Tab 与断连均保留未保存草稿）以及 CLI 指令直发终端。

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
| `scan_begin` | 主动扫描开始前 | `{"t":"scan_begin"}` |
| `sync_begin` | `results` 回读或攻击间隙重扫同步前 | `{"t":"sync_begin"}`（不干扰 App 主动扫描状态与超时看门狗） |
| `ap` | 逐条推送热点信息 | `i`: 索引, `s`: SSID, `b`: BSSID, `c`: 信道, `r`: RSSI, `sel`: 可选 `1` 表示已选中 |
| `scan_end` | 主动扫描推送完成 | `n`: 热点总数（扫描失败时附加 `err: 1`） |
| `sync_end` | 同步列表推送完成 | `n`: 热点总数 |
| `st` | 每 1 秒状态心跳或状态变更时 | `atk`/`dis`/`bcn`/`prb`/`jam`/`snf`: 攻击态, `snfOn`: 嗅探态, `sMode`: 过滤模式, `hop`: 跳频态, `ch`: 当前信道, `ok`/`fail`: TX 计数, `rem`: 定时倒计时(秒) |
| `cfg` | 连接同步或参数修改后 | `delay`: 循环间隔, `scan`: 扫描时长, `frames`: 帧数, `ch`: 起始信道, `sc`: 间隔重扫, `led`: LED 开关, `ap`: 热点开关 |
| `id` | 连接同步或名称/密码修改后 | `ssid`: 当前 Wi-Fi 热点名称, `pass`: 当前 Wi-Fi 密码, `ble`: 当前 BLE 蓝牙名称 |
| `log` | 混杂模式捕获到报文或系统告警时 | `m`: 抓包日志或状态消息（按有效 ATT MTU 动态分片与 UTF-8 安全裁剪） |

**下行指令（手机 → 板子，UTF-8 文本以 `\n` 结尾，单条上限 219B，与串口共享解析器）**：

```text
# 扫描与查询
scan                  results               info                  help
# 攻击控制（未选目标时禁止无参默认全攻；全量测试需显式传入 all）
start deauther <i,j|all>  disassoc <i,j|all>  random_attack       single_ap <index>
attack_time <ms> <i,j|all> stop
# Flood / Jam / 嗅探克隆
beacon_flood          probe_flood           signal_jam            deauth_sniff
# 混杂模式嗅探与跳频
start sniff           sniff all|beacon|probe|deauth|eapol|pwnagotchi
sniff_time <ms> [m]   stop sniff            clear_logs
hop on                hop off               set ch <channel>
# 参数配置（修改后自动写入 Flash 并回推最新 cfg / id；ap off 仅允许 BLE 与串口）
set target <i,j,k|all> set num_frames <n>   set cycle_delay <ms>  set scan_time <ms>
set start_channel <c> set scan_cycles on|off  set led on|off      ap on|off
# 设备身份伪装与重置（掉电记忆）
set ble_name <name>   set ap_ssid <ssid>    set ap_pass <pass>    reset_config
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

### 2. 编译与测试 Android 客户端
- **环境要求**：Android Studio、JDK 17+、Android SDK 36（支持 `minSdk 26`，即 Android 8.0 及以上系统）。
- **构建与测试步骤**：
  1. 使用 Android Studio 打开项目下的 `AndroidApp/` 目录，等待 Gradle 同步完成。
  2. 在 `AndroidApp/` 目录下执行 `./gradlew testDebugUnitTest` 运行离线 JVM 单元测试；执行 `./gradlew assembleDebug` 构建安装包（产物位于 `AndroidApp/app/build/outputs/apk/debug/app-debug.apk`），或连接手机直接点击 **Run**。

---

## 五、项目目录结构

```text
WiFi-Deck/
├── WiFi-Deck.ino              # RTL8720DN 主固件（802.11 注入/嗅探 + BLE NUS 服务 + WebUI + 串口）
├── AndroidApp/                # Android 原生客户端 (Kotlin + Jetpack Compose + MVVM)
│   ├── app/src/main/java/io/github/xiangwen/wifideck/
│   │   ├── data/
│   │   │   ├── BleManager.kt  # BLE 扫描/连接/MTU 协商/串行发送队列/UTF-8 JSON Lines 分帧器
│   │   │   └── Models.kt      # ApItem / DeviceStatus / DeviceConfig / SniffLogEntry 数据模型
│   │   ├── ui/
│   │   │   ├── theme/DeckColors.kt          # 战术暗色主题色板与信号强度映射
│   │   │   ├── components/CommonComponents.kt # 顶部实时遥测仪表盘、门禁提示弹窗与战术卡片组件
│   │   │   ├── tabs/ScanTab.kt              # 频谱扫描页（搜索/频段统计/信号质量条/目标速跳）
│   │   │   ├── tabs/ControlTab.kt           # 射频控制台（目标概览/TX统计/2x2网格/定时与急停）
│   │   │   ├── tabs/SniffTab.kt             # 协议抓包页（协议着色/信道锁定/滚屏锁/导出与双端清空）
│   │   │   ├── tabs/SettingsTab.kt          # 设备参数页（身份伪装/隐身开关/分段调参/CLI直发终端）
│   │   │   ├── MainViewModel.kt             # 状态流管理、75ms 合批节流与协议指令封装
│   │   │   └── MainScreen.kt                # Scaffold 导航骨架、Saveable 状态保持与全局提示
│   │   └── MainActivity.kt    # BLE 前置权限/蓝牙/定位门禁链、状态广播监听与 Edge-to-Edge 入口
│   └── app/src/test/java/io/github/xiangwen/wifideck/ # 离线 JVM 单元测试（Models / BleManager / MainViewModel）
├── AUDIT_FIX_PLAN.md          # 全面审计修复计划与决策 A/B/C/D 历史归档
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
- **v1.6**: 优化双频信道跳频边界与非嗅探态混杂模式保护，支持 UTF-8 中文 SSID 解析、双频全信道 Signal Jam 轮询及 `ap on/off` 热点开关；Android 端新增频段筛选、RSSI 排序、全选操作与抓包日志复制。
- **v1.7**: 新增 WebUI 2.0 四标签页战术界面（补齐 2.4G/5G 筛选、排序、全选、TX 统计、恢复出厂与 `/api/logs` 抓包终端）、纯 WebUI 15s 安全定时嗅探保护（`sniff_time`）、多目标循环中途急停中断（`shouldAbortCycle`）、`scan_results` 容量上限与 BLE 超长 SSID 安全截断、Android 端 BLE 意外断线自动重连、定时任务倒计时显示及抓包日志系统级分享导出。
- **v2.0**: 完成 Batch 1–3 全链路安全加固、协议对齐与发版收口：
  - **固件稳定性与安全**：修复 `wifi_tx_raw_frame` 空指针解引用、`ssid_len` 栈越界与 `promisc_callback`/`scan_results` 跨线程竞态；补全 `deauth_sniff` 蓝牙共存恢复与切模信道校准；落地 `PersistConfig` Magic+CRC32 校验与防冗余擦写、`16` 槽位 `cmdRing` 与跨通道急停优先抢占、动态 ATT MTU 分片与 UTF-8 安全截断、HTTP `Content-Length`/分块发送与严格校验。
  - **跨端协议与防误攻策略**：禁止无目标时回退全攻，App/WebUI 改用带参或显式 `all` 攻击指令；禁用 WebUI 执行 `ap off` 并补充抓包中断弹窗与 15s 自动重连回读；`GET /api/info` 脱敏密码并移除宽松 CORS；`clear_logs` 双端对齐清理固件日志环与丢包统计；新增 `sync_begin`/`sync_end` 隔离间隙重扫同步。
  - **Android 门禁、性能与测试**：落地 Android 8–14+ BLE 权限/蓝牙/定位开关前置门禁链与配置变更不断连；引入字节级 UTF-8 流式分帧、全链路有效 MTU 校验、`ArrayDeque` + 75ms 合批与细粒度 StateFlow 优化高频抓包重组并保持切 Tab/断连草稿；修正权限弹窗硬件型号描述，并补齐覆盖协议模型、BLE 编解码分帧与 ViewModel 核心状态的离线 JVM 单元测试。

---

## 七、致谢

- [7h30th3r0n3/Evil-BW16](https://github.com/7h30th3r0n3/Evil-BW16) — 原版固件基础
- [tesa-klebeband/RTL8720dn-Deauther](https://github.com/tesa-klebeband/RTL8720dn-Deauther) — RTL8720DN 底层 802.11 裸帧注入研究
