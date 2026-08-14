#include "App.h"
#include "clipboard/ClipboardManager.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/Protocol.h"
#include "input/WinInputCapturer.h"
#include "input/WinInputInjector.h"
#include "network/NetworkClient.h"
#include "network/NetworkServer.h"
#include "router/ScreenRouter.h"
#include "transfer/FileTransferManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>

#include <QDateTime>
#include <QTimer>
#include <QVariantMap>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace zb {

namespace {

// UTF-8 ↔ UTF-16 conversion helpers (Windows paths are UTF-16).
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                  static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

} // namespace

App::App(QObject* parent) : QObject(parent) {}

App::~App() {
    stop();
}

bool App::isConnected() const {
    return connected_.load();
}

// ---------------------------------------------------------------------------
// State helpers — emit signals; Qt delivers them to the GUI thread safely.
// ---------------------------------------------------------------------------

void App::setStatus(const QString& s) {
    ZB_LOG_INFO("{}", s.toStdString());
    emit statusChanged(s);
}

void App::setConnected(bool v, const std::string& name, uint32_t w, uint32_t h) {
    connected_ = v;
    emit connectionChanged(v, QString::fromStdString(name), w, h);
}

void App::onTransferOffer(uint64_t id, uint64_t total) {
    emit transferProgress(id, 0, total);
}

void App::onTransferProgress(uint64_t id, uint64_t transferred, uint64_t total) {
    emit transferProgress(static_cast<quint64>(id),
                          static_cast<quint64>(transferred),
                          static_cast<quint64>(total));
}

void App::onTransferComplete(uint64_t id, bool ok, const std::string& msg) {
    emit transferComplete(static_cast<quint64>(id), ok,
                          QString::fromStdString(msg));
}

