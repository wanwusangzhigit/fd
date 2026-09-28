# WiFi Direct + Bluetooth File Transfer

一个完整的 Android 应用 + 跨平台桌面客户端 + iOS/iPadOS 客户端，支持以下任意两端互传文件：

* **Android ↔ Android** — Wi-Fi Direct (P2P) + 蓝牙 RFCOMM 双通道
* **Desktop ↔ Desktop** — 同一 LAN 内的 TCP（局域网）或蓝牙（RFCOMM）
* **Desktop ↔ Android** — 桌面端连到 Android 手机上启动的 `FileTransferService`
  的 TCP 监听端口（默认 8988）
* **iOS/iPadOS ↔ 任意** — iOS 端通过 Network.framework 跑 TCP + Bonjour 发现，
  与桌面原生 / Android 都能直连

桌面端是原生 C++17 实现，iOS 端是纯 Swift + SwiftUI，三端共用同一个 FDFT v2
二进制协议，外加一个 native-only 的 SHA-256 trailer 帧用于完整性校验（默认开启，
对 Android 端透明 —— Android reader 只读取 `size` 字节，trailer 自动忽略）。

中文界面（在系统为中文时自动切换）、随传输进度更新的通知栏、点击打开已接收文件、
取消传输、旋转屏状态保留、组主侧反向发送、文件落到公共 `Downloads/` 并可在系统
文件管理器中直接看到。

## 功能

1. **Wi-Fi Direct P2P** — `WifiP2pManager` 实现设备发现、组协商、组主 / 客户端
   双向角色；客户端自动把自己的 IP 上报给组主，组主据此反向连接客户端发送。
2. **蓝牙 `BluetoothSocket`** — RFCOMM 服务端持续监听（不再每接收一次就停）；
   客户端 `createRfcommSocketToServiceRecord` 发送；统一 UUID 与服务名。
3. **设备发现** — 单一 RecyclerView 同时展示 Wi-Fi Direct peers 和蓝牙
   discovered / paired 设备，分组标识每个设备的来源；下拉刷新触发两种传输的重新发现。
4. **多文件选择** — `GetMultipleContents` 一次选多个文件批量发送；
   - Wi-Fi Direct：每个文件独立 TCP 连接并行发送
   - 蓝牙：单 socket 串行发送多个文件
   - 已选文件队列支持旋转屏保留
5. **传输进度** — 双总线（关键事件 + 进度事件）确保 Completed / Failed 等关键
   事件永远不会被 Progress 淹没丢弃；前台通知实时刷新百分比。
6. **传输速率指示** — UI 端基于 Progress 事件序列计算瞬时速率（KB/s）与剩余
   时间（ETA），随每行实时显示。
7. **传输历史** — JSON 文件持久化接收历史，独立 `HistoryActivity` 查看与
   重新打开已接收文件；最多保留 200 条。
8. **取消传输** — 每个传输任务注册取消句柄到全局 `TransferBus`；列表行上提供
   「取消」按钮一键中止；已完成传输可滑动删除。
9. **打开已接收文件** — 已完成行点击通过 FileProvider 启动 ACTION_VIEW；
   传输行根据 MIME 类型显示对应图标（图片 / 视频 / 音频 / PDF / 压缩 / 通用）。
10. **国际化** — `values-zh-rCN/strings.xml` 提供完整中文翻译；plurals 处理
    「1 file / N files」之类的复数。
11. **完整工程** — Gradle Kotlin DSL、Manifest、布局、主题、图标、ProGuard、
    release signing 配置、单测一应俱全。
12. **跨平台桌面客户端** — `native/` 下原生 C++17 实现，Linux + Windows 双端：
    - FDFT v2 协议 wire-compatible with Android
    - 可选 SHA-256 trailer 帧（KIND=3）端到端校验完整性
    - UDP 广播 LAN 内设备发现（无需手工输入 IP）
    - ncurses 交互式 TUI（Linux）/ Win32 蓝牙 + Winsock
13. **iOS / iPadOS 客户端** — `ios/` 下纯 Swift + SwiftUI，iPhone + iPad universal：
    - 通过 Network.framework（NWListener + NWConnection）跑 TCP，与三端互通
    - 通过 Bonjour (`NetService` + `NetServiceBrowser`) 发现 LAN 上的对端
    - SHA-256 trailer 自动校验（接收方为 iOS / 桌面时）
    - 完整测试覆盖：Linux 上跑 SwiftPM 单测，macOS 上跑 Xcode build

## 协议

二进制长度前缀帧（替代旧的脆弱文本协议）：

