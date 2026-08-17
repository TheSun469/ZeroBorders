#pragma once

#include "TcpTransport.h"
#include "UdpDiscovery.h"
#include "../core/Types.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace zb {

class NetworkServer {
public:
    using ReadyCallback = std::function<void()>;
    using DisconnectCallback = std::function<void(const std::string& reason)>;
    using MessageCallback = std::function<void(MsgType, const std::vector<uint8_t>&)>;

    NetworkServer(TokenHash token, std::string name,
                  uint16_t ctrlPort = kDefaultControlPort,
                  uint16_t dataPort = kDefaultDataPort,
                  uint16_t udpPort = kDefaultUdpPort);
    ~NetworkServer();

    NetworkServer(const NetworkServer&) = delete;
    NetworkServer& operator=(const NetworkServer&) = delete;

    void start(ReadyCallback onReady, DisconnectCallback onDisconnect,
               bool enableDiscovery = true);
    void stop();

    bool isReady() const { return ready_.load(); }

    bool sendControl(MsgType t, const std::vector<uint8_t>& p);
    bool sendData(MsgType t, const std::vector<uint8_t>& p);

    void onControlMessage(MessageCallback cb) { userCtrl_ = std::move(cb); }
    void onDataMessage(MessageCallback cb) { userData_ = std::move(cb); }

    // Restart UDP broadcast so peers can rediscover us after a disconnect.
    // Called by App when running in auto mode (where the initial launch
    // disables server-side discovery because autoDiscovery_ was already
    // active). Without this, the server stays in TCP-listen-only mode and
    // a restarted peer cannot find it via UDP.
    void restartDiscovery();

private:
    enum class CtrlState { WaitingHello, Ready, Closed };
    enum class DataState { WaitingHello, Ready, Closed };

    void acceptControlLoop();
    void acceptDataLoop();
    void handleControl(MsgType t, const std::vector<uint8_t>& p);
    void handleData(MsgType t, const std::vector<uint8_t>& p);
    void checkReady();
    void postTeardown(const std::string& reason);
    void teardownImpl(const std::string& reason);

    static socket_t createListener(uint16_t port);

    TokenHash token_;
    std::string name_;
    uint16_t ctrlPort_;
    uint16_t dataPort_;
    uint16_t udpPort_;
    bool discoveryEnabled_ = true;

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> tearingDown_{false};
    std::atomic<CtrlState> ctrlState_{CtrlState::WaitingHello};
    std::atomic<DataState> dataState_{DataState::WaitingHello};

    socket_t ctrlListener_ = INVALID_SOCKET;
    socket_t dataListener_ = INVALID_SOCKET;
    std::thread ctrlAcceptThread_;
    std::thread dataAcceptThread_;
    std::thread cleanupThread_;

    std::unique_ptr<UdpDiscovery> discovery_;
    std::shared_ptr<TcpTransport> ctrl_;
    std::shared_ptr<TcpTransport> data_;

    ReadyCallback onReady_;
    DisconnectCallback onDisconnect_;
    MessageCallback userCtrl_;
    MessageCallback userData_;

    std::mutex stateMutex_;
};

} // namespace zb