void App::installLogSink() {
    log::addSink([this](log::Level lvl, std::string_view line) {
        // Skip Info and Debug in the GUI log view to reduce noise.
        if (lvl == log::Level::Info || lvl == log::Level::Debug) return;
        QString tag;
        switch (lvl) {
            case log::Level::Debug: tag = "DBG"; break;
            case log::Level::Info:  tag = "INF"; break;
            case log::Level::Warn:  tag = "WRN"; break;
            case log::Level::Error: tag = "ERR"; break;
        }
        emit logMessage(tag, QString::fromUtf8(line.data(), static_cast<int>(line.size())));
    });
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

QString App::roleDescription() const {
    return isServer_.load() ? QStringLiteral("控制端") : QStringLiteral("被控端");
}

void App::setLayout(ScreenLayout layout) {
    layout_ = layout;
    if (router_) {
        router_->setLayout(layout);
    }
    // 本地主动变更时同步给对端；来自对端的变更不再回发，避免回环。
    if (!applyingRemoteLayout_.load()) {
        sendLayoutSync();
    }
}

void App::sendLayoutSync() {
    if (!running_.load() || !connected_.load()) return;
    LayoutSyncMsg msg;
    msg.layout = layout_;
    std::vector<uint8_t> payload = serializeLayoutSync(msg);
    if (isServer_.load()) {
        if (server_ && server_->isReady())
            server_->sendControl(MsgType::LayoutSync, payload);
    } else {
        if (client_ && client_->isReady())
            client_->sendControl(MsgType::LayoutSync, payload);
    }
}

void App::applyRemoteLayout(ScreenLayout layout) {
    applyingRemoteLayout_ = true;
    setLayout(layout);
    applyingRemoteLayout_ = false;
    // 通知 GUI 更新设备位置画布（在 GUI 线程执行）。
    emit layoutChanged(layout);
}

void App::sendPathSync() {
    if (!running_.load() || !connected_.load()) return;
    PathSyncMsg msg;
    msg.receiveDir = config_.receiveDir;
    auto payload = serializePathSync(msg);
    if (isServer_.load()) {
        if (server_ && server_->isReady())
            server_->sendControl(MsgType::PathSync, payload);
    } else {
        if (client_ && client_->isReady())
            client_->sendControl(MsgType::PathSync, payload);
    }
}

void App::notifyPathSync() {
    sendPathSync();
}

void App::applyRemotePath(const std::string& dir) {
    emit remoteReceiveDirChanged(QString::fromStdString(dir));
}

// ---------------------------------------------------------------------------
// Session lock/unlock handling
// ---------------------------------------------------------------------------
// When the local Windows session locks, SendInput is ignored on the secure
// desktop, so any remote-control cursor would be stuck. We force control back
// to the local side and tell the peer we're locked so it can do the same.
// On unlock we re-sync state (layout, clipboard, path, remote dir listing)
// so cross-machine operation resumes seamlessly.

void App::onSessionLock() {
    if (!running_.load()) return;
    // Force local control regardless of connection state — the router may
    // still be in remote-control mode and we don't want the cursor stuck.
    if (router_) router_->forceLocalControl();
    setStatus(QStringLiteral("本地会话已锁定，已切换到本地控制"));
    if (connected_.load()) {
        SessionLockMsg msg{1};
        auto payload = serializeSessionLock(msg);
        if (isServer_.load()) {
            if (server_ && server_->isReady())
                server_->sendControl(MsgType::SessionLock, payload);
        } else {
            if (client_ && client_->isReady())
                client_->sendControl(MsgType::SessionLock, payload);
        }
    }
}

void App::onSessionUnlock() {
    if (!running_.load()) return;
    setStatus(QStringLiteral("本地会话已解锁"));
    if (connected_.load()) {
        // Notify peer we're back so it can resume remote control.
        SessionLockMsg msg{0};
        auto payload = serializeSessionLock(msg);
        if (isServer_.load()) {
            if (server_ && server_->isReady())
                server_->sendControl(MsgType::SessionLock, payload);
        } else {
            if (client_ && client_->isReady())
                client_->sendControl(MsgType::SessionLock, payload);
        }
        // Re-sync state so cross-machine operation resumes cleanly.
        sendLayoutSync();
        sendPathSync();
        if (clipboard_) clipboard_->syncNow();
        requestRemoteDirList("");
        setStatus(QStringLiteral("本地会话已解锁，状态已重新同步"));
    }
}

void App::handlePeerSessionLock(bool locked) {
    peerLocked_ = locked;
    if (locked) {
        // Peer's session is locked — it can't process injected input. If we
        // were remotely controlling it, fall back to local control so the
        // cursor isn't stuck on a desktop that ignores SendInput.
        if (router_) router_->forceLocalControl();
        setStatus(QStringLiteral("对端会话已锁定，跨屏操作暂停，解锁后自动恢复"));
    } else {
        // Peer unlocked — re-sync our clipboard so the peer has the latest
        // content (the secure desktop may have cleared/altered state).
        if (clipboard_) clipboard_->syncNow();
        setStatus(QStringLiteral("对端会话已解锁，可继续跨屏操作"));
    }
}

// ---------------------------------------------------------------------------
// Remote directory listing
// ---------------------------------------------------------------------------

void App::requestRemoteDirList(const std::string& path) {
    if (!running_.load() || !connected_.load()) return;
    ListDirRequestMsg msg;
    msg.requestId = ++listDirReqId_;
    msg.path = path;
    auto payload = serializeListDirRequest(msg);
    if (isServer_.load()) {
        if (server_ && server_->isReady())
            server_->sendControl(MsgType::ListDirRequest, payload);
    } else {
        if (client_ && client_->isReady())
            client_->sendControl(MsgType::ListDirRequest, payload);
    }
}

std::vector<DirEntry> App::scanLocalDirectory(const std::string& dirUtf8, bool& ok) {
    std::vector<DirEntry> result;
    ok = false;

    // Convert UTF-8 → UTF-16 for Windows filesystem APIs.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(),
                                   static_cast<int>(dirUtf8.size()),
                                   nullptr, 0);
    if (wlen <= 0) return result;
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, dirUtf8.c_str(),
                        static_cast<int>(dirUtf8.size()),
                        wpath.data(), wlen);

    std::error_code ec;
    auto it = fs::directory_iterator(wpath, ec);
    if (ec) {
        ZB_LOG_WARN("scanLocalDirectory open [{}] failed: {}",
                    dirUtf8, ec.message());
        return result;
    }

    for (const auto& entry : it) {
        DirEntry de;
        de.name = wideToUtf8(entry.path().filename().wstring());
        if (de.name.empty()) continue;
        std::error_code ecStat;
        de.isDirectory = entry.is_directory(ecStat);
        if (!de.isDirectory) {
            auto sz = entry.file_size(ecStat);
            de.size = ecStat ? 0 : static_cast<uint64_t>(sz);
        }
        auto ftime = entry.last_write_time(ecStat);
        if (!ecStat) {
            // Convert file_time_type to Unix timestamp.
            auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
                ftime - fs::file_time_type::clock::now()
                + std::chrono::system_clock::now());
            de.mtime = static_cast<int64_t>(sctp.time_since_epoch().count());
        }
        result.push_back(std::move(de));
    }

    // Sort: directories first, then files; case-insensitive by name.
    std::sort(result.begin(), result.end(),
              [](const DirEntry& a, const DirEntry& b) {
                  if (a.isDirectory != b.isDirectory) return a.isDirectory;
                  return _wcsicmp(utf8ToWide(a.name).c_str(),
                                  utf8ToWide(b.name).c_str()) < 0;
              });

    ok = true;
    return result;
}

void App::handleListDirRequest(const std::vector<uint8_t>& payload) {
    ListDirRequestMsg req;
    if (!parseListDirRequest(payload, req)) return;

    ListDirResponseMsg resp;
    resp.requestId = req.requestId;

    // Empty path means "root" — list all available drives on Windows.
    std::string target = req.path;
    if (target.empty()) {
        // Enumerate all logical drives.
        wchar_t drives[256];
        DWORD len = GetLogicalDriveStringsW(256, drives);
        std::vector<DirEntry> entries;
        if (len > 0 && len < 256) {
            const wchar_t* p = drives;
            while (*p) {
                std::wstring ws(p);
                DirEntry de;
                de.name = wideToUtf8(ws);  // e.g. "C:\"
                de.isDirectory = true;
                de.size = 0;
                de.mtime = 0;
                entries.push_back(std::move(de));
                p += ws.size() + 1;
            }
        }
        resp.path = "";
        resp.entries = std::move(entries);
        resp.result = 0;

        auto out = serializeListDirResponse(resp);
        if (isServer_.load()) {
            if (server_ && server_->isReady())
                server_->sendControl(MsgType::ListDirResponse, out);
        } else {
            if (client_ && client_->isReady())
                client_->sendControl(MsgType::ListDirResponse, out);
        }
        return;
    }

    resp.path = target;

    bool ok = false;
    resp.entries = scanLocalDirectory(target, ok);
    resp.result = ok ? 0 : 1;

    auto out = serializeListDirResponse(resp);
    if (isServer_.load()) {
        if (server_ && server_->isReady())
            server_->sendControl(MsgType::ListDirResponse, out);
    } else {
        if (client_ && client_->isReady())
            client_->sendControl(MsgType::ListDirResponse, out);
    }
}