```
+---------+---------+---------+----------------------------+
| "FDFT"  | VERSION | KIND    | kind-specific payload      |
| 4 bytes | 1 byte  | 1 byte  |                            |
+---------+---------+---------+----------------------------+

KIND = 1 (FILE)
    +---------+---------+---------+
    | nameLen | name    | size    | 后接 size 字节文件内容
    | 4 bytes | N bytes | 8 bytes |
    +---------+---------+---------+

KIND = 2 (REGISTER_IP)
    +---------+---------+
    | ipLen   | ip      |
    | 1 byte  | N bytes |
    +---------+---------+
```

二进制协议解决了旧版文本协议「文件名含空格时接收端解析崩溃」的问题；帧头包含
版本号便于后续升级。

## 接收文件存储位置

| Android 版本 | 路径 | 可见性 |
|--------------|------|--------|
| 10 (API 29)+ | `Downloads/WiFiDirectFileTransfer/`（公共下载目录，通过 `MediaStore.Downloads`） | 系统文件 / 下载管理器立即可见，无需权限 |
| 9 (API 28) 及以下 | `<app-specific>/Download/WiFiDirectFileTransfer/` | 写完后通过 `MediaScannerConnection.scanFile` 触发索引 |

## 要求

- Android Studio Koala (或更新)
- minSdk **24**, targetSdk **35**, compileSdk **35**
- Java **17**（构建脚本已通过 toolchain 锁定）
- 两台物理 Android 设备（模拟器无法做 Wi-Fi Direct / 蓝牙文件传输）

## 架构

```
App                                   // Application 单例
transfer/
  Constants.kt                        // 全局常量（端口、UUID、buffer 等）
  WireProtocol.kt                     // 二进制长度前缀帧编解码
  StreamExt.kt                        // 流式拷贝扩展函数（含取消支持）
  ReceivedFileManager.kt              // 写到哪儿 + 扫索引 + 打开入口
  PeerRegistry.kt                     // 组主侧 MAC→IP 表 / 客户端上报 IP
  TransferBus.kt                      // 进程级事件总线（拆关键 vs 进度两条流）
  TransferEvent.kt                    // sealed 事件 + Formatter
  TransferHistory.kt                  // 传输历史 JSON 持久化
  SpeedTracker.kt                     // UI 端速率与 ETA 采样器
  FileTransferService.kt              // 前台服务：持续 TCP server + IP 上报 + 发送
  BluetoothConnection.kt              // 蓝牙发送 / 接收连接的抽象
manager/
  WiFiDirectManager.kt                // Wi-Fi P2P 生命周期 + 连接超时清理
  BluetoothManager.kt                 // BT 发现 + 持续 accept 服务端
ui/
  MainActivity.kt                     // 协调各 Manager 与 Service
  HistoryActivity.kt                  // 历史列表（独立 Activity）
  DeviceAdapter.kt                    // 设备列表适配器
  TransferAdapter.kt                  // 传输进度适配器（取消 / 打开 / 速率 / 图标 / 滑动删除）
```

## 使用

1. 在 Android Studio 中打开本目录并安装到 **两台** 真机。
2. 在两台手机上分别授予权限（启动时自动申请）。
3. 在接收方：
   - 点「Discover Wi-Fi Direct」—— TCP 文件服务会自动启动。
   - 点「Listen (Bluetooth)」启动蓝牙接收（若蓝牙未开启会自动提示）。
4. 在发送方：
   - 点「Pick File to Send」选择文件。
   - 点「Discover Wi-Fi Direct」/「Discover Bluetooth」搜索设备。
   - 在「Discovered Devices」列表点击目标设备，传输立即开始。
5. 收到的文件可在「Transfers」列表点击打开；或进系统的「文件 / 下载」管理器查找
   `Downloads/WiFiDirectFileTransfer/`。

## 构建

```bash
# 调试 APK
./gradlew assembleDebug

# 运行单测
./gradlew testDebugUnitTest

# Release APK（需要配置签名；缺失时回退到 debug 签名以便 CI 通过）
./gradlew assembleRelease \
  -PwfdReleaseStoreFile=... \
  -PwfdReleaseStorePassword=... \
  -PwfdReleaseKeyAlias=... \
  -PwfdReleaseKeyPassword=...
```

## 测试

34 个单元测试覆盖：

- `WireProtocolTest` — 帧编解码 round-trip、错误输入拒绝
- `TransferFormatterTest` — 字节大小格式化、百分比钳制、MIME 猜测
- `TransferAdapterStateTest` — 事件折叠状态机
- `StreamExtTest` — 流式拷贝、EOF 检测、协作式取消
- `SanitizeFilenameTest` — 文件名清洗

## 与旧版本相比的关键修复

详见 git 历史。重点：

