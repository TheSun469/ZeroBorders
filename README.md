# ZeroBorders

ZeroBorders 是一款运行于 Windows 平台的局域网软件 KVM（Keyboard-Video-Mouse）工具。它让两台电脑像使用双显示器一样共享一套鼠标键盘——将光标从一台屏幕的边缘移出，即可无缝切换到另一台电脑继续操作，同时支持双向剪贴板和文件传输。

## 功能特性

### 无缝鼠标键盘穿越
- 光标移动到屏幕边缘时自动将控制权切换到对端电脑，反向移动可返回。
- 支持四种相对布局：对端屏幕在本机的**左 / 右 / 上 / 下**。
- 任一端修改布局后，通过 `LayoutSync` 消息双向同步，两端界面实时更新。
- 跨屏拖拽时自动释放已按下的鼠标按键，避免按键"卡住"。
- 子像素级坐标累加，避免微小移动因整数截断而丢失。
- 鼠标滚轮通过 `PostMessage(WM_MOUSEWHEEL/WM_MOUSEHWHEEL)` 直接投送到光标下窗口，解决远程滚轮失效问题。
- **鼠标/键盘控制权分离**：鼠标和点击始终跟随光标所在屏幕（鼠标跨屏即可操作对端），但键盘控制权独立管理——只有在**对端屏幕左键点击**后才切换到对端，避免鼠标滑过边缘时键盘意外跟随。
- **跨屏切换时释放修饰键**：进入或退出远程控制时，向对端发送所有修饰键（Ctrl/Shift/Alt/Win）的 KeyUp 事件，防止状态残留导致快捷键异常（如 Ctrl 卡住引发滚轮缩放网页、数字键变成快捷键）。
- **被控端光标自动隐藏**：鼠标返回控制端后，被控端通过 `CursorLeave` 通知隐藏本地光标，避免被控端屏幕残留静止光标；再次进入时通过 `CursorEnter` 恢复显示。

### 剪贴板共享
- **文字**：`CF_UNICODETEXT` 双向同步。
- **图片**：`CF_DIB` / `CF_DIBV5` 位图传输，自动兼容回退。
- **文件与文件夹**：`CF_HDROP` 格式识别，自动递归打包目录并通过文件传输通道发送，接收端写入磁盘后自动回填到本地剪贴板，可直接 Ctrl+V。
- 本地写入产生的剪贴板变化通过摘要（SHA-256）去重，防止回环。
- `OpenClipboard` 内置 5 次重试（60ms 间隔），应对临时锁定。
- 连接建立后立即将本地剪贴板内容同步给对端。

### 文件传输
- 支持文件和整个文件夹递归传输，保留相对目录结构。
- **双向传输**：本地→远程（上传）与远程→本地（下载），通过文件浏览器右键菜单触发。
- 64 KB 分块传输，带 ACK 确认与实时进度条，进度条在触发侧显示。
- 重名文件自动追加序号（`file (1).txt`、`file (2).txt`...）。
- 接收目录可在文件传输区域自定义，连接时通过 `PathSync` 消息双向同步。
- Windows 下全程使用 UTF-8 / UTF-16 宽字符路径转换，正确处理中文等非 ASCII 路径。
- 远程根目录自动枚举所有逻辑驱动器（C:\、D:\ 等），支持逐级浏览。

### 网络与发现
- **UDP 广播自动发现**（默认端口 24800）：同一局域网内自动找到对端，同时发送有限广播和子网定向广播。
- **自动角色选举**：两端均可选择"自动 / 优先控制端 / 优先被控端"；自动模式下比较 64 位随机节点 ID，较大者作为控制端（Server）。
- 发现对端后继续广播 3 秒宽限期，确保双向互检。
- 也支持手动指定对端 IP 直连。
- **双重认证**：配对码 + 用户名组合生成 SHA-256 令牌，防止局域网内相同识别码冲突。
- 控制通道与数据通道分离：
  - TCP 24801（控制通道）：握手、输入事件、剪贴板通知、布局同步、目录列表等小消息。
  - TCP 24802（数据通道）：文件分块、剪贴板图片等大载荷。
- TCP 连接采用渐进延迟（500ms → 1000ms → 1500ms）与递增超时。
- 断线自动重连（指数退避 1s → 2s → 4s … 上限 30s），并自动重启 UDP 发现。
- 一端断开后，另一端静默等待重连，无需手动操作。