void App::handleListDirResponse(const std::vector<uint8_t>& payload) {
    ListDirResponseMsg resp;
    if (!parseListDirResponse(payload, resp)) return;

    QVariantList list;
    for (const auto& e : resp.entries) {
        QVariantMap m;
        m[QStringLiteral("name")] = QString::fromStdString(e.name);
        m[QStringLiteral("size")] = static_cast<qulonglong>(e.size);
        m[QStringLiteral("isDir")] = e.isDirectory;
        // Convert Unix seconds to QDateTime.
        QDateTime dt;
        dt.setSecsSinceEpoch(static_cast<qint64>(e.mtime));
        m[QStringLiteral("mtime")] = dt;
        list.append(m);
    }

    emit remoteDirListed(QString::fromStdString(resp.path),
                         resp.result == 0, list);
}

void App::requestRemoteFiles(const std::vector<std::string>& remotePaths,
                             const std::string& destDir) {
    if (!running_.load() || !connected_.load()) return;
    FilePullRequestMsg msg;
    msg.requestId = ++pullReqId_;
    msg.destDir = destDir;
    msg.paths = remotePaths;
    auto payload = serializeFilePullRequest(msg);
    // Remember destination so the incoming FileOffer auto-accepts to it.
    {
        std::lock_guard<std::mutex> lk(pendingPullMutex_);
        pendingPullDestDir_ = destDir;
    }
    if (isServer_.load()) {
        if (server_ && server_->isReady())
            server_->sendControl(MsgType::FilePullRequest, payload);
    } else {
        if (client_ && client_->isReady())
            client_->sendControl(MsgType::FilePullRequest, payload);
    }
    ZB_LOG_INFO("Pull request sent: {} file(s) -> {}", remotePaths.size(), destDir);
}

void App::handleFilePullRequest(const std::vector<uint8_t>& payload) {
    FilePullRequestMsg req;
    if (!parseFilePullRequest(payload, req)) return;

    ZB_LOG_INFO("Received pull request for {} file(s)", req.paths.size());

    // Validate that paths exist locally before initiating the send.
    std::vector<std::string> validPaths;
    validPaths.reserve(req.paths.size());
    for (const auto& p : req.paths) {
        std::wstring wp;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, p.c_str(),
                                       static_cast<int>(p.size()), nullptr, 0);
        if (wlen > 0) {
            wp.resize(wlen);
            MultiByteToWideChar(CP_UTF8, 0, p.c_str(),
                                static_cast<int>(p.size()), wp.data(), wlen);
        }
        std::error_code ec;
        if (fs::exists(wp, ec)) {
            validPaths.push_back(p);
        } else {
            ZB_LOG_WARN("Pull request path not found: {}", p);
        }
    }

    if (validPaths.empty()) {
        ZB_LOG_WARN("Pull request has no valid paths, ignoring");
        return;
    }

    // Act as sender: initiate a normal file transfer back to the requester.
    uint64_t id = fileTransfer_ ? fileTransfer_->sendFiles(validPaths) : 0;
    if (id == 0) {
        ZB_LOG_WARN("Failed to start send for pull request");
    }
}

void App::startServer(const AppConfig& cfg) {
    if (running_.load()) return;
    launchServer(cfg, true);
}

void App::startClient(const AppConfig& cfg) {
    if (running_.load()) return;
    launchClient(cfg, cfg.host);
}

