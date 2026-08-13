# ZeroBorders

ZeroBorders 是一款运行于 Windows 平台的局域网软件 KVM（Keyboard-Video-Mouse）工具。它让两台电脑像使用双显示器一样共享一套鼠标键盘——将光标从一台屏幕的边缘移出，即可无缝切换到另一台电脑继续操作，同时支持双向剪贴板和文件传输。

## 功能特性

### 无缝鼠标键盘穿越
- 光标移动到屏幕边缘时自动将控制权切换到对端电脑，反向移动可返回。
- 支持四种相对布局：对端屏幕在本机的**左 / 右 / 上 / 下**。
- 任一端修改布局后，通过 `LayoutSync` 消息双向同步，两端界面实时更新。
- 跨屏拖拽时自动释放已按下的鼠标按键，避免按键"卡住"。
- 子像素级坐标累加，避免微小移动因整数截断而丢失。

### 剪贴板共享
- **文字**：`CF_UNICODETEXT` 双向同步。
- **图片**：`CF_DIB` / `CF_DIBV5` 位图传输。
- **文件与文件夹**：`CF_HDROP` 格式识别，自动递归打包目录并通过文件传输通道发送，接收端写入磁盘后自动回填到本地剪贴板，可直接 Ctrl+V。
- 本地写入产生的剪贴板变化通过摘要（SHA-256）去重，防止回环。

### 文件传输
- 支持文件和整个文件夹递归传输，保留相对目录结构。
- 64 KB 分块传输，带 ACK 确认与实时进度回调。
- 重名文件自动追加序号（`file (1).txt`、`file (2).txt`...）。
- 接收目录默认为系统临时目录，可在设置中自定义。
- Windows 下全程使用 UTF-8 / UTF-16 宽字符路径转换，正确处理中文等非 ASCII 路径。

### 网络与发现
- **UDP 广播自动发现**（默认端口 24800）：同一局域网内自动找到对端。
- **自动角色选举**：两端均可选择"自动 / 优先控制端 / 优先被控端"；自动模式下比较随机节点 ID，较大者作为控制端（Server）。
- 也支持手动指定对端 IP 直连。
- 配对码认证：配对码在本地做 SHA-256 哈希后随握手发送，不在网络上传输明文。
- 控制通道与数据通道分离：
  - TCP 24801（控制通道）：握手、输入事件、剪贴板通知、布局同步等小消息。
  - TCP 24802（数据通道）：文件分块、剪贴板图片等大载荷。
- 断线自动重连（指数退避）。

### 其他
- 配置持久化到 `%APPDATA%\ZeroBorders\config.json`。
- 支持开机自启（写入 `HKCU\...\Run` 注册表）。
- Qt6 Fusion 风格界面，内置日志面板。
- **崩溃捕获**：捕获访问违例、纯虚调用、CRT 错误、未处理 C++ 异常，自动写入调用栈到日志文件。
- **日志系统**：按日期轮转写入 `<程序目录>\log\zb_YYYYMMDD.log`。
- **单实例锁（仅 Release）**：发行版同一台电脑只允许运行一个实例，第二次启动会激活已有窗口。

## 使用方式

### 基本使用

1. **两台电脑**分别运行 ZeroBorders。
2. 在两台电脑的"识别码"输入框中输入**相同的配对码**（例如 `mycode123`）。
3. 选择"控制方式"：
   - **自动选择**（推荐）：两端自动选举，较大的节点 ID 作为控制端。
   - **本机控制对端**：强制本机作为控制端。
   - **对端控制本机**：强制本机作为被控端。
4. 点击"开始连接"，程序会自动通过 UDP 发现对端并建立连接。
5. 连接成功后，将鼠标移到屏幕边缘即可穿越到对端屏幕。

### 屏幕布局

在"设备位置"区域拖拽对端矩形，设置相对位置（左/右/上/下）。修改后**两端自动同步**，无需重连。

### 剪贴板与文件传输

- **文字 / 图片**：在一台电脑 Ctrl+C，另一台 Ctrl+V 即可，自动同步。
- **文件 / 文件夹**：在一台电脑复制文件（Ctrl+C），另一台 Ctrl+V 自动接收并保存到接收目录。
- 接收目录在"文件传输"区域的"本地路径"中设置，支持下拉树形浏览选择。