### 界面
- **Word 2019 浅色风格**：白色卡片 + 浅灰背景（#f3f2f1）、Office 蓝（#0078d4）主色调、Segoe UI / 微软雅黑字体。
- **文件传输双面板**：左侧本地、右侧远程，中间使用可拖拽分割器（QSplitter）自由调整宽度。
- **Xftp 风格路径选择器**：本地和远程均支持点击下拉箭头弹出目录树，单击浏览、双击确认。
  - 本地：`PathComboBox` + `QFileSystemModel` 直接浏览本地文件系统。
  - 远程：`RemotePathComboBox` + `RemoteDirTreeModel` 懒加载远程目录树，展开时才向对端请求子目录。
- **文件表格**：名称 / 大小 / 类型 / 修改日期四列，表头中文化、列宽可调、列顺序可拖拽换位。
- **右键菜单**：选中文件后右键可"上传/下载"或"复制文件地址"。
- **设备位置**：可视化拖拽设置相对布局，支持收起/展开（位置变更时自动展开）。
- **QSplitter 布局**：设置 / 设备位置 / 文件传输 / 日志各区域高度可拖拽调整。
- Qt6 Fusion 基础风格 + 全局 QSS 样式表覆盖。

### 其他
- 配置持久化到 `%APPDATA%\ZeroBorders\config.json`。
- 支持开机自启（写入 `HKCU\...\Run` 注册表）。
- 内置日志面板与系统托盘图标。
- **崩溃捕获**：捕获访问违例、纯虚调用、CRT 错误、未处理 C++ 异常，通过 StackWalk64 自动写入调用栈到日志文件。
- **日志系统**：按日期轮转写入 `<程序目录>\log\zb_YYYYMMDD.log`。
- **单实例锁（仅 Release）**：发行版同一台电脑只允许运行一个实例，第二次启动会激活已有窗口；Debug 版允许多实例以便调试。

## 使用方式

### 基本使用

1. **两台电脑**分别运行 ZeroBorders。
2. 在两台电脑的"识别码"输入框中输入**相同的配对码**（例如 `mycode123`）。
3. 在"用户名"输入框中输入**相同的用户名**（双重保险，防止局域网内识别码冲突）。
4. 选择"控制方式"：
   - **自动选择**（推荐）：两端自动选举，较大的节点 ID 作为控制端。
   - **本机控制对端**：强制本机作为控制端。
   - **对端控制本机**：强制本机作为被控端。
5. 点击"开始连接"，程序会自动通过 UDP 发现对端并建立连接。
6. 连接成功后，将鼠标移到屏幕边缘即可穿越到对端屏幕。

### 屏幕布局

在"设备位置"区域拖拽对端矩形，设置相对位置（左/右/上/下）。修改后**两端自动同步**，无需重连。点击标题栏的 ▼/▶ 按钮可收起/展开该区域，位置变更时会自动展开。

### 文件传输

文件传输区域分为左右两个浏览器面板：

- **左侧（本地）**：浏览本地文件系统，可选择发送目标。
- **右侧（远程）**：浏览对端文件系统，连接后自动加载驱动器列表。

**上传文件（本地 → 远程）：**
1. 在左侧本地浏览器中选中文件或文件夹。
2. **右键** → 选择"上传"，或直接双击文件。
3. 文件发送到远程的接收目录，进度条在左侧显示。

**下载文件（远程 → 本地）：**
1. 在右侧远程浏览器中选中文件或文件夹。
2. **右键** → 选择"下载"，或直接双击文件。
3. 文件保存到左侧本地浏览器当前所在目录，进度条在右侧显示。

**复制文件地址：**
- 右键菜单中的"复制文件地址"可将选中文件的完整路径复制到剪贴板（多选时换行分隔）。

**路径选择（Xftp 风格下拉）：**
- 点击路径框右侧下拉箭头，弹出目录树。
- **单击**目录：更新路径并进入该目录，下拉保持打开可继续展开。
- **双击**目录：确认选择并关闭下拉。
- 导航按钮：← 后退，↑ 上一级。
- 本地路径框可手动输入路径按回车确认；远程路径框为只读。

**调整面板宽度：**
- 拖拽左右浏览器中间的分割条即可调整本地/远程面板宽度。

### 剪贴板

