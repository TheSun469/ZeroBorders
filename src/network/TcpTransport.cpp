#include "TcpTransport.h"
#include "../core/Log.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#else
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif

#include <algorithm>

namespace zb {

TcpTransport::TcpTransport(Channel channel) : channel_(channel) {}

TcpTransport::~TcpTransport() {
    close();
}

bool TcpTransport::connect(const std::string& host, uint16_t port, int timeoutMs) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        ZB_LOG_WARN("socket() failed: {}", WSAGetLastError());
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ZB_LOG_WARN("Invalid host address: {}", host);
        closesocket(s);
        return false;
    }

    // Non-blocking connect with timeout.
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);

    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            ZB_LOG_WARN("connect() failed: {}", err);
            closesocket(s);
            return false;
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(s, &writeSet);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
        rc = select(0, nullptr, &writeSet, nullptr, &tv);
        if (rc <= 0) {
            ZB_LOG_WARN("connect() timed out or failed: rc={}, err={}", rc, WSAGetLastError());
            closesocket(s);
            return false;
        }
        // Verify the connection actually succeeded.
        int sockErr = 0;
        int sockErrLen = sizeof(sockErr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sockErr), &sockErrLen) < 0
            || sockErr != 0) {
            ZB_LOG_WARN("connect() SO_ERROR: {}", sockErr);
            closesocket(s);
            return false;
        }
    }

    // Restore blocking mode.
    mode = 0;
    ioctlsocket(s, FIONBIO, &mode);

    // Disable Nagle for low-latency control channel; data channel can keep it.
    int flag = (channel_ == Channel::Control) ? 1 : 0;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    // Enable keep-alive.
    BOOL keepAlive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));
    tcp_keepalive ka{};
    ka.onoff = 1;
    ka.keepalivetime = 10000;
    ka.keepaliveinterval = 3000;
    DWORD ret = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &ret, nullptr, nullptr);

    sock_ = s;
    connected_ = true;
    closing_ = false;
    disconnectFired_ = false;
    return true;
}

void TcpTransport::adopt(SOCKET s) {
    // Ensure the accepted socket is blocking (the listener may be non-blocking).
    u_long mode = 0;
    ioctlsocket(s, FIONBIO, &mode);

    sock_ = s;
    connected_ = true;
    closing_ = false;
    disconnectFired_ = false;

    int flag = (channel_ == Channel::Control) ? 1 : 0;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
}

void TcpTransport::start() {
    recvThread_ = std::thread([this] { recvLoop(); });
}

bool TcpTransport::send(MsgType type, const std::vector<uint8_t>& payload) {
    return sendRaw(buildFrame(channel_, type, payload));
}

bool TcpTransport::sendRaw(const std::vector<uint8_t>& frame) {
    if (!connected_.load()) return false;
    std::lock_guard<std::mutex> lk(sendMutex_);
    // Re-check connected_ after acquiring the lock in case disconnect fired
    // while we were waiting.
    if (!connected_.load() || sock_ == INVALID_SOCKET) return false;
    size_t total = 0;
    while (total < frame.size()) {
        int n = ::send(sock_, reinterpret_cast<const char*>(frame.data() + total),
                       static_cast<int>(frame.size() - total), 0);
        if (n == SOCKET_ERROR) {
            ZB_LOG_WARN("send() failed: {}", WSAGetLastError());
            fireDisconnect();
            return false;
        }
        if (n == 0) {
            fireDisconnect();
            return false;
        }
        total += static_cast<size_t>(n);
    }
    return true;
}

void TcpTransport::close() {
    closing_ = true;

    SOCKET s;
    {
        std::lock_guard<std::mutex> lk(sockMutex_);
        s = sock_;
        sock_ = INVALID_SOCKET;
    }
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }

    // Only join the receive thread if we are NOT running on it ourselves.
    if (recvThread_.joinable()) {
        if (std::this_thread::get_id() != recvThread_.get_id()) {
            recvThread_.join();
        }
        // If called from within recvLoop, the thread is still finishing;
        // the destructor MUST NOT run from that thread (owner contract).
    }
}

void TcpTransport::fireDisconnect() {
    bool expected = false;
    if (!disconnectFired_.compare_exchange_strong(expected, true)) return;

    connected_ = false;

    // Close the socket from this thread to unblock recv(); do NOT join here
    // because we may be running on recvThread_ itself.
    SOCKET s;
    {
        std::lock_guard<std::mutex> lk(sockMutex_);
        s = sock_;
        sock_ = INVALID_SOCKET;
    }
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }

    if (onDisconnect_) onDisconnect_();
}

void TcpTransport::recvLoop() {
    FrameParser parser(channel_);
    parser.onFrame([this](MsgType t, const std::vector<uint8_t>& p) {
        if (!onMessage_) return;
        try {
            onMessage_(t, p);
        } catch (const std::exception& e) {
            ZB_LOG_ERROR("Uncaught exception in message callback on {} channel: {}",
                         channel_ == Channel::Control ? "control" : "data", e.what());
        } catch (...) {
            ZB_LOG_ERROR("Unknown exception in message callback on {} channel",
                         channel_ == Channel::Control ? "control" : "data");
        }
    });

    std::vector<uint8_t> buf(64 * 1024);
    while (connected_.load() && !closing_.load()) {
        int n = ::recv(sock_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
        if (n > 0) {
            try {
                parser.feed(buf.data(), static_cast<size_t>(n));
            } catch (const std::exception& e) {
                ZB_LOG_ERROR("Exception while parsing frame on {} channel: {}",
                             channel_ == Channel::Control ? "control" : "data", e.what());
                break;
            } catch (...) {
                ZB_LOG_ERROR("Unknown exception while parsing frame on {} channel",
                             channel_ == Channel::Control ? "control" : "data");
                break;
            }
        } else if (n == 0) {
            ZB_LOG_INFO("Connection closed by peer ({} channel)",
                        channel_ == Channel::Control ? "control" : "data");
            break;
        } else {
            int err = WSAGetLastError();
            if (err == WSAEINTR || err == WSAEWOULDBLOCK) continue;
            ZB_LOG_INFO("recv() error on {} channel: {}",
                        channel_ == Channel::Control ? "control" : "data", err);
            break;
        }
    }

    fireDisconnect();
}

} // namespace zb
