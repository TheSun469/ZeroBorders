#pragma once

#include "config/AppConfig.h"
#include "core/ScreenLayout.h"
#include "network/UdpDiscovery.h"
#include "router/InputEventSender.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace zb {

class NetworkServer;
class NetworkClient;
class WinInputCapturer;
class WinInputInjector;
class ClipboardManager;
class ScreenRouter;
class FileTransferManager;
struct InputEvent;

// Snapshot of a file transfer for the UI.
struct TransferStatus {
    uint64_t id = 0;
    uint64_t transferred = 0;
    uint64_t total = 0;
    bool active = false;
    bool completed = false;
    bool ok = false;
    std::string message;
};

// The App owns and orchestrates all backend modules. It is a QObject that
// emits signals on state changes; signals are delivered to the GUI thread via
// Qt's queued connection mechanism (callbacks arrive on network/input threads).
class App : public QObject {
    Q_OBJECT
public:
    explicit App(QObject* parent = nullptr);
    ~App() override;

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Lifecycle (non-blocking; spawns background threads internally).
    void startServer(const AppConfig& cfg);
    void startClient(const AppConfig& cfg);
    // Automatic mode: discover a peer with the same pairing code and elect
    // roles automatically (larger random nodeId becomes the server).
    void startAuto(const AppConfig& cfg);
    void stop();
    bool isRunning() const { return running_.load(); }
    bool isServer() const { return isServer_.load(); }
    bool isConnected() const;

    // Which role was decided in auto mode (valid after connection).
    QString roleDescription() const;

    // Dynamically change the screen layout while connected (server side only).
    void setLayout(ScreenLayout layout);

    // File transfer (sender). Returns 0 on failure.
    uint64_t sendFiles(const std::vector<std::string>& paths);

    // Receiver: accept/reject an incoming offer (called from UI after prompt).
    void acceptTransfer(uint64_t id, const std::string& destDir);
    void rejectTransfer(uint64_t id);

    // Install a log sink that forwards log lines via logMessage signal.
    void installLogSink();

signals:
    void statusChanged(const QString& status);
    void connectionChanged(bool connected, const QString& peerName,
                          uint32_t peerWidth, uint32_t peerHeight);
    void roleDetermined(bool isServer);
    void transferProgress(quint64 id, quint64 transferred, quint64 total);
    void transferComplete(quint64 id, bool ok, const QString& message);
    void incomingOffer(quint64 id, const QStringList& files, quint64 total);
    void logMessage(const QString& level, const QString& line);
    void sessionStopped();
    // 对端通过 LayoutSync 推送了新的相对屏幕布局。
    void layoutChanged(ScreenLayout layout);

private:
    void resetSession();
    void setStatus(const QString& s);
    void setConnected(bool v, const std::string& name = "",
                      uint32_t w = 0, uint32_t h = 0);
    void onTransferProgress(uint64_t id, uint64_t transferred, uint64_t total);
    void onTransferComplete(uint64_t id, bool ok, const std::string& msg);
    void onTransferOffer(uint64_t id, uint64_t total);

    void wireServerCallbacks();
    void wireClientCallbacks();
    void wireFileTransferCallbacks();

    // 应用来自对端的 LayoutSync（只更新本地状态与路由器，不再回发）。
    void applyRemoteLayout(ScreenLayout layout);
    // 通过控制通道把当前 layout_ 广播给对端。
    void sendLayoutSync();

    // Internal launchers used by startServer/startClient/startAuto.
    void launchServer(const AppConfig& cfg, bool enableDiscovery);
    void launchClient(const AppConfig& cfg, const std::string& host,
                      int connectTimeoutMs = 5000);
    // Try launching a client with a short delay and a couple of retries to
    // handle the race where the server's TCP listener isn't ready immediately
    // after auto-discovery election. Runs on the calling (background) thread.
    void launchClientAuto(const AppConfig& cfg, const std::string& host);

    AppConfig config_;
    ScreenLayout layout_ = ScreenLayout::LeftOf;
    uint64_t nodeId_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> isServer_{false};
    std::atomic<bool> autoMode_{false};

    std::unique_ptr<UdpDiscovery> autoDiscovery_;
    std::unique_ptr<NetworkServer> server_;
    std::unique_ptr<NetworkClient> client_;
    std::unique_ptr<WinInputCapturer> capturer_;
    std::unique_ptr<WinInputInjector> injector_;
    std::unique_ptr<ClipboardManager> clipboard_;
    std::unique_ptr<ScreenRouter> router_;
    std::unique_ptr<FileTransferManager> fileTransfer_;
    std::unique_ptr<InputEventSender> inputSender_;

    uint32_t localW_ = 0;
    uint32_t localH_ = 0;
    std::atomic<uint32_t> clientW_{0};
    std::atomic<uint32_t> clientH_{0};

    std::atomic<bool> hasControl_{false};
    std::atomic<bool> connected_{false};
    // 正在应用对端推送的布局，避免 setLayout 再次回发造成回环。
    std::atomic<bool> applyingRemoteLayout_{false};

    // Tracks transfer IDs that originated from clipboard (auto-accept, put
    // received files into clipboard on completion).
    std::mutex clipboardTransferMutex_;
    std::unordered_map<uint64_t, std::string> clipboardTransfers_;
};

} // namespace zb