- **文字 / 图片**：在一台电脑 Ctrl+C，另一台 Ctrl+V 即可，自动同步。
- **文件 / 文件夹**：在一台电脑复制文件（Ctrl+C），另一台 Ctrl+V 自动接收并保存到接收目录。
- 接收目录在文件传输区域的"本地路径"中设置，支持下拉树形浏览选择；连接建立和路径变更时自动同步给对端。

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
- 重连采用指数退避，避免频繁重试。

### 锁屏与跨屏控制

ZeroBorders 支持锁屏场景下的优雅处理，保证连接不中断、状态不丢失。

**Win+L 锁屏转发：**
- 跨屏控制时按 Win+L，会锁定**鼠标当前所在界面**的电脑，而不是两台都锁。
  - 鼠标在控制端（本地模式）→ Win+L 走系统默认行为，锁定控制端。
  - 鼠标在被控端（远程模式）→ Win+L 被控制端拦截并转发，被控端的输入注入器检测到 Win+L 组合后直接调用 `LockWorkStation()` 锁定本机。
  - 这是由于 `SendInput` 注入的 Win+L 无法可靠触发锁屏（Windows 限制模拟安全快捷键），因此在被控端注入器中直接拦截 Win+L 组合键并调用原生 API。

**锁屏后会话同步：**
- 程序通过 `WTSRegisterSessionNotification` 监听 Windows 会话锁屏/解锁事件。
- 任一端锁屏时：
  - 自动切回本地控制，避免光标卡在不可达桌面。
  - 通过 `SessionLock (0x0E)` 消息通知对端本机已锁屏。
  - 对端收到通知后暂停跨屏操作，等待解锁。
- 任一端解锁后：
  - 自动重新同步屏幕布局、剪贴板内容、接收目录路径和远程文件列表。
  - 通知对端本机已解锁，跨屏操作自动恢复。
- 全程 TCP 连接保持不断开，无需重新配对。

**已知限制（安全桌面）：**
- Windows 锁屏运行在**安全桌面（Winlogon desktop）**，与用户桌面隔离。
- 用户态的 `SendInput` API **无法向安全桌面注入输入**，这是 Windows 的安全机制。
- 因此，**被锁定的一端在锁屏期间无法被跨屏控制**（鼠标/键盘指令到不了锁屏界面）。
- 解锁后跨屏操作自动恢复，不影响使用。