void App::launchServer(const AppConfig& cfg, bool enableDiscovery) {
    config_ = cfg;
    layout_ = layoutFromString(cfg.layout);

    std::string name = cfg.serverName;
    if (name.empty()) {
        char hostname[256]{};
        DWORD size = sizeof(hostname);
        if (GetComputerNameA(hostname, &size)) name = hostname;
        else name = "ZeroBorders-Server";
    }

    localW_ = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
    localH_ = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));

    TokenHash token = sha256(cfg.pairingCode + "|" + cfg.username);

    server_ = std::make_unique<NetworkServer>(token, name,
        cfg.controlPort, cfg.dataPort, cfg.udpPort);
    capturer_ = std::make_unique<WinInputCapturer>();
    clipboard_ = std::make_unique<ClipboardManager>();
    router_ = std::make_unique<ScreenRouter>();
    fileTransfer_ = std::make_unique<FileTransferManager>();
    inputSender_ = std::make_unique<InputEventSender>();

    wireServerCallbacks();

    isServer_ = true;
    running_ = true;

    setStatus(QStringLiteral("等待对端连接..."));

    server_->start(
        [this] {
            setConnected(true, "", localW_, localH_);
            clientW_ = localW_;
            clientH_ = localH_;
            router_->configure(localW_, localH_, clientW_.load(),
                               clientH_.load(), layout_);
            setStatus(QStringLiteral("对端已连接"));
            // Sync local clipboard to the newly connected peer.
            if (clipboard_) clipboard_->syncNow();
            // Push the current relative layout to the peer so both sides
            // start from the same arrangement.
            sendLayoutSync();
            // Exchange receive directory paths so each side can display
            // where files will land on the remote machine.
            sendPathSync();
            // Request the peer's receive directory listing to populate
            // the remote file browser.
            requestRemoteDirList("");
        },
        [this](const std::string& reason) {
            setConnected(false);
            if (router_) router_->forceLocalControl();
            if (running_.load()) {
                setStatus(QStringLiteral("对端已断开，正在重新搜索..."));
                // In auto mode the server was launched with discovery
                // disabled (autoDiscovery_ was still running at launch time).
                // Restart UDP broadcast now so a restarted peer can find us.
                // Post to the GUI thread because this callback runs on the
                // server's cleanup thread.
                if (autoMode_.load() && server_) {
                    QMetaObject::invokeMethod(this, [this] {
                        if (autoMode_.load() && running_.load() && server_) {
                            server_->restartDiscovery();
                        }
                    }, Qt::QueuedConnection);
                }
            } else {
                setStatus(QStringLiteral("对端已断开：") + QString::fromStdString(reason));
            }
        },
        enableDiscovery);

    capturer_->start([this](const InputEvent& ev) -> bool {
        if (server_ && server_->isReady()) return router_->processEvent(ev);
        return false;
    });
}

void App::launchClient(const AppConfig& cfg, const std::string& host,
                       int connectTimeoutMs) {
    config_ = cfg;
    layout_ = layoutFromString(cfg.layout);

    TokenHash token = sha256(cfg.pairingCode + "|" + cfg.username);

    client_ = std::make_unique<NetworkClient>(token,
        cfg.controlPort, cfg.dataPort, cfg.udpPort);
    injector_ = std::make_unique<WinInputInjector>();
    clipboard_ = std::make_unique<ClipboardManager>();
    fileTransfer_ = std::make_unique<FileTransferManager>();

    wireClientCallbacks();

    isServer_ = false;
    running_ = true;

    setStatus(host.empty() ? QStringLiteral("正在搜索对端...")
                           : QStringLiteral("正在连接 %1...").arg(QString::fromStdString(host)));

    auto onReady = [this](const WelcomeMsg& w) {
        setConnected(true, "", w.screenWidth, w.screenHeight);
        setStatus(QStringLiteral("已连接，对端分辨率 %1x%2")
                      .arg(w.screenWidth).arg(w.screenHeight));
        // Sync local clipboard to the newly connected peer.
        if (clipboard_) clipboard_->syncNow();
        // Exchange receive directory paths and request remote listing.
        sendPathSync();
        requestRemoteDirList("");
    };
    auto onDisconnect = [this](const std::string& reason) {
        bool wasConnected = connected_.exchange(false);
        hasControl_ = false;
        // 断开连接时恢复被控端光标，防止光标永久隐藏。
        if (injector_) injector_->setCursorVisible(true);
        emit connectionChanged(false, "", 0, 0);
        if (wasConnected && running_.load()) {
            setStatus(QStringLiteral("连接断开，正在重新搜索..."));
            // In auto mode, the client's built-in scheduleReconnect only
            // tries direct TCP to lastHost_. If the server crashed and
            // restarted (new process), it may have a different IP or may
            // need UDP discovery. Post to the GUI thread to stop the
            // client and restart full auto discovery.
            if (autoMode_.load()) {
                QMetaObject::invokeMethod(this, [this] {
                    restartAutoDiscovery();
                }, Qt::QueuedConnection);
            }
        } else if (!running_.load()) {
            setStatus(QStringLiteral("已断开：") + QString::fromStdString(reason));
        }
    };

    bool ok = false;
    if (!host.empty()) {
        ok = client_->connectToHost(host, onReady, onDisconnect, connectTimeoutMs);
    } else {
        ok = client_->start(onReady, onDisconnect, 15000);
    }

    if (!ok) {
        setStatus(QStringLiteral("连接建立失败"));
        resetSession();
        running_ = false;
        emit sessionStopped();
    }
}

void App::launchClientAuto(const AppConfig& cfg, const std::string& host) {
    // Called on the UDP discovery thread after election. There is a small
    // race: the server side may not have bound its TCP listener yet. We wait
    // briefly and retry a couple of times. This runs on a background thread,
    // so blocking here does not freeze the UI.
    isServer_ = false;

    auto tryConnect = [this, &cfg, &host](int timeoutMs) -> bool {
        // launchClient sets up members and blocks until connected or timeout.
        // On failure it calls resetSession() + sets running_ = false.
        // We need to restore running_ so we can retry.
        launchClient(cfg, host, timeoutMs);
        return connected_.load();
    };

    // First attempt after a short delay to let the server start listening.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!running_.load()) return;
    if (tryConnect(3000)) return;

    // Second attempt after a longer delay.
    if (!running_.load()) return;
    setStatus(QStringLiteral("正在重试连接..."));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    if (!running_.load()) return;
    running_ = true; // launchClient failure cleared it
    if (tryConnect(3000)) return;

    // Final attempt.
    if (!running_.load()) return;
    setStatus(QStringLiteral("正在重试连接..."));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (!running_.load()) return;
    running_ = true;
    tryConnect(5000);
}