- **二进制协议** 取代了文件名带空格会解析崩溃的文本协议
- **蓝牙服务端持续监听** 不再每接收一次就停
- **组主侧反向发送** 通过客户端上报 IP 实现
- **接收文件落到公共 Downloads** 通过 MediaStore 在系统文件管理器立即可见
- **前台通知随进度更新** 替代旧版静态通知
- **Uri 持久化权限** 解决旋转屏 / 退出后 Service 端读不了文件
- **取消传输** UI 按钮 + 协作式取消句柄
- **连接超时清理** 旧版 `WifiP2pManager.connect` 后无超时永久挂起
- **统一 UUID** 删除死代码 `App.getBluetoothUuid()`，避免与硬编码 UUID 冲突
- **release 签名 + R8 + 资源压缩** 旧版 release 包等于未签名且未优化

### 第二轮迭代新增

- **多文件批量发送**（GetMultipleContents，并行 / 串行自适应）
- **传输历史持久化**（JSON 文件 + HistoryActivity 入口）
- **传输速率与 ETA**（UI 端实时计算瞬时速率）
- **下拉刷新**（SwipeRefreshLayout 同时触发两种传输的发现）
- **滑动删除已完成项**（ItemTouchHelper）
- **文件类型图标**（按 mime 显示 image / video / audio / pdf / zip / generic）
- **lint 0 errors**（修复 MissingPermission、NewApi、DefaultLocale、InlinedApi、CoarseFineLocation、DataExtractionRules、MonochromeLauncherIcon 等 7 errors → 0）

### 第三轮迭代新增（原生桌面客户端）

- **跨平台 C++17 实现**：Linux（GCC + BlueZ + ncurses）+ Windows（MinGW-w64 + Win32）
- **端到端 SHA-256 完整性校验**：原生两端默认启用，Android 端透明忽略
- **LAN 设备发现**：UDP 广播 beacon（端口 18998），无需手工输入 IP
- **ncurses 交互式 TUI**（Linux）：实时刷新 peers / send queue / recv log
- **MinGW-w64 交叉编译**：在 Linux 主机上直接产出可在 Windows 上运行的 `.exe`
- **CPack 打包**：单命令产出 `.deb`（Linux）或 `.zip`（Windows）

### 第四轮迭代新增（iOS / iPadOS 客户端）

- **纯 Swift + SwiftUI** universal app（iPhone + iPad 同一份代码）
- **Network.framework TCP**：`NWListener` + `NWConnection`，与三端 wire 兼容
- **Bonjour 设备发现**：`_wfd._tcp.` 服务类型，iOS 14+ 自动申请本地网络权限
- **SwiftPM 工程**：协议库可在 Linux 上构建 + 单测，无需 Xcode
- **XcodeGen 配置**：在 macOS 上 `xcodegen generate` 即可生成完整 `.xcodeproj`
- **GitHub Actions 三平台 matrix**：Linux GCC + Windows MinGW + macOS Xcode + Android Gradle

## 原生桌面客户端 (`native/`)

### 目录结构

```
native/
├── CMakeLists.txt              # Linux + Windows 双平台构建
├── cmake/
│   └── mingw-x86_64.cmake      # MinGW-w64 交叉编译工具链
├── src/
│   ├── Protocol.{h,cpp}        # FDFT v2 二进制协议（KIND=1 FILE / 2 REGISTER_IP / 3 HASH）
│   ├── Hash.{h,cpp}            # 自包含 SHA-256（FIPS 180-4，零依赖）
│   ├── Platform.{h,cpp}        # 跨平台抽象：socket / fs / 终端 / 时间
│   ├── Util.{h,cpp}            # 字节格式化、文件名清洗、本地 IP 获取
│   ├── TcpTransport.{h,cpp}    # TCP 发送 + 多线程监听
│   ├── BluetoothTransport.{h,cpp}  # Win32 / BlueZ 双后端 RFCOMM
│   ├── Discovery.{h,cpp}       # UDP 广播 beacon + 监听
│   ├── FileSink.{h,cpp}        # 接收文件落盘（共享于两个 transport）
│   ├── Progress.{h,cpp}        # TTY 进度条（自动检测）
│   ├── main.cpp                # CLI 入口（send / recv / discover / beacon / hash）
│   └── tui.cpp                 # ncurses 交互式 TUI（Linux）
└── tests/                      # 6 个测试：protocol / util / hash / filesink / integration / discovery
```

### 在 Linux 上构建

```bash
# 依赖（Ubuntu / Debian）
sudo apt install build-essential cmake libbluetooth-dev libncursesw5-dev mingw-w64

# 配置 + 构建
cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native -j

# 跑测试
ctest --test-dir build/native --output-on-failure
```

产物：

| 文件 | 说明 |
|------|------|
| `build/native/wfd_transfer`   | CLI 客户端（必装） |
| `build/native/wfd_tui`        | ncurses 交互式 TUI |
| `build/native/libwfd_core.a`  | 静态库，可被外部工程复用 |
| `build/native/test_*`         | 6 个测试二进制 |

