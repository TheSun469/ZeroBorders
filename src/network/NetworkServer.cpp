#include "NetworkServer.h"
#include "../core/Log.h"

// Platform.h (pulled in via NetworkServer.h -> TcpTransport.h) provides
// winsock2.h/ws2tcpip.h on Windows and the POSIX socket headers +
// closesocket/WSAGetLastError/errno mappings on Linux. Here we only add the
// platform-specific extras. GetSystemMetrics is replaced by Qt's
// QGuiApplication::primaryScreen() so <windows.h> is no longer needed.
#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/select.h>  // select() on Linux requires this header
#include <fcntl.h>        // fcntl, O_NONBLOCK
#endif

#include <QGuiApplication>
#include <QScreen>

#include <chrono>

namespace zb {

NetworkServer::NetworkServer(TokenHash token, std::string name,
                             uint16_t ctrlPort, uint16_t dataPort, uint16_t udpPort)
    : token_(token), name_(std::move(name)),
      ctrlPort_(ctrlPort), dataPort_(dataPort), udpPort_(udpPort) {}

NetworkServer::~NetworkServer() {
    stop();
}

socket_t NetworkServer::createListener(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ZB_LOG_ERROR("bind() port {} failed: {}", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (listen(s, 1) == SOCKET_ERROR) {
        ZB_LOG_ERROR("listen() port {} failed: {}", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }

    // Set the listening socket to non-blocking so accept loops can poll with
    // select() and a timeout.
#ifdef _WIN32
    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
    return s;
}

void NetworkServer::start(ReadyCallback onReady, DisconnectCallback onDisconnect,
                          bool enableDiscovery) {
    onReady_ = std::move(onReady);
    onDisconnect_ = std::move(onDisconnect);
    discoveryEnabled_ = enableDiscovery;

    ctrlListener_ = createListener(ctrlPort_);
    dataListener_ = createListener(dataPort_);
    if (ctrlListener_ == INVALID_SOCKET || dataListener_ == INVALID_SOCKET) {
        teardownImpl("控制/数据端口监听失败");
        return;
    }

    running_ = true;
    if (enableDiscovery) {
        discovery_ = std::make_unique<UdpDiscovery>();
        discovery_->startServer(udpPort_, token_, name_);
    }

    ctrlAcceptThread_ = std::thread([this] { acceptControlLoop(); });
    dataAcceptThread_ = std::thread([this] { acceptDataLoop(); });

    ZB_LOG_INFO("Server listening on control {} / data {}", ctrlPort_, dataPort_);
}

void NetworkServer::stop() {
    if (!running_.exchange(false)) {
        // still clean up sockets/threads if partially up
    }
    if (ctrlListener_ != INVALID_SOCKET) {
        closesocket(ctrlListener_);
        ctrlListener_ = INVALID_SOCKET;
    }
    if (dataListener_ != INVALID_SOCKET) {
        closesocket(dataListener_);
        dataListener_ = INVALID_SOCKET;
    }
    if (discovery_) {
        discovery_->stop();
        discovery_.reset();
    }
    // If a teardown is already in progress on the cleanup thread, wait for it
    // to finish BEFORE running our own. Otherwise we would race on the
    // transport pointers and fire onDisconnect_ twice (the cleanup thread's
    // teardownImpl fires it once for the real session loss; the one below
    // sees ready_==false and skips it).
    if (cleanupThread_.joinable()) cleanupThread_.join();
    // Run a final synchronous teardown on the calling thread (never a recv
    // thread). tearingDown_ is false here because the cleanup thread resets
    // it on exit; set it to guard against re-entry during onDisconnect_.
    tearingDown_ = true;
    teardownImpl("正在停止");
    tearingDown_ = false;
    if (ctrlAcceptThread_.joinable()) ctrlAcceptThread_.join();
    if (dataAcceptThread_.joinable()) dataAcceptThread_.join();
    ready_ = false;
}

void NetworkServer::acceptControlLoop() {
    while (running_.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(ctrlListener_, &readSet);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        // select()'s first argument is ignored on Windows but must be the
        // highest fd + 1 on Linux. Passing fd+1 works on both platforms.
        int rc = select(static_cast<int>(ctrlListener_) + 1, &readSet, nullptr, nullptr, &tv);
        if (rc <= 0) continue;

        sockaddr_in client{};
        socklen_t len = sizeof(client);
        socket_t clientSock = accept(ctrlListener_, reinterpret_cast<sockaddr*>(&client), &len);
        if (clientSock == INVALID_SOCKET) continue;

        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        ZB_LOG_INFO("Control connection from {}", ip);

        std::lock_guard<std::mutex> lk(stateMutex_);
        if (ctrl_) {
            // Only one client at a time; reject subsequent connections.
            closesocket(clientSock);
            continue;
        }
        ctrlState_ = CtrlState::WaitingHello;
        ctrl_ = std::make_shared<TcpTransport>(Channel::Control);
        ctrl_->adopt(clientSock);
        ctrl_->onDisconnect([this] {
            postTeardown("控制通道断开");
        });
        ctrl_->onMessage([this](MsgType t, const std::vector<uint8_t>& p) {
            handleControl(t, p);
        });
        ctrl_->start();
    }
}

void NetworkServer::acceptDataLoop() {
    while (running_.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(dataListener_, &readSet);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        // select()'s first argument is ignored on Windows but must be the
        // highest fd + 1 on Linux. Passing fd+1 works on both platforms.
        int rc = select(static_cast<int>(dataListener_) + 1, &readSet, nullptr, nullptr, &tv);
        if (rc <= 0) continue;

        sockaddr_in client{};
        socklen_t len = sizeof(client);
        socket_t clientSock = accept(dataListener_, reinterpret_cast<sockaddr*>(&client), &len);
        if (clientSock == INVALID_SOCKET) continue;

        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        ZB_LOG_INFO("Data connection from {}", ip);

        std::lock_guard<std::mutex> lk(stateMutex_);
        if (data_) {
            closesocket(clientSock);
            continue;
        }
        dataState_ = DataState::WaitingHello;
        data_ = std::make_shared<TcpTransport>(Channel::Data);
        data_->adopt(clientSock);
        data_->onDisconnect([this] {
            postTeardown("数据通道断开");
        });
        data_->onMessage([this](MsgType t, const std::vector<uint8_t>& p) {
            handleData(t, p);
        });
        data_->start();
    }
}

void NetworkServer::handleControl(MsgType t, const std::vector<uint8_t>& p) {
    if (ctrlState_ == CtrlState::WaitingHello) {
        if (t != MsgType::Hello) {
            ZB_LOG_WARN("Expected Hello, got {}", static_cast<int>(t));
            postTeardown("握手错误");
            return;
        }
        HelloMsg hello;
        if (!parseHello(p, hello) || hello.version != kProtocolVersion) {
            WelcomeMsg wb{};
            wb.result = 2;
            ctrl_->send(MsgType::Welcome, serializeWelcome(wb));
            postTeardown("版本不兼容");
            return;
        }
        if (hello.token != token_) {
            WelcomeMsg wb{};
            wb.result = 1;
            ctrl_->send(MsgType::Welcome, serializeWelcome(wb));
            postTeardown("识别码错误");
            return;
        }

        WelcomeMsg wb{};
        wb.result = 0;
        // Use Qt to obtain the primary screen size instead of the Windows-only
        // GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN).
        QScreen* screen = QGuiApplication::primaryScreen();
        QSize screenSize = screen ? screen->size() : QSize(0, 0);
        wb.screenWidth = static_cast<uint32_t>(screenSize.width());
        wb.screenHeight = static_cast<uint32_t>(screenSize.height());
        wb.capabilities = kCurrentCapabilities;
        ctrl_->send(MsgType::Welcome, serializeWelcome(wb));
        ctrlState_ = CtrlState::Ready;
        ZB_LOG_INFO("Client authenticated ({}x{})", wb.screenWidth, wb.screenHeight);
        checkReady();
        return;
    }

    if (t == MsgType::Ping) {
        uint64_t ts = 0;
        if (parsePingPong(p, ts)) {
            ctrl_->send(MsgType::Pong, serializePingPong(ts));
        }
        return;
    }
    if (t == MsgType::Goodbye) {
        postTeardown("对端主动断开");
        return;
    }
    if (userCtrl_) userCtrl_(t, p);
}

void NetworkServer::handleData(MsgType t, const std::vector<uint8_t>& p) {
    if (dataState_ == DataState::WaitingHello) {
        if (t != MsgType::DataHello) {
            ZB_LOG_WARN("Expected DataHello, got {}", static_cast<int>(t));
            postTeardown("数据握手错误");
            return;
        }
        DataHelloMsg dh;
        if (!parseDataHello(p, dh) || dh.version != kProtocolVersion || dh.token != token_) {
            postTeardown("数据握手验证失败");
            return;
        }
        dataState_ = DataState::Ready;
        ZB_LOG_INFO("Data channel authenticated");
        checkReady();
        return;
    }
    if (userData_) userData_(t, p);
}

void NetworkServer::checkReady() {
    bool expected = false;
    if (ctrlState_ == CtrlState::Ready && dataState_ == DataState::Ready &&
        ready_.compare_exchange_strong(expected, true)) {
        if (discovery_) {
            discovery_->stop();
            discovery_.reset();
        }
        ZB_LOG_INFO("Both channels ready - session active");
        if (onReady_) onReady_();
    }
}

void NetworkServer::postTeardown(const std::string& reason) {
    bool expected = false;
    if (!tearingDown_.compare_exchange_strong(expected, true)) return;
    if (cleanupThread_.joinable()) cleanupThread_.join();
    cleanupThread_ = std::thread([this, reason] {
        teardownImpl(reason);
        tearingDown_ = false;
    });
}

void NetworkServer::restartDiscovery() {
    // Stop any existing discovery first.
    if (discovery_) {
        discovery_->stop();
        discovery_.reset();
    }
    // Start broadcasting again so peers can find us after a disconnect.
    discovery_ = std::make_unique<UdpDiscovery>();
    discovery_->startServer(udpPort_, token_, name_);
    ZB_LOG_INFO("UDP discovery restarted (waiting for peer to reconnect)");
}

void NetworkServer::teardownImpl(const std::string& reason) {
    bool wasReady = ready_.exchange(false);
    ZB_LOG_INFO("Server teardown: {} (wasReady={})", reason, wasReady);

    std::shared_ptr<TcpTransport> ctrl;
    std::shared_ptr<TcpTransport> data;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        ctrl.swap(ctrl_);
        data.swap(data_);
    }
    // Close outside the lock; close() joins the recv thread, which must never
    // be the current thread because we always arrive here via postTeardown or
    // stop() (never directly from a transport callback).
    if (ctrl) ctrl->close();
    if (data) data->close();
    ctrl.reset();
    data.reset();

    // Reset to WaitingHello so the accept loops can immediately accept new
    // connections. The accept loops re-set these when a connection arrives.
    ctrlState_ = CtrlState::WaitingHello;
    dataState_ = DataState::WaitingHello;

    // Only notify the application when a real session was lost. This avoids
    // double-firing onDisconnect_ when stop() runs a final teardown after
    // the cleanup thread already handled the disconnect, and avoids
    // notifying on handshake failures (which the accept loops recover from
    // automatically).
    if (wasReady && onDisconnect_) onDisconnect_(reason);

    // If the server is still running (not being stopped by the user) and
    // discovery was enabled, restart UDP broadcast so the peer can discover
    // us again and reconnect automatically. This makes the non-disconnected
    // side silently wait for reconnection without user intervention.
    if (running_.load() && discoveryEnabled_ && !discovery_) {
        restartDiscovery();
    }
}

bool NetworkServer::sendControl(MsgType t, const std::vector<uint8_t>& p) {
    // Copy the transport pointer under the lock, then release the lock before
    // calling send(). send() may fail and trigger fireDisconnect() -> postTeardown(),
    // which joins the cleanup thread; doing that while holding stateMutex_
    // would deadlock if the cleanup thread is itself waiting on stateMutex_
    // inside teardownImpl().
    std::shared_ptr<TcpTransport> ctrl;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        ctrl = ctrl_;
    }
    if (ctrl) return ctrl->send(t, p);
    return false;
}

bool NetworkServer::sendData(MsgType t, const std::vector<uint8_t>& p) {
    std::shared_ptr<TcpTransport> data;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        data = data_;
    }
    if (data) return data->send(t, p);
    return false;
}

} // namespace zb