### 路径选择（Xftp 风格下拉）

- 点击"本地路径"右侧下拉箭头，弹出文件系统目录树。
- **单击**目录：更新路径，下拉保持打开，可继续展开浏览子目录。
- **双击**目录：确认选择并关闭下拉。
- 也可直接在输入框中手动输入路径，按回车确认。

### 日志查看

日志文件位于程序所在目录的 `log` 文件夹下，按日期命名：

```
ZeroBorders/
├── ZeroBorders.exe
└── log/
    └── zb_20260813.log    # 当天日志
```

- 包含所有运行信息（连接状态、文件传输、剪贴板、错误等）。
- 若程序异常退出，日志末尾会有 `!!! APPLICATION CRASH !!!` 段落，包含异常代码和调用栈，便于排查。

### 单实例说明

| 模式 | 单实例锁 | 行为 |
|------|----------|------|
| **Debug（开发版）** | 禁用 | 允许同时启动多个实例，方便开发调试双机通信 |
| **Release（发行版）** | 启用 | 只允许运行一个实例，第二次启动会激活已有窗口后退出 |

单实例锁通过 Windows 命名互斥量 `ZeroBorders_SingleInstance_Mutex` 实现，仅 `NDEBUG` 宏定义时（Release 编译）启用。

### 断线重连

- 任一端断开后，另一端**自动重新进入 UDP 发现模式**，重新搜索对端。
- 无需手动操作，两端重新发现后会自动重新选举角色并连接。

## 编译方式

### 环境要求

| 依赖 | 版本 / 说明 |
|------|-------------|
| 操作系统 | Windows 10 及以上（`_WIN32_WINNT=0x0A00`） |
| 编译器 | MSVC 2022（支持 C++20） |
| CMake | ≥ 3.20 |
| Qt | 6.x（Widgets + Network 模块），当前工程默认路径为 `C:/Qt/Qt6.11.1/6.11.1/msvc2022_64` |
| nlohmann/json | 3.11.3（CMake FetchContent 自动拉取，无需手动安装） |

如果 Qt 安装在其他路径，修改根目录 [CMakeLists.txt](CMakeLists.txt) 中的 `CMAKE_PREFIX_PATH`：

```cmake
set(CMAKE_PREFIX_PATH "你的Qt路径/msvc2022_64" CACHE PATH "Qt6 prefix" FORCE)
```

### 编译步骤

```powershell
# 在项目根目录执行
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

编译产物位于 `build/bin/Debug/ZeroBorders.exe`。构建后会自动运行 `windeployqt` 复制 Qt 运行时 DLL，因此可直接双击运行。

如需 Release 版本：

```powershell
cmake --build build --config Release
```

### 发行打包

项目提供一键打包脚本 [scripts/build_release.ps1](scripts/build_release.ps1)，自动完成 Release 编译、Qt 运行时部署、文件收集和 ZIP 打包：

```powershell
# 使用默认 Qt 路径打包
.\scripts\build_release.ps1

# 指定 Qt 路径和版本号
.\scripts\build_release.ps1 -QtPath "C:\Qt\6.11.1\msvc2022_64" -Version "1.0.0"
```

脚本流程：
1. 清理旧构建，CMake Release 配置
2. MSVC 编译（`--config Release --parallel`）
3. `windeployqt --release` 自动部署 Qt 运行时 DLL
4. 收集 exe + DLL + 创建 `log` 目录到 `dist/ZeroBorders/`
5. 打包为 `dist/ZeroBorders_YYYYMMDD.zip`

参数说明：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-QtPath` | `C:\Qt\Qt6.11.1\6.11.1\msvc2022_64` | Qt 安装路径 |
| `-BuildDir` | `build_release` | 构建中间目录 |
| `-DistDir` | `dist` | 输出目录 |
| `-Version` | 当天日期（如 `20260813`） | ZIP 文件名中的版本号 |

打包结果输出到 `dist/` 目录，包含可直接运行的文件夹和 ZIP 压缩包。

### 链接的系统库

- `ws2_32`（Winsock 网络）
- `bcrypt`（SHA-256 哈希）
- `iphlpapi`（获取本机 IP / 网络接口信息）
- `user32`、`gdi32`（输入捕获、剪贴板、位图操作）
- `dbghelp`（崩溃时捕获调用栈）