void App::startAuto(const AppConfig& cfg) {
    if (running_.load()) return;
    config_ = cfg;
    layout_ = layoutFromString(cfg.layout);

    std::random_device rd;
    std::mt19937_64 rng(rd());
    nodeId_ = rng();
    if (nodeId_ == 0) nodeId_ = 1;

    // Combine pairing code + username for the token hash (double authentication).
    TokenHash token = sha256(cfg.pairingCode + "|" + cfg.username);

    running_ = true;
    autoMode_ = true;
    isServer_ = false;

    setStatus(QStringLiteral("正在局域网搜索对端..."));

    autoDiscovery_ = std::make_unique<UdpDiscovery>();
    // The callback runs on the UDP discovery thread. After finding a peer,
    // the discovery thread keeps broadcasting for a ~3s grace period so the
    // other side also receives our packet. We must NOT join/reset it here
    // (that would block). Instead post the session launch to the main thread
    // and schedule cleanup after the grace period elapses.
    autoDiscovery_->startAuto(cfg.udpPort, token, nodeId_, cfg.rolePreference,
        [this, cfg](const std::string& peerIp, UdpDiscovery::AutoRole role) {
            bool isServer = (role == UdpDiscovery::AutoRole::Server);
            QMetaObject::invokeMethod(this, [this, cfg, peerIp, isServer] {
                emit roleDetermined(isServer);

                if (isServer) {
                    ZB_LOG_INFO("Auto role: server (controlling side)");
                    std::thread([this, cfg] {
                        launchServer(cfg, false);
                    }).detach();
                } else {
                    ZB_LOG_INFO("Auto role: client (controlled side), peer={}", peerIp);
                    std::thread([this, cfg, peerIp] {
                        launchClientAuto(cfg, peerIp);
                    }).detach();
                }

                // Discovery thread enters a 3s grace period after onFound.
                // Schedule reset after it finishes to avoid blocking the UI.
                QTimer::singleShot(4000, this, [this] {
                    if (autoDiscovery_) {
                        autoDiscovery_.reset();
                    }
                });
            }, Qt::QueuedConnection);
        });
}

void App::restartAutoDiscovery() {
    // Runs on the GUI thread (posted via QMetaObject::invokeMethod from the
    // disconnect callback). If the client already managed to reconnect via
    // scheduleReconnect before we got here, bail out.
    if (!autoMode_.load() || !running_.load()) return;
    if (connected_.load()) return;

    ZB_LOG_INFO("Restarting auto discovery after disconnect");

    // Stop the current client (and its reconnect thread) or server.
    // scheduleReconnect uses a condition variable so stop() returns quickly.
    if (client_) client_->stop();
    if (server_) server_->stop();

    // Tear down all session-specific components. running_ stays true so the
    // session is considered active (just searching for a peer).
    if (capturer_) { capturer_->stop(); }
    if (clipboard_) { clipboard_->stop(); }
    if (inputSender_) { inputSender_->stop(); }
    if (fileTransfer_) { fileTransfer_->cancelAll(); }

    client_.reset();
    server_.reset();
    capturer_.reset();
    injector_.reset();
    clipboard_.reset();
    router_.reset();
    fileTransfer_.reset();
    inputSender_.reset();
    autoDiscovery_.reset();

    isServer_ = false;
    hasControl_ = false;

    setStatus(QStringLiteral("正在重新搜索对端..."));

    // Reuse the existing nodeId_ so role election is stable across reconnects.
    TokenHash token = sha256(config_.pairingCode + "|" + config_.username);
    autoDiscovery_ = std::make_unique<UdpDiscovery>();
    autoDiscovery_->startAuto(config_.udpPort, token, nodeId_, config_.rolePreference,
        [this](const std::string& peerIp, UdpDiscovery::AutoRole role) {
            bool isServer = (role == UdpDiscovery::AutoRole::Server);
            QMetaObject::invokeMethod(this, [this, peerIp, isServer] {
                if (!running_.load() || connected_.load()) return;
                emit roleDetermined(isServer);

                if (isServer) {
                    ZB_LOG_INFO("Auto role: server (controlling side)");
                    std::thread([this] {
                        launchServer(config_, false);
                    }).detach();
                } else {
                    ZB_LOG_INFO("Auto role: client (controlled side), peer={}", peerIp);
                    std::thread([this, peerIp] {
                        launchClientAuto(config_, peerIp);
                    }).detach();
                }

                QTimer::singleShot(4000, this, [this] {
                    if (autoDiscovery_) {
                        autoDiscovery_.reset();
                    }
                });
            }, Qt::QueuedConnection);
        });
}

