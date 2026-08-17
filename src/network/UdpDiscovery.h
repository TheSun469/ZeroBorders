#pragma once

#include "../core/Types.h"
#include "../core/Platform.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Platform.h defines SOCKET_ERROR_VALUE but not SOCKET_ERROR on Linux; provide
// it here so the existing implementation can keep using the same macro.
// On Windows SOCKET_ERROR comes from winsock2.h (via Platform.h).
#ifndef SOCKET_ERROR
#define SOCKET_ERROR (-1)
#endif

namespace zb {

// LAN peer discovery via UDP broadcast.
//
// Server mode: broadcasts an announcement every 2 seconds.
// Client mode: listens for announcements and reports matching servers.
// Auto mode: both sides broadcast and listen; roles are elected by comparing
//            random 64-bit node IDs (the larger ID becomes the server).
class UdpDiscovery {
public:
    enum class AutoRole { Server, Client };
    using FoundCallback = std::function<void(const std::string& peerIp)>;
    using AutoFoundCallback = std::function<void(const std::string& peerIp, AutoRole role)>;

    UdpDiscovery();
    ~UdpDiscovery();

    UdpDiscovery(const UdpDiscovery&) = delete;
    UdpDiscovery& operator=(const UdpDiscovery&) = delete;

    void startServer(uint16_t port, const TokenHash& token, const std::string& name,
                     FoundCallback onClientResponse = nullptr);
    void startClient(uint16_t port, const TokenHash& token, FoundCallback onServerFound);

    // Symmetric discovery: every node both broadcasts and listens on the same
    // port. Role preference: 0=auto, 1=prefer server/controller, 2=prefer client.
    // Election rules:
    //   - If one prefers server and other doesn't insist on server → server pref wins
    //   - Otherwise fall back to larger nodeId becoming server.
    void startAuto(uint16_t port, const TokenHash& token, uint64_t nodeId,
                   int rolePref, AutoFoundCallback onFound);
    void stop();

private:
    void run();

    std::atomic<bool> running_{false};
    std::thread thread_;
    socket_t sock_ = INVALID_SOCKET;
};

} // namespace zb