## 软件架构

### 目录结构

```
ZeroBorders/
├── CMakeLists.txt              # 顶层 CMake（依赖拉取、Qt 配置）
├── resources/                  # 图标、Windows 资源文件
└── src/
    ├── main.cpp                # 程序入口：Winsock 初始化 + Qt 事件循环
    ├── App.h / App.cpp         # 核心编排器，持有所有模块并转发信号
    │
    ├── core/                   # 基础类型与协议
    │   ├── Types.h             # 常量（端口、分块大小、Magic、能力标志）
    │   ├── Protocol.h/.cpp     # 消息类型枚举、载荷结构、序列化/反序列化
    │   ├── Event.h             # InputEvent 输入事件结构
    │   ├── ScreenLayout.h      # 布局枚举（LeftOf/RightOf/Above/Below）与方向计算
    │   ├── Hash.h/.cpp         # SHA-256（基于 BCrypt）
    │   └── Log.h               # 轻量级日志（支持多 Sink，编译期格式化）
    │
    ├── network/                # 网络层
    │   ├── UdpDiscovery.h/.cpp # UDP 广播发现 + 自动角色选举
    │   ├── TcpTransport.h/.cpp # 帧封装（Magic + Version + Type + Length + Payload）
    │   ├── NetworkServer.h/.cpp# 控制端：监听两个 TCP 端口，管理握手与消息分发
    │   └── NetworkClient.h/.cpp# 被控端：发现/连接、断线重连
    │
    ├── input/                  # 输入捕获与注入
    │   ├── IInputCapturer.h    # 捕获接口（Raw Input）
    │   ├── IInputInjector.h    # 注入接口（SendInput）
    │   ├── WinInputCapturer.*  # Windows Raw Input 实现，支持抑制/光标warp/按键释放
    │   └── WinInputInjector.*  # Windows SendInput 实现，坐标映射
    │
    ├── router/                 # 屏幕路由（控制端独有逻辑）
    │   ├── ScreenRouter.h/.cpp # 边缘检测、控制权切换、坐标映射
    │   └── InputEventSender.h  # 将 InputEvent 通过网络回调发送
    │
    ├── clipboard/              # 剪贴板
    │   └── ClipboardManager.*  # WM_CLIPBOARDUPDATE 监听、CF_HDROP/TEXT/DIB 读写、防抖去重
    │
    ├── transfer/               # 文件传输
    │   └── FileTransferManager.* # offer/accept/chunk/ack 状态机、目录递归、UTF-8路径处理
    │
    ├── config/                 # 配置
    │   ├── AppConfig.h         # 配置数据结构
    │   └── ConfigManager.*     # JSON 读写（%APPDATA%）、开机自启注册表
    │
    └── gui/                    # Qt6 界面
        ├── MainWindow.*        # 主窗口：连接设置、日志面板、系统托盘
        └── DeviceLayoutWidget.*# 可视化拖拽设备相对位置的画布
```

### 模块关系

```
┌─────────────────────────────────────────────────────┐
│                      MainWindow                      │  (Qt6 Widgets)
└──────────────────────────┬──────────────────────────┘
                           │ signals/slots
┌──────────────────────────▼──────────────────────────┐
│                         App                          │  编排器
│  持有: UdpDiscovery / NetworkServer|Client          │
│        WinInputCapturer / WinInputInjector          │
│        ScreenRouter / ClipboardManager              │
│        FileTransferManager / ConfigManager          │
└──────┬──────────┬──────────┬──────────┬─────────────┘
       │          │          │          │
  ┌────▼───┐ ┌────▼────┐ ┌───▼────┐ ┌──▼──────────┐
  │network │ │  input  │ │router  │ │ clipboard/   │
  │UDP/TCP │ │ Raw/Send│ │ edge   │ │ file transfer│
  └────────┘ └─────────┘ └────────┘ └──────────────┘
```

### 通信协议

所有消息通过统一的二进制帧传输：

```
+--------+--------+--------+--------+----------------+
| 0x5A   | 0x42   | ver(1) | type(1)| length(4, BE)  |
+--------+--------+--------+--------+----------------+
|                  payload (length bytes)             |
+----------------------------------------------------+
```