void App::stop() {
    if (!running_.exchange(false)) return;

    ZB_LOG_INFO("Stopping session...");

    // Shutdown order matters: stop the producers of work first so the
    // consumers below are not touched by in-flight callbacks.
    //   1. UDP discovery  - no more peer notifications
    //   2. Input capturer - hook thread stops calling router/server
    //   3. Network server/client - recv threads stop calling
    //      fileTransfer/clipboard callbacks
    //   4. Input sender   - drain pending events
    //   5. File transfer  - safe to cancel now that recv threads are gone
    //   6. Clipboard      - listener thread stops last
    if (autoDiscovery_) {
        autoDiscovery_->stop();
        autoDiscovery_.reset();
    }
    if (capturer_) capturer_->stop();
    if (server_) server_->stop();
    if (client_) client_->stop();
    if (inputSender_) inputSender_->stop();
    if (fileTransfer_) fileTransfer_->cancelAll();
    if (clipboard_) clipboard_->stop();

    autoMode_ = false;
    resetSession();
    setConnected(false);
    setStatus(QStringLiteral("已停止"));
    emit sessionStopped();
}

void App::resetSession() {
    autoDiscovery_.reset();
    inputSender_.reset();
    fileTransfer_.reset();
    capturer_.reset();
    injector_.reset();
    clipboard_.reset();
    router_.reset();
    server_.reset();
    client_.reset();
}

// ---------------------------------------------------------------------------
// File transfer
// ---------------------------------------------------------------------------

uint64_t App::sendFiles(const std::vector<std::string>& paths,
                        uint8_t flags,
                        const std::string& destDir) {
    if (!fileTransfer_ || !running_.load()) return 0;
    return fileTransfer_->sendFiles(paths, flags, destDir);
}

void App::acceptTransfer(uint64_t id, const std::string& destDir) {
    if (fileTransfer_) fileTransfer_->accept(id, destDir);
}

void App::rejectTransfer(uint64_t id) {
    if (fileTransfer_) fileTransfer_->reject(id);
}

// ---------------------------------------------------------------------------
// Server wiring
// ---------------------------------------------------------------------------

void App::wireFileTransferCallbacks() {
    fileTransfer_->onProgress([this](uint64_t id, uint64_t x, uint64_t tot) {
        onTransferProgress(id, x, tot);
    });
    fileTransfer_->onComplete([this](uint64_t id, bool ok, const std::string& m) {
        onTransferComplete(id, ok, m);
    });
    fileTransfer_->onOffer([this](uint64_t id, const std::vector<TransferEntry>&,
                                  uint64_t total, const std::string& /*destDir*/,
                                  uint8_t /*flags*/) {
        onTransferOffer(id, total);
    });

    // Incoming offers are auto-accepted because every transfer in this
    // application is explicitly user-initiated (right-click upload/download
    // or clipboard file copy). A modal confirmation dialog on the receiver
    // is counter-productive in a KVM context: it appears on the controlled
    // machine while the user is driving it from the controller, and causes
    // the sender to hang at 0% if it is never clicked.
    //
    // Destination directory priority:
    //   1. pendingPullDestDir_ — set when this side requested a download
    //   2. offerDestDir       — set by the sender (e.g. upload to remote browser path)
    //   3. config_.receiveDir — configured default
    //   4. TEMP/ZeroBorders   — fallback in accept()
    fileTransfer_->setIncomingOfferCallback(
        [this](uint64_t id, const std::vector<TransferEntry>& /*entries*/,
               uint64_t total, const std::string& offerDestDir, uint8_t /*flags*/) {
            std::string pullDest;
            {
                std::lock_guard<std::mutex> lk(pendingPullMutex_);
                pullDest = pendingPullDestDir_;
                pendingPullDestDir_.clear();
            }
            std::string destDir;
            if (!pullDest.empty()) {
                destDir = pullDest;
            } else if (!offerDestDir.empty()) {
                destDir = offerDestDir;
            } else {
                destDir = config_.receiveDir;
            }
            ZB_LOG_INFO("Auto-accepting transfer {} ({} bytes) to {}",
                        id, total, destDir.empty() ? "(default)" : destDir);
            onTransferOffer(id, total);
            fileTransfer_->accept(id, destDir);
        });

    // Configure the default receive directory (used as a fallback / when the
    // incoming-offer callback accepts without overriding the directory).
    if (!config_.receiveDir.empty()) {
        fileTransfer_->setDefaultReceiveDir(config_.receiveDir);
    }

    // When a clipboard-initiated file transfer completes, put the received
    // file paths into the local clipboard so Ctrl+V works on the receiver.
    fileTransfer_->setClipboardCompleteCallback(
        [this](uint64_t /*id*/, const std::vector<std::string>& paths) {
            if (clipboard_) clipboard_->setRemoteFiles(paths);
        });
}

