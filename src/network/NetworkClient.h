#pragma once

#include "TcpTransport.h"
#include "UdpDiscovery.h"
#include "../core/Types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace zb {

class NetworkClient {
public:
    using ReadyCallback = std::function<void(const WelcomeMsg&)>;
    using DisconnectCallback = std::function<void(const std::string& reason)>;
    using MessageCallback = std::function<void(MsgType, const std::vector<uint8_t>&)>;

    NetworkClient(TokenHash token,
                  uint16_t ctrlPort = kDefaultControlPort,
                  uint16_t dataPort = kDefaultDataPort,
                  uint16_t udpPort = kDefaultUdpPort);
    ~NetworkClient();

    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;

    // Discover server automatically, then connect. Returns false on failure.
    bool start(ReadyCallback onReady, DisconnectCallback onDisconnect,
               int discoveryTimeoutMs = 15000);
    // Connect directly to a known host (skips UDP discovery).
    bool connectToHost(const std::string& host, ReadyCallback onReady,
                       DisconnectCallback onDisconnect, int timeoutMs = 5000);
    void stop();

    bool isReady() const { return ready_.load(); }

    void sendControl(MsgType t, const std::vector<uint8_t>& p);
    void sendData(MsgType t, const std::vector<uint8_t>& p);

    void onControlMessage(MessageCallback cb) { userCtrl_ = std::move(cb); }
    void onDataMessage(MessageCallback cb) { userData_ = std::move(cb); }

private:
    enum class CtrlState { WaitingWelcome, Ready, Closed };
    enum class DataState { Idle, Ready, Closed };

    bool connectInternal(const std::string& host, ReadyCallback onReady,
                         DisconnectCallback onDisconnect, int timeoutMs);
    bool tryConnect(const std::string& host, int timeoutMs);
    void handleControl(MsgType t, const std::vector<uint8_t>& p);
    void handleData(MsgType t, const std::vector<uint8_t>& p);
    void scheduleReconnect();
    void postTeardown(const std::string& reason, bool retry);
    void teardownImpl(const std::string& reason, bool retry);

    TokenHash token_;
    uint16_t ctrlPort_;
    uint16_t dataPort_;
    uint16_t udpPort_;

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> tearingDown_{false};
    std::atomic<CtrlState> ctrlState_{CtrlState::WaitingWelcome};
    std::atomic<DataState> dataState_{DataState::Idle};

    std::unique_ptr<UdpDiscovery> discovery_;
    std::shared_ptr<TcpTransport> ctrl_;
    std::shared_ptr<TcpTransport> data_;

    ReadyCallback onReady_;
    DisconnectCallback onDisconnect_;
    MessageCallback userCtrl_;
    MessageCallback userData_;

    std::mutex stateMutex_;
    std::mutex handshakeMutex_;
    std::condition_variable handshakeCv_;
    bool welcomeReceived_ = false;
    WelcomeMsg welcome_{};

    std::thread reconnectThread_;
    std::thread cleanupThread_;
    std::atomic<bool> reconnecting_{false};
    std::string lastHost_;
    int reconnectDelaySec_ = 1;

    // Allows stop() to break the reconnect thread out of its backoff wait
    // immediately instead of blocking for up to 30 seconds on join().
    std::mutex reconnectMtx_;
    std::condition_variable reconnectCv_;
};

} // namespace zb