- Magic：`Z` `B`（0x5A 0x42）
- Version：当前为 1
- Type：消息类型（见 [Protocol.h](src/core/Protocol.h) 中的 `MsgType` 枚举）
- Length：大端序 32 位载荷长度

消息类型按通道划分：

| 范围 | 通道 | 典型消息 |
|------|------|----------|
| 0x01–0x0F | 控制通道 | Hello / Welcome / InputEvent / CursorEnter / CursorLeave / LayoutSync / Ping / Goodbye |
| 0x10–0x1F | 控制通道 | ClipboardText / ClipboardNotify / FileOffer / FileAccept / TransferProgress / TransferComplete |
| 0x20–0x2F | 数据通道 | DataHello / ClipboardImage / ClipboardData / FileChunk / FileChunkAck |

### 控制权切换流程

1. 控制端（Server）通过 Raw Input 捕获本机鼠标移动。
2. `ScreenRouter` 检测到光标到达布局定义的交叉边缘（例如布局为 `RightOf` 时，光标碰到屏幕右边缘）。
3. Server 发送 `CursorEnter` 消息给被控端（Client），包含入口坐标。
4. Client 接收后通过 `WinInputInjector` 把光标 warp 到对应位置，并开始注入后续输入事件。
5. Client 端检测到光标碰到返回边缘时，发送 `CursorLeave`，Server 恢复本地控制并把光标 warp 回本机边缘。

### 剪贴板同步流程

1. `ClipboardManager` 通过 `AddClipboardFormatListener` 监听 `WM_CLIPBOARDUPDATE`。
2. 使用 `GetClipboardSequenceNumber` 去重，配合防抖延迟处理 Explorer 的延迟渲染。
3. 按优先级读取格式：`CF_HDROP`（文件）> `CF_DIBV5`/`CF_DIB`（图片）> `CF_UNICODETEXT`（文字）。
4. 文件路径通过 `FileTransferManager` 走 offer → accept → chunk → complete 流程；完成后接收端调用 `setFiles` 写入 `CF_HDROP`。
5. 写入本地剪贴板前记录 SHA-256 摘要，下次监听到相同摘要时判定为"自己写入的回显"并跳过。

### 关键设计决策

- **控制/数据双通道分离**：输入事件等低延迟消息走控制通道，文件分块等大流量走数据通道，避免大文件传输阻塞鼠标键盘事件。
- **UTF-8 网络表示 + UTF-16 本地路径**：所有路径在协议层用 UTF-8 窄字符串，进入 Windows 文件 API 前通过 `MultiByteToWideChar`/`WideCharToMultiByte` 转换，确保中文路径正确。
- **回调 → Qt 信号桥接**：网络与输入线程通过 `std::function` 回调进入 `App`，`App` 再通过 Qt 的 `queued connection` 发射信号到 GUI 线程，保证线程安全。
- **回环防护**：剪贴板用摘要去重，布局同步用 `applyingRemoteLayout_` 原子标志防止收到对端布局后再次广播。

## 配置文件

配置文件位于 `%APPDATA%\ZeroBorders\config.json`，主要字段：

```json
{
    "role": 0,
    "rolePreference": 0,
    "pairingCode": "test123",
    "serverName": "",
    "host": "",
    "layout": "left_of",
    "controlPort": 24801,
    "dataPort": 24802,
    "udpPort": 24800,
    "receiveDir": "",
    "startMinimized": false
}
```

| 字段 | 说明 |
|------|------|
| `role` | 0=Server（控制端），1=Client（被控端） |
| `rolePreference` | 自动模式下的偏好：0=自动，1=优先控制端，2=优先被控端 |
| `pairingCode` | 配对码，两端需一致 |
| `layout` | 相对布局：`left_of` / `right_of` / `above` / `below` |
| `receiveDir` | 接收文件目录，留空则使用系统临时目录 |

## 端口说明

| 端口 | 协议 | 用途 |
|------|------|------|
| 24800 | UDP | 局域网设备发现广播 |
| 24801 | TCP | 控制通道（握手、输入、剪贴板通知、布局同步） |
| 24802 | TCP | 数据通道（文件分块、剪贴板图片） |

如 Windows 防火墙弹出提示，请允许 ZeroBorders 在专用网络通信。
