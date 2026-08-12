#pragma once

#include "../core/Protocol.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace zb {

// A single framed TCP connection on either the control or data channel.
//
// Lifetime rule: the owner must NOT destroy this object from within the
// disconnect callback (which runs on the receive thread). Call close() from
// another thread, or schedule teardown asynchronously.
class TcpTransport {
public:
    using MessageCallback = std::function<void(MsgType, const std::vector<uint8_t>&)>;
    using DisconnectCallback = std::function<void()>;

    explicit TcpTransport(Channel channel);
    ~TcpTransport();

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool connect(const std::string& host, uint16_t port, int timeoutMs = 5000);
    // Adopt an already-accepted socket (server side).
    void adopt(SOCKET s);
    void start();

    void send(MsgType type, const std::vector<uint8_t>& payload);
    void sendRaw(const std::vector<uint8_t>& frame);

    void close();
    bool isConnected() const { return connected_.load(); }

    void onMessage(MessageCallback cb) { onMessage_ = std::move(cb); }
    void onDisconnect(DisconnectCallback cb) { onDisconnect_ = std::move(cb); }

    Channel channel() const { return channel_; }

private:
    void recvLoop();
    void fireDisconnect();

    Channel channel_;
    SOCKET sock_ = INVALID_SOCKET;
    std::atomic<bool> connected_{false};
    std::atomic<bool> closing_{false};
    std::atomic<bool> disconnectFired_{false};
    std::thread recvThread_;
    std::mutex sendMutex_;
    std::mutex sockMutex_;
    MessageCallback onMessage_;
    DisconnectCallback onDisconnect_;
};

} // namespace zb