### 在 Windows 上构建（推荐：交叉编译）

```bash
# 一次性安装 MinGW-w64 工具链
sudo apt install mingw-w64 g++-mingw-w64-x86_64-posix

# 配置 + 构建
cmake -S native -B build/win64 \
      -DCMAKE_TOOLCHAIN_FILE=native/cmake/mingw-x86_64.cmake
cmake --build build/win64 -j --target wfd_transfer
```

产物 `build/win64/wfd_transfer.exe` 是 **完全静态链接** 的（2.7 MB），不依赖任何
外部 DLL，可以直接拷到任意 Windows 机器运行（Win7+）。蓝牙支持通过 `bthprops.cpl`
+ Win32 `SOCKADDR_BTH` 实现。

### 在 Windows 上原生构建（MSVC）

需要 Visual Studio 2019+ 与 CMake：

```powershell
cmake -S native -B build\msvc -G "Visual Studio 17 2022" -A x64
cmake --build build\msvc --config Release -j
```

## iOS / iPadOS 客户端 (`ios/`)

iOS 端通过 Network.framework 跑 TCP（与桌面 / Android 同一 FDFT v2 协议），
通过 Bonjour 发布 / 发现 `_wfd._tcp.` 服务。一份代码同时支持 iPhone 与 iPad。

### 在 Linux 上构建 + 测试（协议库部分）

不需要 Xcode —— SwiftPM 协议库可以独立构建：

```bash
cd ios
swift build           # 构建 FDFTProtocol + wfd-cli
swift test            # 跑所有单元测试（Protocol / Hash / FileSink）
swift run wfd-cli hash /some/file       # 算 SHA-256
```

需要 Swift 5.9+（GitHub Actions 的 `swift-actions/setup-swift@v2` 自动安装）。

### 在 macOS 上构建 iOS App

需要 Xcode 15+ 和 [XcodeGen](https://github.com/yonaskolb/XcodeGen)：

```bash
brew install xcodegen
cd ios && xcodegen generate
open WFDTransfer.xcodeproj
# 在 Xcode 中 ⌘R 即可安装到模拟器或真机
```

`project.yml` 配置成 universal app（`TARGETED_DEVICE_FAMILY: "1,2"`），
iPhone 与 iPad 自动切换布局（NavigationSplitView / TabView）。

### CLI 用法

```bash
# 接收文件到 ~/Downloads/wfd_received/（默认端口 8988）
./wfd_transfer recv

# 发送文件（自动附加 SHA-256 trailer）
./wfd_transfer send --to 192.168.1.42 file1.zip file2.png

# 与 Android 端互通 —— 不带 trailer
./wfd_transfer send --to 192.168.1.42:8988 --no-hash photo.jpg

# 在 LAN 上发现其他 wfd_transfer 实例
./wfd_transfer discover --seconds 5

# 在后台持续广播自己
./wfd_transfer beacon &

# 计算本地文件 SHA-256（与 `sha256sum` 输出格式一致）
./wfd_transfer hash photo.jpg
```

### Android ↔ Desktop 互操作

桌面端能与运行 FDFT v2 协议的 Android 设备无缝互传：

```text
桌面接收 ← Android 发送：
  1. 在 Android 上 Pick File → 点 Discovered Devices 中的桌面（手动输入 IP）
  2. 桌面跑 `./wfd_transfer recv --port 8988`
  3. 文件落到 `~/Downloads/wfd_received/`

桌面发送 → Android 接收：
  1. 在 Android 上 Discover Wi-Fi Direct（自动启动 FileTransferService 的 TCP 服务）
  2. 桌面跑 `./wfd_transfer send --to <android-ip>:8988 --no-hash photo.jpg`
     （必须用 --no-hash，因为 Android 端只读 size 字节不读 trailer）
```

### 协议扩展（KIND=3 HASH trailer）

原生端在文件正文之后追加一个可选的 HASH 帧做端到端校验：

```
+---------+---------+---------+---------+----------------+
| "FDFT"  | VERSION | KIND=3  | algo    | digestLen      |
| 4 bytes | 1 byte  | 1 byte  | 1 byte  | 1 byte         |
+---------+---------+---------+---------+----------------+
| digest (algo=1 时为 32 字节 SHA-256)                   |
+--------------------------------------------------------+
```

- 发送方在流式写文件正文的同时计算 SHA-256，正文结束追加 trailer
- 接收方在读完 `size` 字节后用 `MSG_PEEK` 探测 6 字节，若是 HASH 帧则校验
- Android 端只读 `size` 字节，trailer 自动被忽略 → 完全向后兼容