void App::wireServerCallbacks() {
    fileTransfer_->setSendCallback([this](MsgType t, const std::vector<uint8_t>& p) -> bool {
        if (!server_) return false;
        // Control-channel messages (0x01-0x1F) include FileOffer/FileAccept/
        // TransferComplete; data-channel messages (0x20+) include FileChunk/
        // FileChunkAck. Route each to the correct socket so small control
        // messages are not delayed by bulk data and get TCP_NODELAY.
        if (static_cast<uint8_t>(t) < 0x20)
            return server_->sendControl(t, p);
        else
            return server_->sendData(t, p);
    });
    wireFileTransferCallbacks();

    // Dedicated sender thread coalesces mouse-move events so the hook thread
    // never blocks on TCP sends and clicks are not delayed by a move backlog.
    inputSender_->start([this](const InputEvent& ev) {
        if (server_ && server_->isReady()) {
            server_->sendControl(MsgType::InputEvent, serializeInputEvent(ev));
        }
    });

    clipboard_->start([this](const ClipboardContent& content) {
        if (!server_ || !server_->isReady()) return;
        if (content.format == ClipboardFormat::Text) {
            ClipboardTextMsg msg;
            msg.text = content.text;
            server_->sendControl(MsgType::ClipboardText, serializeClipboardText(msg));
        } else if (content.format == ClipboardFormat::Image) {
            ClipboardImageMsg msg;
            msg.width = content.imageWidth;
            msg.height = content.imageHeight;
            msg.data = content.pngData;
            server_->sendData(MsgType::ClipboardImage, serializeClipboardImage(msg));
        } else if (content.format == ClipboardFormat::Files) {
            if (fileTransfer_) {
                fileTransfer_->sendFiles(content.filePaths, 1);  // flags=1 → clipboard
            }
        }
    });

    router_->onCursorEnter([this](const CursorEnterMsg& msg) {
        if (server_) server_->sendControl(MsgType::CursorEnter, serializeCursorEnter(msg));
    });
    router_->onCursorLeave([this](Edge clientEdge) {
        // 通知被控端光标已离开，被控端据此隐藏本地光标并停止接受输入注入。
        if (server_) {
            CursorLeaveMsg msg{};
            msg.edge = clientEdge;
            server_->sendControl(MsgType::CursorLeave, serializeCursorLeave(msg));
        }
    });
    router_->onSuppress([this](bool suppress) {
        if (capturer_) capturer_->setSuppress(suppress);
    });
    router_->onWarpCursor([this](int32_t x, int32_t y) {
        if (capturer_) capturer_->warpCursor(x, y);
        else SetCursorPos(x, y);
    });
    router_->onReleaseButtons([this] {
        if (capturer_) capturer_->releaseAllButtons();
    });
    // 进入/退出远程控制时向被控端发送所有修饰键的 KeyUp 事件，
    // 防止 Ctrl/Shift/Alt/Win 在对端残留导致组合键卡住
    // （如 Ctrl 卡住导致滚轮缩放网页、数字键变成快捷键）。
    router_->onReleaseKeys([this] {
        if (!inputSender_) return;
        InputEvent ev{};
        ev.type = EventType::KeyUp;
        // 释放左右 Ctrl/Shift/Alt/Win
        uint16_t mods[] = {VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT,
                           VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};
        for (uint16_t vk : mods) {
            ev.key.vkCode = vk;
            ev.key.scanCode = 0;
            ev.key.extended = (vk == VK_RMENU || vk == VK_RCONTROL ||
                               vk == VK_RSHIFT || vk == VK_RWIN);
            inputSender_->submit(ev);
        }
    });
    router_->onCursorVisible([this](bool visible) {
        if (capturer_) capturer_->setCursorVisible(visible);
    });
    router_->onSendEvent([this](const InputEvent& ev) {
        if (inputSender_) inputSender_->submit(ev);
    });

    server_->onControlMessage([this](MsgType t, const std::vector<uint8_t>& p) {
        try {
            if (t == MsgType::CursorLeave) {
                CursorLeaveMsg msg{};
                if (parseCursorLeave(p, msg)) router_->handleCursorLeave(msg.edge);
            } else if (t == MsgType::ClipboardText) {
                ClipboardTextMsg msg{};
                if (parseClipboardText(p, msg)) clipboard_->setRemoteText(msg.text);
            } else if (t == MsgType::LayoutSync) {
                LayoutSyncMsg msg{};
                if (parseLayoutSync(p, msg)) applyRemoteLayout(msg.layout);
            } else if (t == MsgType::PathSync) {
                PathSyncMsg msg{};
                if (parsePathSync(p, msg)) applyRemotePath(msg.receiveDir);
            } else if (t == MsgType::ListDirRequest) {
                handleListDirRequest(p);
            } else if (t == MsgType::ListDirResponse) {
                handleListDirResponse(p);
            } else if (t == MsgType::FilePullRequest) {
                handleFilePullRequest(p);
            } else if (t == MsgType::SessionLock) {
                SessionLockMsg msg{};
                if (parseSessionLock(p, msg)) handlePeerSessionLock(msg.state != 0);
            } else if (fileTransfer_ &&
                       (t == MsgType::FileOffer || t == MsgType::FileAccept ||
                        t == MsgType::TransferProgress || t == MsgType::TransferComplete)) {
                fileTransfer_->handleMessage(t, p);
            }
        } catch (const std::exception& e) {
            ZB_LOG_ERROR("Server ctrl callback error: {}", e.what());
        }
    });

    server_->onDataMessage([this](MsgType t, const std::vector<uint8_t>& p) {
        try {
            if (t == MsgType::ClipboardImage) {
                ClipboardImageMsg msg{};
                if (parseClipboardImage(p, msg))
                    clipboard_->setRemoteImage(msg.data, msg.width, msg.height);
            } else if (fileTransfer_) {
                fileTransfer_->handleMessage(t, p);
            }
        } catch (const std::exception& e) {
            ZB_LOG_ERROR("Server data callback error: {}", e.what());
        }
    });
}

