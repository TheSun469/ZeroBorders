#include "NetworkClient.h"
#include "../core/Log.h"

#include <chrono>

namespace zb {

NetworkClient::NetworkClient(TokenHash token, uint16_t ctrlPort, uint16_t dataPort, uint16_t udpPort)
    : token_(token), ctrlPort_(ctrlPort), dataPort_(dataPort), udpPort_(udpPort) {}

NetworkClient::~NetworkClient() {
    stop();
}

bool NetworkClient::start(ReadyCallback onReady, DisconnectCallback onDisconnect,
                          int discoveryTimeoutMs) {
    onReady_ = std::move(onReady);
    onDisconnect_ = std::move(onDisconnect);
    running_ = true;

    discovery_ = std::make_unique<UdpDiscovery>();
    std::string foundHost;
    std::mutex mtx;
    std::condition_variable cv;
    bool found = false;

    discovery_->startClient(udpPort_, token_, [&](const std::string& ip) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!found) { found = true; foundHost = ip; }
        }
        cv.notify_all();
    });

    std::unique_lock<std::mutex> lk(mtx);
    bool ok = cv.wait_for(lk, std::chrono::milliseconds(discoveryTimeoutMs),
                          [&] { return found; });
    discovery_->stop();
    discovery_.reset();

    if (!ok) {
        running_ = false;
        ZB_LOG_ERROR("No server found on the LAN within {} ms", discoveryTimeoutMs);
        return false;
    }

    ZB_LOG_INFO("Discovered server at {}", foundHost);
    lastHost_ = foundHost;
    return connectInternal(foundHost, onReady_, onDisconnect_, 5000);
}

bool NetworkClient::connectToHost(const std::string& host, ReadyCallback onReady,
                                  DisconnectCallback onDisconnect, int timeoutMs) {
    onReady_ = std::move(onReady);
    onDisconnect_ = std::move(onDisconnect);
    running_ = true;
    lastHost_ = host;
    return connectInternal(host, onReady_, onDisconnect_, timeoutMs);
}

bool NetworkClient::connectInternal(const std::string& host, ReadyCallback onReady,
                                    DisconnectCallback onDisconnect, int timeoutMs) {
    if (onReady) onReady_ = std::move(onReady);
    if (onDisconnect) onDisconnect_ = std::move(onDisconnect);

    if (!tryConnect(host, timeoutMs)) {
        running_ = false;
        if (onDisconnect_) onDisconnect_("无法建立连接");
        return false;
    }
    return true;
}

bool NetworkClient::tryConnect(const std::string& host, int timeoutMs) {
    // Tear down any stale transports from a previous attempt (silently).
    auto cleanupPartial = [this]() {
        std::shared_ptr<TcpTransport> ctrl, data;
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            ctrl.swap(ctrl_);
            data.swap(data_);
        }
        if (ctrl) ctrl->close();
        if (data) data->close();
        ctrl.reset();
        data.reset();
        ctrlState_ = CtrlState::Closed;
        dataState_ = DataState::Closed;
        ready_ = false;
    };

    auto ctrl = std::make_shared<TcpTransport>(Channel::Control);
    if (!ctrl->connect(host, ctrlPort_, timeoutMs)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(handshakeMutex_);
        welcomeReceived_ = false;
    }

    ctrl_ = ctrl;
    ctrlState_ = CtrlState::WaitingWelcome;
    ctrl->onMessage([this](MsgType t, const std::vector<uint8_t>& p) { handleControl(t, p); });
    ctrl->onDisconnect([this] {
        postTeardown("控制通道断开", running_.load());
    });
    ctrl->start();

    HelloMsg hello{};
    hello.version = kProtocolVersion;
    hello.capabilities = kCurrentCapabilities;
    hello.token = token_;
    ctrl->send(MsgType::Hello, serializeHello(hello));

    // Wait for Welcome
    {
        std::unique_lock<std::mutex> lk(handshakeMutex_);
        if (!handshakeCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                   [this] { return welcomeReceived_; })) {
            cleanupPartial();
            return false;
        }
        if (welcome_.result != 0) {
            cleanupPartial();
            return false;
        }
    }

    // Establish data channel
    auto data = std::make_shared<TcpTransport>(Channel::Data);
    if (!data->connect(host, dataPort_, timeoutMs)) {
        cleanupPartial();
        return false;
    }
    data_ = data;
    dataState_ = DataState::Ready;
    data->onMessage([this](MsgType t, const std::vector<uint8_t>& p) { handleData(t, p); });
    data->onDisconnect([this] {
        postTeardown("数据通道断开", running_.load());
    });
    data->start();

    DataHelloMsg dh{};
    dh.version = kProtocolVersion;
    dh.token = token_;
    data->send(MsgType::DataHello, serializeDataHello(dh));

    ready_ = true;
    reconnectDelaySec_ = 1;
    ZB_LOG_INFO("Connected to {} ({}x{})", host, welcome_.screenWidth, welcome_.screenHeight);
    if (onReady_) onReady_(welcome_);
    return true;
}