> **关于突破安全桌面限制的说明：**
> 像 ToDesk、向日葵等远程控制软件能在锁屏后继续运行，依赖的是**内核驱动 + 系统服务**架构：内核驱动直接读写键盘/鼠标类驱动的输入队列（绕过桌面隔离），并需 WHQL 签名才能在普通用户电脑加载。ZeroBorders 目前为纯用户态实现，此项能力作为**后续开发方向**预留。详见 [待开发：内核驱动方案](#待开发内核驱动方案)。

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
3. `windeployqt --release --no-svg` 自动部署 Qt 运行时 DLL（不含 SVG 模块）
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
├── scripts/                    # 打包脚本
└── src/
    ├── main.cpp                # 程序入口：Winsock 初始化 + Word 2019 样式 + Qt 事件循环
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
    │   ├── IInputInjector.h    # 注入接口（SendInput + 光标可见性）
    │   ├── WinInputCapturer.*  # Windows Raw Input 实现，支持抑制/光标warp/按键释放
    │   └── WinInputInjector.*  # Windows SendInput 实现，坐标映射、修饰键释放、光标隐藏/恢复
    │
    ├── router/                 # 屏幕路由（控制端独有逻辑）
    │   ├── ScreenRouter.h/.cpp # 边缘检测、控制权切换（鼠标/键盘分离）、坐标映射、CursorLeave 通知
    │   └── InputEventSender.h  # 将 InputEvent 通过网络回调发送
    │
    ├── clipboard/              # 剪贴板
    │   └── ClipboardManager.*  # WM_CLIPBOARDUPDATE 监听、CF_HDROP/TEXT/DIB 读写、防抖去重
    │
    ├── transfer/               # 文件传输
    │   └── FileTransferManager.* # offer/accept/chunk/ack 状态机、目录递归、UTF-8路径处理
    │
    ├── config/                 # 配置
    │   ├── AppConfig.h         # 配置数据结构（含 username 双重认证字段）
    │   └── ConfigManager.*     # JSON 读写（%APPDATA%）、开机自启注册表
    │
    └── gui/                    # Qt6 界面
        ├── MainWindow.*        # 主窗口：连接设置、双面板文件传输、日志、系统托盘
        ├── DeviceLayoutWidget.*# 可视化拖拽设备相对位置的画布（可收起）
        ├── FileBrowserWidget.* # 文件浏览器面板（导航栏 + 表格 + 右键菜单 + 进度条）
        ├── PathComboBox.*      # 本地路径 Xftp 风格下拉（QFileSystemModel 目录树）
        ├── RemotePathComboBox.*# 远程路径 Xftp 风格下拉（懒加载远程目录树）
        ├── RemoteDirTreeModel.*# 远程目录懒加载树模型
        └── RemoteFileModel.*   # 远程文件表格模型（驱动器/文件夹/文件图标）
```

### 模块关系

```
┌─────────────────────────────────────────────────────┐
│                      MainWindow                      │  (Qt6 Widgets, Word 2019 浅色主题)
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

| 范围 | 通道 | 消息 |
|------|------|------|
| 0x01 | 控制 | Hello（握手，含配对码+用户名哈希令牌） |
| 0x02 | 控制 | Welcome（握手响应，含屏幕尺寸） |
| 0x03 | 控制 | InputEvent（键盘/鼠标输入事件） |
| 0x04 | 控制 | CursorEnter（光标进入对端） |
| 0x05 | 控制 | CursorLeave（光标离开对端） |
| 0x06/0x07 | 控制 | Ping / Pong（心跳） |
| 0x08 | 控制 | Goodbye（正常断开） |
| 0x09 | 控制 | LayoutSync（屏幕相对布局变更） |
| 0x0A | 控制 | PathSync（接收目录路径同步） |
| 0x0B | 控制 | ListDirRequest（请求列出对端目录） |
| 0x0C | 控制 | ListDirResponse（返回目录条目列表） |
| 0x0D | 控制 | FilePullRequest（请求对端发送指定文件/下载） |
| 0x0E | 控制 | SessionLock（会话锁屏/解锁通知，1=locked, 0=unlocked） |
| 0x10–0x11 | 控制 | ClipboardText / ClipboardNotify |
| 0x12–0x15 | 控制 | FileOffer / FileAccept / TransferProgress / TransferComplete |
| 0x20 | 数据 | DataHello（数据通道握手） |
| 0x21–0x22 | 数据 | ClipboardImage / ClipboardData |
| 0x23–0x24 | 数据 | FileChunk / FileChunkAck |

### 控制权切换流程

1. 控制端（Server）通过 Raw Input 捕获本机鼠标移动。
2. `ScreenRouter` 检测到光标到达布局定义的交叉边缘（例如布局为 `RightOf` 时，光标碰到屏幕右边缘）。
3. Server 发送 `CursorEnter` 消息给被控端（Client），包含入口坐标。
4. Client 接收后通过 `WinInputInjector` 把光标 warp 到对应位置，显示本地光标，并开始注入后续鼠标事件。
5. **键盘控制权延迟切换**：鼠标进入对端后 `remoteControl_=true`（鼠标和点击立即跟随），但 `keyboardControl_` 仍为 `false`，键盘事件暂不转发。只有当用户**左键点击**对端屏幕后才置 `keyboardControl_=true`，键盘才开始转发到对端。
6. Client 端检测到光标碰到返回边缘时，发送 `CursorLeave`，Server 恢复本地控制并把光标 warp 回本机边缘。
7. Server 收到返回边缘信号时，向 Client 发送 `CursorLeave`，Client 据此隐藏本地光标、置 `hasControl_=false` 停止接受输入注入，并释放所有修饰键。
8. 每次进入/退出远程控制都会重置 `keyboardControl_=false`，确保下次进入对端必须再次左键点击才能切换键盘。

**控制权标志分离设计：**

| 标志 | 作用 | 切换时机 |
|------|------|----------|
| `remoteControl_` | 鼠标和点击是否转发到对端 | 鼠标跨屏立即切换 |
| `keyboardControl_` | 键盘事件是否转发到对端 | 左键点击对端屏幕后才切换 |
| `hasControl_`（被控端） | 是否接受控制端的输入注入 | 收到 `CursorEnter` 置 true，收到 `CursorLeave` 置 false |

### 剪贴板同步流程

1. `ClipboardManager` 通过 `AddClipboardFormatListener` 监听 `WM_CLIPBOARDUPDATE`。
2. 使用 `GetClipboardSequenceNumber` 去重，配合防抖延迟处理 Explorer 的延迟渲染。
3. 按优先级读取格式：`CF_HDROP`（文件）> `CF_DIBV5`/`CF_DIB`（图片）> `CF_UNICODETEXT`（文字）。
4. 文件路径通过 `FileTransferManager` 走 offer → accept → chunk → complete 流程；完成后接收端调用 `setFiles` 写入 `CF_HDROP`。
5. 写入本地剪贴板前记录 SHA-256 摘要，下次监听到相同摘要时判定为"自己写入的回显"并跳过。

### 文件传输流程

**上传（本地 → 远程）：**
1. 本地浏览器右键"上传" → `App::sendFiles()` 发送 `FileOffer`。
2. 远程自动回复 `FileAccept`，本地通过数据通道发送 `FileChunk`。
3. 远程逐块写入磁盘，回 `FileChunkAck`，进度实时回传。
4. 完成后发送 `TransferComplete`。

**下载（远程 → 本地）：**
1. 远程浏览器右键"下载" → 发送 `FilePullRequest`（含请求 ID、目标目录、文件路径列表）。
2. 对端收到后，将这些文件作为发送方走 FileOffer 流程反向推送。
3. 本地接收后保存到当前浏览器所在目录。

**远程目录浏览：**
1. 用户展开远程路径树节点 → `RemoteDirTreeModel::fetchMore()` 发出 `fetchRequested`。
2. `MainWindow` 转发为 `ListDirRequest` 发送给对端。
3. 对端扫描本地目录（空路径时枚举所有逻辑驱动器），返回 `ListDirResponse`。
4. 响应同时填充下拉树缓存和主表格（仅当路径匹配当前浏览路径时更新主表）。

### 关键设计决策

- **控制/数据双通道分离**：输入事件等低延迟消息走控制通道，文件分块等大流量走数据通道，避免大文件传输阻塞鼠标键盘事件。
- **UTF-8 网络表示 + UTF-16 本地路径**：所有路径在协议层用 UTF-8 窄字符串，进入 Windows 文件 API 前通过 `MultiByteToWideChar`/`WideCharToMultiByte` 转换，确保中文路径正确。
- **回调 → Qt 信号桥接**：网络与输入线程通过 `std::function` 回调进入 `App`，`App` 再通过 Qt 的 `queued connection` 发射信号到 GUI 线程，保证线程安全。
- **回环防护**：剪贴板用摘要去重，布局同步用 `applyingRemoteLayout_` 原子标志防止收到对端布局后再次广播。
- **远程目录懒加载**：`RemoteDirTreeModel` 只在用户展开节点时才请求对端目录，避免一次性扫描整个文件系统；树中只显示文件夹，与本地 PathComboBox 行为一致。
- **线程安全**：UDP 发现线程不在自身执行流中调用 `stop()/join()`，而是设置 `running_=false` 自然退出；线程清理通过 `QMetaObject::invokeMethod` 在主线程执行 `join()`。
- **Word 2019 主题实现**：通过全局 QSS 样式表实现，下拉箭头在运行时用 `QPainter` 绘制为 PNG data URI 注入（项目未链接 Qt SVG 模块）。
- **鼠标/键盘控制权分离**：`ScreenRouter` 用 `remoteControl_` 和 `keyboardControl_` 两个标志分别管理鼠标和键盘转发。鼠标跨屏后 `remoteControl_` 立即为 true（鼠标移动和点击跟随光标），但 `keyboardControl_` 只在左键点击后才置 true，避免鼠标滑过边缘时键盘意外跟随。每次进入或退出远程控制都会重置 `keyboardControl_`，防止上次点击的残留状态导致键盘直接转发。
- **被控端光标可见性管理**：`WinInputInjector` 实现 `setCursorVisible`，通过 `SetSystemCursor` 将所有系统光标（箭头、I 型、链接手型、调整大小等）替换为 1x1 透明光标。被控端收到 `CursorEnter` 显示光标、收到 `CursorLeave` 隐藏光标，确保用户在控制端操作时被控端屏幕不会残留静止光标；断开连接和析构时自动恢复系统光标，防止永久隐藏。

## 配置文件

配置文件位于 `%APPDATA%\ZeroBorders\config.json`，主要字段：

```json
{
    "role": 0,
    "rolePreference": 0,
    "pairingCode": "test123",
    "username": "user",
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
| `username` | 用户名，与配对码组合进行双重认证，两端需一致 |
| `layout` | 相对布局：`left_of` / `right_of` / `above` / `below` |
| `receiveDir` | 接收文件目录，留空则使用系统临时目录 |
| `startMinimized` | 是否启动时最小化到托盘 |

## 端口说明

| 端口 | 协议 | 用途 |
|------|------|------|
| 24800 | UDP | 局域网设备发现广播（同时发往 255.255.255.255 和子网定向广播） |
| 24801 | TCP | 控制通道（握手、输入、剪贴板通知、布局/路径同步、目录列表） |
| 24802 | TCP | 数据通道（文件分块、剪贴板图片） |

如 Windows 防火墙弹出提示，请允许 ZeroBorders 在专用网络通信。

## 待开发：内核驱动方案

### 背景

ZeroBorders 当前为纯用户态实现，使用 `SendInput` API 注入鼠标键盘事件。Windows 锁屏后切换到安全桌面（Winlogon desktop），用户态 API 无法向其注入输入，导致被锁定的一端在锁屏期间无法被跨屏控制。要突破此限制，需要引入内核驱动。

### 现状对比

| 维度 | ToDesk / 向日葵 | ZeroBorders（当前） |
|------|----------------|-------------------|
| 进程类型 | 系统服务（Session 0） | 用户态 GUI（用户会话） |
| 屏幕捕获 | 内核驱动，跨桌面 | 用户态 DXGI/BitBlt，仅用户桌面 |
| 输入注入 | 内核驱动写输入队列 | 用户态 SendInput，安全桌面失效 |
| 驱动签名 | WHQL 正式签名 | 无 |
| 锁屏后行为 | 继续捕获和注入 | 暂停（SendInput 失效），解锁后自动恢复 |

### 内核驱动方案技术栈

突破安全桌面限制需要以下基础设施：

1. **内核驱动开发（WDK）**
   - 使用 Windows Driver Kit (WDK) 开发 `.sys` 内核驱动
   - 输入注入：直接向键盘/鼠标类驱动的输入队列写入数据，绕过桌面隔离
   - 屏幕捕获（可选）：从显卡/framebuffer 直接读取像素，跨桌面捕获

2. **系统服务架构改造**
   - 当前 ZeroBorders 为用户态 GUI 程序，运行在用户会话
   - 需改造为**系统服务（Session 0）+ 用户态 UI** 的双进程架构
   - 系统服务在用户锁屏/注销时不受影响，捕获和注入逻辑持续运行

3. **驱动数字签名（关键门槛）**
   - Windows 10/11 x64 强制要求内核驱动有有效签名
   - 正式分发需要 **WHQL 签名**，流程为：
     1. 购买 **EV 代码签名证书**（约 2000-6000 元/年，由 DigiCert/Sectigo 等 CA 签发，交付为物理 USB 令牌）
     2. 用 EV 证书在微软硬件开发者中心注册
     3. 上传驱动，通过 HLK（硬件实验室工具包）测试
     4. 通过后微软返回 WHQL 签名，驱动才能在任意普通用户电脑加载
   - 无 WHQL 签名时，只能在**测试签名模式**下加载（`bcdedit /set testsigning on` + 重启），桌面右下角会显示"测试模式"水印，不适合正式分发

4. **INF 安装与软件集成**
   - 编写 INF 文件描述驱动安装
   - 软件安装时通过 `SetupCopyOEMInf` 或 `PnPUtil` 自动安装驱动
   - 驱动加载失败需有降级方案（回退到用户态模式）

### 风险与成本

| 项目 | 说明 |
|------|------|
| BSOD 风险 | 内核驱动任何 bug 都可能导致蓝屏，需在虚拟机调试 |
| 签名成本 | EV 证书年费数千元 + WHQL 测试环境搭建 |
| 兼容性维护 | 不同 Windows 版本（10/11 各构建）内核结构有差异 |
| 杀毒软件拦截 | 部分杀软会拦截未签名或非 WHQL 驱动加载 |
| 开发环境 | 需安装 WDK，当前 CMake + Qt 构建系统需新增驱动构建配置 |

### 开发计划

此项作为后续开发方向，暂未实施。短期方案（已实现）：锁屏自动暂停 + 解锁自动恢复，保证连接不断开、状态不丢失。待确认投入签名成本后，可启动内核驱动原型开发（先在测试签名模式下验证技术可行性）。
