# WiFi-Deck

[![Firmware CI](https://github.com/xiangwen-ctrl/WiFi-Deck/actions/workflows/firmware.yml/badge.svg)](https://github.com/xiangwen-ctrl/WiFi-Deck/actions/workflows/firmware.yml)
[![Android CI](https://github.com/xiangwen-ctrl/WiFi-Deck/actions/workflows/android.yml/badge.svg)](https://github.com/xiangwen-ctrl/WiFi-Deck/actions/workflows/android.yml)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=xiangwen-ctrl_WiFi-Deck&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=xiangwen-ctrl_WiFi-Deck)
[![Release](https://img.shields.io/github/v/release/xiangwen-ctrl/WiFi-Deck)](https://github.com/xiangwen-ctrl/WiFi-Deck/releases/latest)
![Platform](https://img.shields.io/badge/Platform-BW16--Kit%20(RTL8720DN)-blue)
![Band](https://img.shields.io/badge/Wi--Fi-2.4GHz%20%26%205GHz-crimson)
![Control](https://img.shields.io/badge/Control-UART%20%7C%20WebUI%20%7C%20BLE%205.0-success)
![License](https://img.shields.io/badge/License-GPLv3-blue)

基于 **Ai-Thinker BW16-Kit（Realtek RTL8720DN）** 的便携式 2.4GHz / 5GHz 双频 Wi-Fi 测试与 802.11 协议分析工具。  
在 [Evil-BW16](https://github.com/7h30th3r0n3/Evil-BW16) 的基础上重写了固件调度与通信链路，支持 **BLE 5.0 Android 客户端**、**板载 WebUI** 和 **UART 串口** 3种控制方式，开发板上不用接屏幕或按键就能直接用。用手机蓝牙连接控制时，手机可以正常走移动数据上网，开发板抓包、跳频或关闭热点隐身时也不会断联。

> ⚠️ 本项目仅用于无线安全教学、802.11 协议研究及已获得明确授权的测试场景，请勿用于任何未授权的网络干扰，使用者需自行承担相应法律责任。

---

## 界面预览

### Android 客户端（BLE 5.0）

<p align="center">
  <img src="docs/screenshots/app-scan.jpg" width="45%" alt="Android 频谱扫描页面" />
  &nbsp;&nbsp;
  <img src="docs/screenshots/app-settings.jpg" width="45%" alt="Android 设备参数与链路状态页面" />
</p>

### 板载 WebUI（`http://192.168.1.1`）

<p align="center">
  <img src="docs/screenshots/webui.png" width="92%" alt="板载暗色 WebUI 扫描与控制界面" />
</p>

### 三端功能对照

| 模块 | Android 客户端（BLE 5.0，推荐） | 板载 WebUI（`http://192.168.1.1`） | UART 串口 CLI（`115200`） |
|---|---|---|---|
| 状态栏 | BLE 连接态、重连进度、当前信道、热点/目标/TX 统计、一键急停 | 定时轮询 `/api/status`、运行状态、TX 计数、BLE 在线提示、一键急停 | 打印启动诊断、信道切换、TX 统计与 `stop` 急停 |
| 扫描页 | 2.4G / 5G 频段计数过滤、SSID/BSSID 搜索、RSSI 排序、多选/全选 | 2.4G / 5G 标签、信号条、动态扫描倒计时、多目标勾选 | `scan`、`results`、`set target <i,j\|all>` |
| 控制页 | Deauth / Disassoc / 单目标点射 / Flood / Jam / 定时测试、运行态高亮 | 已选目标概览、定时压测、零目标拦截与单目标点射 | `start deauther`、`disassoc`、`single_ap`、`attack_time`、`beacon_flood`、`probe_flood`、`signal_jam`、`deauth_sniff` |
| 抓包页 | 6种过滤模式、自动跳频、常用双频信道锁定、滚屏锁、导出分享、同步清空 | 查看历史 `/api/logs`；Web 端开启抓包会自动按15秒定时并在结束后重连恢复 | `start sniff`、`sniff <mode>`、`sniff_time`、`hop on/off`、`set ch`、`clear_logs` |
| 设置页 | 自定义 BLE 名称 / 热点 SSID / 密码（写 Flash，按 `RST` 重启生效）、`ap off` 隐身开关 | 修改运行参数、密码脱敏读取（留空不改）、恢复默认（禁止 `ap off` 防断联） | `set <key> <val>`、`ap on/off`、`info`、`reset_config` |

---

## 硬件与控制方式

### BW16-Kit 硬件说明

**Ai-Thinker BW16-Kit** 开发板采用安信可 BW16 模组（核心 SoC 为 **Realtek RTL8720DN**，内置 **ARM V8 KM4 + ARM V8M KM0** 双核，分别兼容 Cortex-M4F 与 Cortex-M0），单颗芯片集成了：
- **2.4GHz + 5GHz 双频 Wi-Fi（802.11 a/b/g/n）**：支持 2.4GHz（CH 1–13）与 5GHz（CH 36–165），可跑 SoftAP 热点、STA 双频扫描、原始管理帧注入和混杂模式监听；
- **BLE 5.0 低功耗蓝牙**：与 Wi-Fi 共用天线但协议栈独立，Wi-Fi 切信道抓包或关热点时蓝牙依然能正常通信；
- **板载外设**：Type-C 供电（建议 `5V > 500mA`）与 **CH340C** USB 转串口（接 `PA7_LOG_TX` / `PA8_LOG_RX`）、三色 RGB LED（`PA12` / `PA13` / `PA14`）、`RST` 复位键和 `BURN` 烧录键。

官方资料已整理在 [docs/hardware/](docs/hardware/) 目录，需要查引脚或电路可以直接打开：
- [BW16-Kit_V1.2.3_Specification.pdf](docs/hardware/BW16-Kit_V1.2.3_Specification.pdf)（开发板规格书：引脚定义、电气与射频参数、按键与 LED 说明）
- [BW16-Kit_V1.2_Schematic.pdf](docs/hardware/BW16-Kit_V1.2_Schematic.pdf)（V1.2 原理图：Type-C / CH340C 自动下载电路与 GPIO 连接）
- [BW16_V1.2.2_Datasheet_CN.pdf](docs/hardware/BW16_V1.2.2_Datasheet_CN.pdf)（BW16 模组中文规格书：RTL8720DN 架构、天线与供电设计）

### 为什么推荐用蓝牙（BLE 5.0）控制？

开发板本身只有一套 Wi-Fi 射频前端，如果靠连接板载 `WiFi-Deck` 热点来操控（WebUI），会遇到两个实际问题：
1. 板载热点不接外网，单无线网卡的电脑连上后就没法再通过 Wi-Fi 上网，手机通常也只能靠移动数据；
2. 一旦进入混杂模式抓包或者用 `ap off` 关掉热点隐身，板载 Wi-Fi 热点必然断开，网页控制台也就跟着失联了。

走 **BLE 5.0 蓝牙控制（Android App）** 可以把控制链路和 Wi-Fi 射频拆开：
- 开发板上不用焊接屏幕或按键，插上 USB 电源或充电宝就能工作，所有操作都在手机里完成；
- 手机走蓝牙控制不占用 Wi-Fi，可以继续用 4G/5G 移动数据（或正常连家里的路由器 Wi-Fi）上网；开发板抓包跳频或关热点隐身期间，蓝牙连接也一直在线，随时能看报文和一键急停；
- 在 App 内点顶部“连接”即可自动扫描直连，不用去手机系统蓝牙设置里手动配对；BLE 数据通道和蓝牙耳机的音频通道（A2DP / HFP）互相独立，连着板子时照样可以戴蓝牙耳机听歌通话。

### 三种控制方式对比

| 控制方式 | 怎么连 | 适用场景 |
|---|---|---|
| **BLE 5.0 Android App（主力推荐）** | 打开 App 点顶部“连接”即可 | 不占 Wi-Fi（手机可正常上网）、不影响蓝牙耳机；支持后台保活、断线自动重连、抓包流实时导出以及 `ap off` 隐身模式，适合日常和外出使用。 |
| **板载 WebUI** | 连热点 `WiFi-Deck`（密码 `12345678`），打开 `http://192.168.1.1` | 不用装 App，手机或电脑浏览器都能直接用；抓包时会自动定时15秒并在结束后恢复热点（单网卡电脑连此热点期间无法通过 Wi-Fi 上外网）。 |
| **UART 串口 CLI** | USB 接电脑串口，波特率 `115200` | 可用全部底层命令并直接看串口运行/内存日志，适合桌面调试和开发。 |

---

## 系统架构

### 整体结构

```mermaid
flowchart LR
    subgraph CH["1. 三通道控制输入"]
        direction TB
        APP["Android 客户端 (BLE 5.0 NUS)"]
        WEB["板载 WebUI (HTTP 192.168.1.1:80)"]
        CLI["UART 串口终端 (115200 baud)"]
    end

    subgraph SCHED["2. 固件统一调度与配置层"]
        direction TB
        RING["命令队列 (cmdRing 16槽 / serialCmdQueue 8槽)"]
        STOP["跨通道急停抢占 (emergencyStopRequested / priorityStopPending)"]
        DISPATCH["统一指令解析器 (handleCommand / executeCommand)"]
        FLASH["Flash 掉电配置 (PersistConfig: Magic + CRC32)"]
    end

    subgraph RF["3. RTL8720DN 双频射频执行层"]
        direction TB
        SCAN["2.4G / 5G 扫描引擎 (scanNetworks + scanMutex)"]
        TX["时间片 802.11 帧注入 (stepSlicedTxCycle)"]
        SNIFF["混杂抓包与双频跳频 (promisc_callback + PromiscFlightGuard)"]
    end

    subgraph UP["4. 上行流控与三端状态回传"]
        direction TB
        HUB["双环形日志与扫描游标 (sniffLogRing / webLogHistory / bleScanPush)"]
        BLE_TX["BLE GATT Notify 上行 (MTU 247 + TX Credit 分级限流)"]
        HTTP_TX["lwIP BSD Socket 响应 (SO_REUSEADDR + 15秒抓包自愈)"]
        UART_TX["UART 串口实时回显 (≤ 25 行/秒 节流输出)"]
    end

    APP -->|"bleWriteCB 入队 cmdRing"| RING
    CLI -->|"pollSerialInput 入队 serialCmdQueue"| RING
    RING --> DISPATCH
    WEB -->|"handleWebClient 同步调用"| DISPATCH

    APP -.->|"stop 抢占"| STOP
    WEB -.->|"stop 抢占"| STOP
    CLI -.->|"stop 抢占"| STOP

    DISPATCH <-->|"memcmp 防磨损读写"| FLASH
    DISPATCH --> SCAN
    DISPATCH --> TX
    DISPATCH --> SNIFF
    STOP -.->|"shouldAbortCycle 即时打断"| SCAN
    STOP -.->|"shouldAbortCycle 即时打断"| TX
    STOP -.->|"shouldAbortCycle 即时打断"| SNIFF

    SCAN --> HUB
    TX --> HUB
    SNIFF --> HUB
    HUB -->|"st / ap / log / cfg"| BLE_TX
    HUB -->|"GET /api/status /scan /logs"| HTTP_TX
    HUB -->|"诊断与抓包报文"| UART_TX

    classDef client fill:#1e293b,stroke:#38bdf8,stroke-width:1.5px,color:#f8fafc
    classDef sched fill:#172554,stroke:#60a5fa,stroke-width:1.5px,color:#f8fafc
    classDef radio fill:#3b0764,stroke:#c084fc,stroke-width:1.5px,color:#f8fafc
    classDef uplink fill:#064e3b,stroke:#34d399,stroke-width:1.5px,color:#f8fafc
    classDef urgent fill:#450a0a,stroke:#f87171,stroke-width:1.5px,color:#fecaca

    class APP,WEB,CLI client
    class RING,DISPATCH,FLASH sched
    class STOP urgent
    class SCAN,TX,SNIFF radio
    class HUB,BLE_TX,HTTP_TX,UART_TX uplink
```

### 固件端实现细节（多模块架构）

- **模块化拆分与零堆碎片热路径**  
  固件按职责拆分为 `WiFi-Deck.ino` 主控调度与 `config`、`ble`、`wifi_scan`、`wifi_attack`、`sniff`、`web`、`command`、`utils` 共 9 个模块，将 `<lwip/sockets.h>` 宏污染严格隔离在 `web.cpp` 编译单元内。扫描结构体 `WiFiScanResult` 使用定长 `char ssid[33]` 与 `char bssid_str[18]`（配合 `std::sort` 原地排序），消除 `scanMutex` 临界区内的动态堆分配；HTTP 大报文（`/api/scan`、`/api/logs`）采用两遍精确计算 `Content-Length` + `1KB` 栈缓冲流式写出，杜绝长期运行堆内存碎片。
- **指令入口与跨通道急停**  
  BLE 写回调 `bleWriteCB` 运行在蓝牙协议栈线程中，收到指令后会写入带临界区保护的16槽环形队列 `cmdRing`，避免阻塞蓝牙任务；串口输入由 `pollSerialInput` 非阻塞读入8槽队列 `serialCmdQueue`，两者都在主循环 `loop()` 里交给 `handleCommand` / `executeCommand` 顺序执行。Web 端请求（`handleWebClient`）则在解析 `/api/command` 等接口后同步调用 `executeCommand(..., fromHttp = true)`，以便立即返回 HTTP `200`/`400`/`403` JSON 状态。  
  只要任意通道收到 `stop`，就会立刻置位 `emergencyStopRequested` 和 `priorityStopPending`。发包循环由时间片状态机 `stepSlicedTxCycle` 驱动（每次最多切片发送8帧并喂狗），配合 `shouldAbortCycle()` 在扫描等待与发包间隙检查急停标志，实现随时打断。
- **并发安全与防误攻**  
  异步扫描回调 `scanResultHandler` 与主循环之间通过 FreeRTOS 互斥锁 `scanMutex` 同步；通过 `pumpBleScanPush` 游标状态机在主循环里分步推送热点列表，防止热点较多时阻塞主循环。混杂抓包引入了在途回调计数 `PromiscFlightGuard`（`promiscCallbackInFlight`），切信道、发送克隆帧或退出混杂模式前会等在途回调完全结束；抓包文本经 UTF-8 裁剪后分别写入24槽实时流缓冲 `sniffLogRing`（推给 BLE/串口）和32槽历史缓冲 `webLogHistory`（递增全局序号 `webLogTotalSeq` 供 `/api/logs?since=` 增量拉取）。  
  `wifi_tx_raw_frame` 对驱动内部结构体做逐级判空与帧长（`<= 1500B`）校验，Beacon/Probe 构造器限制 SSID `<= 32B` 与96B 栈缓冲边界；App 与 WebUI 下发攻击时必须带目标索引（如 `start deauther 0,2,5`），未选目标直接拒绝，全量发包需要显式传 `all`。
- **BLE 上行分级背压限流**  
  抓包高峰期如果无限制地发 Notify 会耗尽协议栈 TX credit，把每秒1次的 `st` 心跳饿死并触发 `0x0108` 超时断连。`bleSendSniffLog` 和主循环对此做了分级流控：`EAPOL`、`Deauth`、`Disassoc` 与 `Pwnagotchi` 帧只要还有 `>= 1` 个 credit（间隔 `>= 15ms`）就优先发送；普通日志则要求至少保留3个 credit 给心跳和命令响应，并根据 ATT MTU 在 `100ms–800ms` 之间动态退避。
- **WLAN 热模式切换与双频 BLE 共存调度**  
  针对 RTL8720DN 单射频共用本振（`RF_0x18`）与 Wi-Fi/BLE 共用 PTA 天线仲裁的硬件约束，`enterStaMode()` / `startSoftAP()` 改用 SDK 原生 `wifi_set_mode()` 热切换（配合 `dhcps_deinit()`），避免 `wifi_off()` / `wifi_on()` 反复冷重置射频与 PTA 导致 BLE 断连（`0x0108`）及无法重连；`enforceBleCoexPta()` / `restoreBleCoex()` 通过驱动内部 `[pcoex+8]` 互斥锁（`CoexMutexGuard`）保护，避免与驱动线程并发踩踏共存寄存器。双频扫描按 16 批次 `wifi_set_pscan_chan(PSCAN_ENABLE)` 分段执行（所有 5GHz 批次严格限制为 `≤2` 个信道、批次间回切 2.4GHz `CH 1` 留出 BLE 喘息窗口）；双频跳频采用 `2:1` 5GHz/2.4GHz 交错序列（`INTERLEAVED_DUAL_BAND_CHANNELS`），5GHz 定频抓包/发包期间则由 `maintainBle5GHzKeepalive()` 周期回切保活。
- **WebUI Gzip 压缩、增量轮询与 Flash 掉电记忆**  
  前端源码独立维护于 `tools/webui/index.html`，通过 `tools/pack_webui.py` 预压缩为 `webui_gz.h`（`Content-Encoding: gzip`，体积缩减约 `70.7%`）；`/api/status` 支持 FNV-1a `ETag` / `304 Not Modified` 协商，`/api/logs?since=<seq>` 仅回传新增报文并在无新帧时跳过 4KB 缓冲分配。纯 WebUI 用户点抓包时，固件先完整发出 HTTP 200 再关闭热点进入15秒定时抓包，结束后自动拉起 SoftAP 并重绑80端口。配置保存使用 `PersistConfig`（含 `CFG_MAGIC` `0x57444B33`、版本号与 `CRC32` 校验），`saveConfigToFlash` 会先用 `memcmp` 比对，参数没变时不重复擦写 Flash。

### Android 客户端实现细节（`AndroidApp/`）

- **GATT 四阶段串行握手与断线重连状态机**  
  `BleManager`（实现 `BleTransport` 接口并通过 `MainViewModel.Factory` 注入）把扫描、建链、错峰 `400ms` MTU 协商、服务发现、写 CCCD 开启 Notify 与初始状态同步串成带超时的单线流水线，并通过有界写队列与 `WriteAckTracker`（`90ms` 写确认超时、最小 `25ms` 间隔，普通指令最多 8 次 / `stop` 队首插队最多 10 次退避重试）控制下发节奏。意外断线后，`BleReconnectTracker` 会先用缓存设备尝试1次 `4.5s` 快速直连，失败立刻切回 BLE 扫描（按 MAC 锁定原设备，最多退避重试5轮）：

```mermaid
sequenceDiagram
    autonumber
    participant VM as MainViewModel
    participant BM as BleManager
    participant FW as BW16 Firmware (BLE NUS)

    VM->>BM: requestBleConnect() -> startScan()
    BM->>FW: ScanFilter(NUS UUID 6E400001...)
    FW-->>BM: ScanResult (lastDevice cached)
    BM->>FW: connectGatt(TRANSPORT_LE)
    FW-->>BM: onConnectionStateChange(STATE_CONNECTED)
    Note over BM,FW: Delay 400ms to avoid slave_init_mtu_req collision (2.2s fallback)
    BM->>FW: requestMtu(247)
    FW-->>BM: onMtuChanged(mtu=247)
    BM->>FW: discoverServices()
    FW-->>BM: onServicesDiscovered(NUS RX/TX)
    BM->>FW: writeDescriptor(CCCD = ENABLE_NOTIFICATION)
    FW-->>BM: onDescriptorWrite(GATT_SUCCESS)
    BM->>VM: connectionState = CONNECTED
    BM->>FW: sendCommand("info") + sendCommand("results")
    FW-->>VM: Notify {"t":"cfg","ver":"2.2.0",...} + {"t":"id",...} + {"t":"sync_begin"..."sync_end"}
    Note over BM,FW: On unexpected disconnect: 1x 4.5s direct connect -> up to 5x scan fallback
```

- **后台保活与 UTF-8 字节分帧**  
  连接或重连期间自动启动 `connectedDevice` 类型前台服务 `BleKeepAliveService`，防止手机息屏或切后台后系统断掉蓝牙；上行分片由 `BleLineFramer`（单行上限 `4096B`）先按字节攒够整行 `\n` 再解码 UTF-8，避免中文热点名跨包被切坏成乱码。
- **主线程顺序消费与 Compose 渲染优化**  
  BLE 回调收到的数据先写入 `Channel`，由 `MainViewModel` 在主线程顺序消费（每轮最多处理64条后 `yield()`），避免多线程读写状态冲突；数据模型均标记 `@Immutable`，抓包日志和扫描列表通过500槽 `ArrayDeque` 与 `LinkedHashMap` 按 `75ms` 周期合批刷新，配合预缓存日志配色表、`LazyRow` 目标芯片与 `SaveableStateProvider` 隔离各页重组，高负载抓包下界面也能保持流畅。

---

## 快速开始

### 硬件与编译环境

- **开发板**：**Ai-Thinker BW16-Kit**（RTL8720DN），请用支持数据传输的 USB 线接电脑或独立供电口。
- **固件环境**：Arduino IDE 2.x 或 `arduino-cli`，安装 **Realtek Ameba Boards (AmebaD)** **`3.1.7`**，开发板管理器地址：
  ```text
  https://github.com/ambiot/ambd_arduino/raw/master/Arduino_package/package_realtek_amebad_index.json
  ```
- **Android 环境**：手机需 **Android 8.0+**（`minSdk 26`，`targetSdk 36`）；从源码编译需 **JDK 17+** 与 **Android SDK 36**（Build Tools `35.0.0`）。

### 烧录固件

**方式一：Windows 一键烧录（无需安装 Arduino IDE）**  
前往 [GitHub Releases](https://github.com/xiangwen-ctrl/WiFi-Deck/releases/latest) 下载 **`WiFi-Deck-v2.2.0-FlashTool.zip`** 并完整解压，双击 **`一键烧录固件.bat`**：脚本会自动识别 USB 串口，带自动下载电路的 BW16-Kit 直接回车即可免按键自动烧录；旧版开发板或外接串口模块可选模式 `[2]`，按提示**按住板载 `BURN` 键不放 → 短按一下 `RST` 键 → 松开 `BURN` 键**完成烧录。

**方式二：Arduino IDE / 命令行源码烧录**
1. Arduino IDE 打开根目录 `WiFi-Deck.ino`，开发板选 **`Ai-Thinker BW16 (RTL8720DN)`**。
2. **进入下载模式**：支持自动下载的 BW16-Kit 可开启 `Auto Flash Mode: Enable` 直接上传；或手动**按住板载 `BURN` 键不放 → 短按一下 `RST` 键 → 松开 `BURN` 键**。
3. 选好串口点上传即可（串口监视器波特率 **`115200`**）。

命令行编译与烧录（`arduino-cli`）：
```bash
arduino-cli compile --clean --fqbn realtek:AmebaD:Ai-Thinker_BW16 --warnings all .
arduino-cli upload -p COM6 --fqbn realtek:AmebaD:Ai-Thinker_BW16:AutoUploadMode=Enable .
```

### 连接与使用

- **Android 客户端（推荐）**：前往 [GitHub Releases](https://github.com/xiangwen-ctrl/WiFi-Deck/releases/latest) 或 [Gitee Releases](https://gitee.com/xiangwen-ctrl/WiFi-Deck/releases) 下载 **`WiFi-Deck-v2.2.0.apk`** 安装，按提示授予蓝牙/附近设备权限（Android 8–11 需同时开启定位）后在 App 内点顶部连接按钮即可（不用在系统蓝牙里配对，手机可继续上网，也不影响蓝牙耳机）。
- **板载 WebUI**：手机或电脑连上热点（默认名称 **`WiFi-Deck`**，密码 **`12345678`**），浏览器打开 **`http://192.168.1.1`**。
- **UART 串口**：波特率 `115200`，发 `help` 可看完整命令列表。
- **本地编译 App 与跑单元测试**：
  ```bash
  cd AndroidApp
  ./gradlew testDebugUnitTest assembleDebug --no-daemon
  ```
  > Windows PowerShell 请将 `./gradlew` 换为 `.\gradlew.bat`，编译后的安装包在 `AndroidApp/app/build/outputs/apk/debug/app-debug.apk`。

---

## 目录与协议参考

### 目录结构

```text
WiFi-Deck/
├── WiFi-Deck.ino                          # 主控入口：setup() 初始化与 loop() 时间片调度
├── config.h / config.cpp                  # PersistConfig (Magic + CRC32) Flash 读写、固件版本与全局参数
├── ble.h / ble.cpp                        # BLE GATT NUS 服务、异步指令队列与 TX Credit 分级流控
├── wifi_scan.h / wifi_scan.cpp            # 2.4G/5G 双频扫描、定长无堆结构体与 scanMutex 并发保护
├── wifi_attack.h / wifi_attack.cpp        # Deauth / Disassoc / Beacon / Probe 帧构造与时间片注入
├── sniff.h / sniff.cpp                    # 802.11 混杂抓包、PromiscFlightGuard 与双环形日志缓冲
├── web.h / web.cpp                        # lwIP BSD Socket HTTP 服务、Gzip 响应、ETag 304 与增量日志流
├── command.h / command.cpp                # UART / BLE / HTTP 三端统一命令解析与分发
├── utils.h / utils.cpp                    # UTF-8 安全截断、JSON 转义、MAC 解析与 LED 状态控制
├── webui_gz.h                             # 由 pack_webui.py 生成的 Gzip 压缩 WebUI PROGMEM 头文件
├── tools/
│   ├── pack_webui.py                      # WebUI 确定性 Gzip 打包与完整性自检脚本
│   └── webui/index.html                   # 板载暗色 WebUI 独立前端源码
├── AndroidApp/                            # Android 客户端 (Kotlin + Jetpack Compose + MVVM)
│   └── app/src/
│       ├── main/java/io/github/xiangwen/wifideck/
│       │   ├── MainActivity.kt            # 权限/蓝牙/定位门禁链与系统蓝牙广播监听
│       │   ├── data/
│       │   │   ├── BleManager.kt          # BLE 扫描、连接、MTU 协商、串行写队列与超时控制
│       │   │   ├── BleReconnectTracker.kt # 断线重连状态机（快速直连切扫描 + 退避）与 ACK 追踪
│       │   │   ├── BleProtocol.kt         # BleTransport 契约、UTF-8 换行分帧器与 MTU 载荷编解码
│       │   │   ├── BleKeepAliveService.kt # connectedDevice 前台保活服务
│       │   │   └── Models.kt              # @Immutable 状态、配置、热点与抓包日志数据模型
│       │   └── ui/
│       │       ├── MainViewModel.kt       # Factory 依赖注入、业务状态流、75ms 合批节流与防误攻校验
│       │       ├── MainScreen.kt          # 底部四标签页导航与页面状态保持
│       │       ├── components/            # 顶部遥测仪表盘、门禁弹窗与公共卡片
│       │       ├── tabs/                  # ScanTab / ControlTab / SniffTab / SettingsTab
│       │       └── theme/                 # 暗色主题配色
│       └── test/java/io/github/xiangwen/wifideck/
│           ├── data/BleProtocolTest.kt
│           ├── data/BleReconnectTrackerTest.kt
│           ├── data/ModelsTest.kt
│           └── ui/MainViewModelTest.kt
├── docs/
│   ├── hardware/                          # BW16-Kit 官方规格书与原理图 PDF
│   └── screenshots/                       # Android 客户端与 WebUI 界面截图
├── .github/workflows/
│   ├── firmware.yml                       # WebUI Gzip 一致性校验与 Realtek AmebaD 固件编译 CI
│   └── android.yml                        # 单元测试、APK 构建与 Tag 自动发版 CI
├── LICENSE
└── README.md
```

<details>
<summary>📡 展开查看：完整串口/BLE 控制指令、BLE JSON Lines 协议与 HTTP API</summary>

#### 1. 统一控制指令（UART / BLE 共用，单条上限 219B，串口以 `\r` / `\n` 结尾）
```text
# 扫描与查询
scan                        results                     info                    help
tx_stats                    reset_txstats

# 射频测试（未选目标拒绝无参攻击，全量测试需显式传 all）
start deauther <i,j|all>    disassoc <i,j|all>          single_ap <index>       random_attack
attack_time <ms> <i,j|all>  stop [all|deauther]

# Flood / Jam / 嗅探克隆
beacon_flood                probe_flood                 signal_jam              deauth_sniff

# 混杂抓包与信道跳频
start sniff                 sniff all|beacon|probe|deauth|eapol|pwnagotchi
sniff_time <ms> [mode]      stop sniff                  clear_logs
hop on                      hop off                     set ch <channel>

# 参数配置与热点控制（写入 Flash 掉电记忆；ap off 仅允许 BLE 与串口）
set target <i,j,k|all>      set num_frames <n>          set cycle_delay <ms>    set scan_time <ms>
set start_channel <c>       set scan_cycles on|off      set led on|off          ap on|off
set ble_name <name>         set ap_ssid <ssid>          set ap_pass <pass>      reset_config
```

#### 2. BLE GATT（Nordic UART Service）与上行 JSON Lines
- **Service UUID**：`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- **RX (Phone → Board)**：`6E400002-B5A3-F393-E0A9-E50E24DCCA9E`（`Write` / `Write Without Response`）
- **TX (Board → Phone)**：`6E400003-B5A3-F393-E0A9-E50E24DCCA9E`（`Notify`）

| `t` 类型 | 用途 | 核心字段 |
|---|---|---|
| `scan_begin` / `scan_end` | 主动扫描起止 | `n`：热点数量（失败附 `err: 1`） |
| `sync_begin` / `sync_end` | `results` 回读或间隙重扫同步 | `n`：热点数量（不触发主动扫描超时） |
| `ap` | 单条扫描热点 | `i`：索引，`s`：SSID，`b`：BSSID，`c`：信道，`r`：RSSI，`sel`：选中标志 |
| `st` | 每秒或状态变更心跳 | `atk`/`dis`/`bcn`/`prb`/`jam`/`snf`、`snfOn`、`sMode`、`hop`、`ch`、`ok`/`fail`、`rem`、`ap` |
| `cfg` / `id` | 运行配置与设备身份同步 | `delay`、`scan`、`frames`、`ch`、`sc`、`led`、`ap`、`ver` / `ssid`、`pass`、`ble` |
| `log` | 抓包报文与系统提示 | `m`：单行日志文本 |

#### 3. WebUI HTTP API（`192.168.1.1:80`，同源访问）
| 路由 | 方法 | 说明 |
|---|---|---|
| `/` | `GET` | 内置暗色 WebUI 页面（`Content-Encoding: gzip` 预压缩传输） |
| `/api/scan` | `GET` / `POST` | 流式读取扫描缓存 / 触发双频扫描 |
| `/api/status` | `GET` | 查询运行状态、信道、TX 统计与 `ble_connected`（支持 `If-None-Match` / `304 Not Modified`） |
| `/api/info` | `GET` | 查询配置参数与固件 `version`（`pass` 字段脱敏返回空字符串） |
| `/api/logs` / `/api/logs/clear` | `GET` / `POST` | 读取抓包历史（支持 `?since=<seq>` 增量拉取） / 双端同步清空日志 |
| `/api/command` / `/api/target` / `/api/config` | `POST` | 执行控制指令（禁止 `ap off`）、更新目标索引、保存 Flash 配置 |

</details>

---

## 常见问题排查

1. **上传固件时串口连接超时 / 进不去烧录模式？**  
   确认用的是带数据传输的 USB 线并装好 CH340 驱动；点上传前先手动进下载模式：**按住 `BURN` 不放 → 短按 `RST` → 松开 `BURN`**。
2. **全频段发包或扫描时开发板偶尔自动重启？**  
   双频连续发射瞬态电流较大，电脑前置 USB 口或细长线压降大时容易触发欠压复位，换主板后置 USB 口或独立 5V 供电即可。
3. **App 点连接一直扫不到开发板？**  
   Android 8–11 系统扫 BLE 必须同时打开系统的**“位置信息（GPS）”总开关**；另外 BW16 同一时间只能连1台手机，如果刚被别的设备连着请先断开或按 `RST` 复位。
4. **在 WebUI 点开始抓包后网页怎么断开了？**  
   芯片进混杂抓包模式时必须关闭热点，从 Web 端发起抓包会自动跑15秒定时抓包，结束后自动拉起热点和80端口，页面会重连并读取结果；想长时间抓包请用蓝牙 App。
5. **改完自定义 Wi-Fi 或 BLE 蓝牙名后需要按 `RST` 吗？**  
   **是的，建议改完按一下板载 `RST` 键。** 保存时新名称和密码已写入 Flash（断电不丢），但蓝牙在连接状态下不会重新广播、手机系统也会缓存旧设备名，热点切换时部分设备也可能因为缓存连不上，按一下 `RST` 重启板子最稳妥。

---

## 版本记录

- **v2.2.0**：根治关闭热点（`ap off`）、双频扫描（`scan`）、混杂抓包（`start sniff`）、双频跳频（`hop on`）、`deauth_sniff` 及 5GHz 发包期间 BLE 超时断连（`0x0108`）与无法重连的底层共存顽疾：
  - **WLAN 热模式切换与 PTA 互斥保护**：`enterStaMode()` 与 `startSoftAP()` 改用 SDK 原生 `wifi_set_mode()` 热切换（配合 `dhcps_deinit()`），彻底消除 `wifi_off()` / `wifi_on()` 反复冷重置射频/PTA 引发蓝牙失联的问题；新增基于驱动内部 `[pcoex+8]` 互斥锁的 `CoexMutexGuard` 保护 `enforceBleCoexPta()` / `restoreBleCoex()`，同步恢复 `0x778=0x01`、`0x6c0/0x6c4=0x55555555`、`0x6c8=0x0000FFFF` 并清除 KM0 `H2C 0x60` PS-TDMA 残留，混杂抓包运行于 `RTW_MODE_STA`；
  - **双频分段扫描与 5GHz BLE 保活**：针对 RTL8720DN 单射频共用本振（`RF_0x18`）特性，将双频扫描重构为 16 批次 `wifi_set_pscan_chan(PSCAN_ENABLE)` 分段扫描（所有 5GHz 批次严格限制为 `≤2` 个信道、批次间回切 2.4GHz `CH 1` 留出 `95ms` BLE 喘息窗口）并移除扫描等待循环内的无锁 PTA 写入；双频跳频重构为 `INTERLEAVED_DUAL_BAND_CHANNELS`（`2:1` 5GHz/2.4GHz 交错，单次 5GHz 驻留 `≤700ms`），新增 5GHz 定频抓包/发包期间的 `maintainBle5GHzKeepalive()` 保活窗口；
  - **BLE 链路协同与 Android 客户端增强**：优化固件 BLE 连接参数协商（`30–50ms` 间隔、`6000ms` 监督超时）并在 1 秒状态心跳 `st` 新增 `ap` 字段防止热点开关回弹；Android 客户端升级至 `v2.2.0`（`versionCode = 220`），增强 GATT 写入繁忙重试与退避容错、补齐重连后配置与扫描结果自愈同步，单元测试扩充至 47 项。
- **v2.1.0**：将单文件固件重构为 9 模块多文件架构，消除扫描临界区动态 `String` 堆分配并改为栈缓冲流式 HTTP JSON 输出；独立维护 `tools/webui/index.html` 并引入 Gzip 预压缩（Flash 缩减约 `28KB`）、`/api/status` ETag `304` 协商与 `/api/logs?since=` 增量日志拉取；打通全端固件与客户端版本号（`v2.1.0`）；Android 端完成 `BleTransport` 依赖注入解耦、`@Immutable` / `LazyRow` 渲染优化并将单元测试扩充至 43 项；新增 Firmware 编译 CI。
- **v2.0**：修复固件 802.11 帧构造边界、混杂模式与扫描并发问题，WebUI 改用直连 lwIP BSD Socket 解决抓包恢复时80端口绑定失败，加入 BLE 抓包分级限流与 `PersistConfig` CRC32 校验；Android 端新增前台保活、断线重连状态机、权限与定位门禁、75ms 日志合批渲染及32个单元测试，CI 接入单测门禁和自动发版。
- **v1.6**：完善 2.4G/5G 全信道跳频与 `ap off` 隐身模式，Android 客户端拆分为四标签页，支持自定义 AP 与 BLE 名称持久化。
- **v1.5**：完成双频扫描/发包/抓包基础固件、内置 WebUI 与初版 BLE Android 客户端。

---

## 参与贡献

项目还在持续完善中，欢迎通过 Issue 反馈不同手机、Android 版本或 BW16 开发板的兼容问题（建议附上串口日志或复现步骤），也欢迎在固件模块化、802.11 帧解析和客户端体验上直接提 Pull Request：

1. Fork 本仓库并从 `main` 新建分支（`feat/xxx` 或 `fix/xxx`）。
2. 提交前请留意：固件端不要在混杂模式或蓝牙回调里阻塞或分配大块内存，确认编译无新增警告；Android 端涉及协议或状态机改动请补充单测并跑通 `./gradlew testDebugUnitTest assembleDebug --no-daemon`。
3. 发起 Pull Request，简单说明改动内容和验证情况即可。

---

## 致谢与协议

- 感谢 [7h30th3r0n3/Evil-BW16](https://github.com/7h30th3r0n3/Evil-BW16)（MIT License）与 [tesa-klebeband/RTL8720dn-Deauther](https://github.com/tesa-klebeband/RTL8720dn-Deauther) 在 RTL8720DN 802.11 底层研究上的开源基础。
- 本项目基于 [GNU General Public License v3.0 (GPL-3.0)](LICENSE) 开源。