void NetworkClient::handleControl(MsgType t, const std::vector<uint8_t>& p) {
    if (ctrlState_ == CtrlState::WaitingWelcome) {
        if (t == MsgType::Welcome) {
            WelcomeMsg wb;
            if (parseWelcome(p, wb)) {
                {
                    std::lock_guard<std::mutex> lk(handshakeMutex_);
                    welcome_ = wb;
                    welcomeReceived_ = true;
                }
                handshakeCv_.notify_all();
                if (wb.result == 0) {
                    ctrlState_ = CtrlState::Ready;
                }
            }
            return;
        }
        ZB_LOG_WARN("Expected Welcome, got {}", static_cast<int>(t));
        return;
    }

    if (t == MsgType::Ping) {
        uint64_t ts = 0;
        if (parsePingPong(p, ts)) {
            sendControl(MsgType::Pong, serializePingPong(ts));
        }
        return;
    }
    if (t == MsgType::Goodbye) {
        postTeardown("对端主动断开", false);
        return;
    }
    if (userCtrl_) userCtrl_(t, p);
}

void NetworkClient::handleData(MsgType t, const std::vector<uint8_t>& p) {
    if (userData_) userData_(t, p);
}

void NetworkClient::stop() {
    running_ = false;
    reconnecting_ = false;
    ready_ = false;
    if (cleanupThread_.joinable()) cleanupThread_.join();
    teardownImpl("stopping", false);
    if (reconnectThread_.joinable()) reconnectThread_.join();
}

void NetworkClient::postTeardown(const std::string& reason, bool retry) {
    bool expected = false;
    if (!tearingDown_.compare_exchange_strong(expected, true)) return;
    if (cleanupThread_.joinable()) cleanupThread_.join();
    cleanupThread_ = std::thread([this, reason, retry] {
        teardownImpl(reason, retry);
        tearingDown_ = false;
    });
}

void NetworkClient::teardownImpl(const std::string& reason, bool retry) {
    if (ready_.exchange(false)) {
        ZB_LOG_WARN("Session lost: {}", reason);
    } else {
        ZB_LOG_INFO("Client teardown: {}", reason);
    }

    std::shared_ptr<TcpTransport> ctrl;
    std::shared_ptr<TcpTransport> data;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        ctrl.swap(ctrl_);
        data.swap(data_);
    }
    if (ctrl) ctrl->close();
    if (data) data->close();
    ctrl.reset();
    data.reset();

    ctrlState_ = CtrlState::Closed;
    dataState_ = DataState::Closed;

    if (onDisconnect_) onDisconnect_(reason);

    if (retry && running_.load() && !reconnecting_.exchange(true)) {
        scheduleReconnect();
    }
}

void NetworkClient::scheduleReconnect() {
    if (reconnectThread_.joinable()) reconnectThread_.join();
    reconnectThread_ = std::thread([this] {
        // Try direct reconnect to last known host with exponential backoff.
        while (running_.load() && !ready_.load()) {
            ZB_LOG_INFO("Reconnecting to {} in {}s...", lastHost_, reconnectDelaySec_);
            std::this_thread::sleep_for(std::chrono::seconds(reconnectDelaySec_));
            if (!running_.load()) break;
            if (tryConnect(lastHost_, 5000)) {
                reconnecting_ = false;
                return;
            }
            reconnectDelaySec_ = std::min(reconnectDelaySec_ * 2, 30);
        }
        reconnecting_ = false;
    });
}

void NetworkClient::sendControl(MsgType t, const std::vector<uint8_t>& p) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (ctrl_) ctrl_->send(t, p);
}

void NetworkClient::sendData(MsgType t, const std::vector<uint8_t>& p) {
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (data_) data_->send(t, p);
}

} // namespace zb