// ---------------------------------------------------------------------------
// Client wiring
// ---------------------------------------------------------------------------

void App::wireClientCallbacks() {
    fileTransfer_->setSendCallback([this](MsgType t, const std::vector<uint8_t>& p) -> bool {
        if (!client_) return false;
        // Route control messages (0x01-0x1F) to the control channel and data
        // messages (0x20+) to the data channel. See wireServerCallbacks().
        if (static_cast<uint8_t>(t) < 0x20)
            return client_->sendControl(t, p);
        else
            return client_->sendData(t, p);
    });
    wireFileTransferCallbacks();

    clipboard_->start([this](const ClipboardContent& content) {
        if (!client_ || !client_->isReady()) return;
        if (content.format == ClipboardFormat::Text) {
            ClipboardTextMsg msg;
            msg.text = content.text;
            client_->sendControl(MsgType::ClipboardText, serializeClipboardText(msg));
        } else if (content.format == ClipboardFormat::Image) {
            ClipboardImageMsg msg;
            msg.width = content.imageWidth;
            msg.height = content.imageHeight;
            msg.data = content.pngData;
            client_->sendData(MsgType::ClipboardImage, serializeClipboardImage(msg));
        } else if (content.format == ClipboardFormat::Files) {
            if (fileTransfer_) {
                fileTransfer_->sendFiles(content.filePaths, 1);
            }
        }
    });

    client_->onControlMessage([this](MsgType t, const std::vector<uint8_t>& p) {
        try {
            if (t == MsgType::InputEvent) {
                if (hasControl_.load()) {
                    InputEvent ev{};
                    if (parseInputEvent(p, ev)) injector_->inject(ev);
                }
            } else if (t == MsgType::CursorEnter) {
                CursorEnterMsg msg{};
                if (parseCursorEnter(p, msg)) {
                    // 进入被控模式前释放所有修饰键，清理之前可能残留的
                    // Ctrl/Shift/Alt/Win 状态，防止跨屏切换时组合键卡住。
                    injector_->releaseAllKeys();
                    injector_->injectMouseMove(msg.x, msg.y);
                    hasControl_ = true;
                    // 显示本地光标，让用户在被控端屏幕上看到鼠标。
                    injector_->setCursorVisible(true);
                }
            } else if (t == MsgType::CursorLeave) {
                // 控制端鼠标已返回，被控端停止接受输入注入并隐藏本地光标，
                // 避免被控端屏幕上残留静止光标干扰用户在控制端的操作。
                hasControl_ = false;
                injector_->releaseAllKeys();
                injector_->setCursorVisible(false);
            } else if (t == MsgType::Ping) {
                uint64_t ts = 0;
                if (parsePingPong(p, ts))
                    client_->sendControl(MsgType::Pong, serializePingPong(ts));
            } else if (t == MsgType::ClipboardText) {
                ClipboardTextMsg msg{};
                if (parseClipboardText(p, msg)) clipboard_->setRemoteText(msg.text);
            } else if (t == MsgType::LayoutSync) {
                LayoutSyncMsg msg{};
                if (parseLayoutSync(p, msg)) applyRemoteLayout(msg.layout);
            } else if (t == MsgType::PathSync) {
                PathSyncMsg msg{};
                if (parsePathSync(p, msg)) applyRemotePath(msg.receiveDir);
            } else if (t == MsgType::ListDirRequest) {
                handleListDirRequest(p);
            } else if (t == MsgType::ListDirResponse) {
                handleListDirResponse(p);
            } else if (t == MsgType::FilePullRequest) {
                handleFilePullRequest(p);
            } else if (t == MsgType::SessionLock) {
                SessionLockMsg msg{};
                if (parseSessionLock(p, msg)) handlePeerSessionLock(msg.state != 0);
            } else if (fileTransfer_ &&
                       (t == MsgType::FileOffer || t == MsgType::FileAccept ||
                        t == MsgType::TransferProgress || t == MsgType::TransferComplete)) {
                fileTransfer_->handleMessage(t, p);
            }
        } catch (const std::exception& e) {
            ZB_LOG_ERROR("Client ctrl callback error: {}", e.what());
        }
    });

    client_->onDataMessage([this](MsgType t, const std::vector<uint8_t>& p) {
        try {
            if (t == MsgType::ClipboardImage) {
                ClipboardImageMsg msg{};
                if (parseClipboardImage(p, msg))
                    clipboard_->setRemoteImage(msg.data, msg.width, msg.height);
            } else if (fileTransfer_) {
                fileTransfer_->handleMessage(t, p);
            }
        } catch (const std::exception& e) {
            ZB_LOG_ERROR("Client data callback error: {}", e.what());
        }
    });
}

} // namespace zb
