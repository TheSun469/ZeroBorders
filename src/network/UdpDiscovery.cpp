#include "UdpDiscovery.h"
#include "../core/Hash.h"
#include "../core/Log.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <vector>

#ifdef _WIN32
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace zb {

namespace {

// Cross-platform helper: set SO_RCVTIMEO. Windows accepts milliseconds in a
// DWORD; POSIX requires a struct timeval.
inline void setRcvTimeout(socket_t s, int timeoutMs) {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif
}

// Enumerate all active IPv4 interfaces and compute their directed broadcast
// addresses (e.g. 192.168.1.255).  Sending to 255.255.255.255 alone is
// unreliable on multi-homed Windows machines because the limited broadcast
// only egresses through one interface.
std::vector<in_addr> collectBroadcastAddresses() {
    std::vector<in_addr> result;

    // Always include the limited broadcast.
    in_addr limited{};
    limited.s_addr = htonl(INADDR_BROADCAST);
    result.push_back(limited);

#ifdef _WIN32
    ULONG bufLen = 16 * 1024;
    std::vector<uint8_t> buf(bufLen);
    auto pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    DWORD ret = GetAdaptersInfo(pAdapter, &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
        ret = GetAdaptersInfo(pAdapter, &bufLen);
    }
    if (ret != NO_ERROR) {
        ZB_LOG_WARN("GetAdaptersInfo failed: {}, using 255.255.255.255 only", ret);
        return result;
    }

    for (auto adapter = pAdapter; adapter; adapter = adapter->Next) {
        if (adapter->Type != MIB_IF_TYPE_ETHERNET && adapter->Type != IF_TYPE_IEEE80211)
            continue;
        for (auto ip = &adapter->IpAddressList; ip; ip = ip->Next) {
            in_addr ipAddr{};
            in_addr mask{};
            inet_pton(AF_INET, ip->IpAddress.String, &ipAddr);
            inet_pton(AF_INET, ip->IpMask.String, &mask);
            // Skip loopback and zero-address
            if (ipAddr.s_addr == 0 || (ipAddr.s_addr & htonl(0xFF000000)) == htonl(0x7F000000))
                continue;
            // Directed broadcast = (ip & mask) | ~mask
            in_addr bcast{};
            bcast.s_addr = (ipAddr.s_addr & mask.s_addr) | ~mask.s_addr;
            // Avoid adding duplicates
            bool dup = false;
            for (auto& a : result) {
                if (a.s_addr == bcast.s_addr) { dup = true; break; }
            }
            if (!dup) result.push_back(bcast);
        }
    }
#else
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) {
        ZB_LOG_WARN("getifaddrs failed: {}, using 255.255.255.255 only", errno);
        return result;
    }

    for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        // Skip interfaces that are down or loopback.
        if ((ifa->ifa_flags & IFF_UP) == 0) continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK)) continue;
        if ((ifa->ifa_flags & IFF_BROADCAST) == 0) continue;

        auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        in_addr ipAddr = sa->sin_addr;
        if (ipAddr.s_addr == 0) continue;
        // Skip 127.x.x.x
        if ((ipAddr.s_addr & htonl(0xFF000000)) == htonl(0x7F000000)) continue;

        in_addr bcast{};
        if (ifa->ifa_broadaddr) {
            bcast = reinterpret_cast<sockaddr_in*>(ifa->ifa_broadaddr)->sin_addr;
        } else if (ifa->ifa_netmask) {
            in_addr mask = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask)->sin_addr;
            bcast.s_addr = (ipAddr.s_addr & mask.s_addr) | ~mask.s_addr;
        } else {
            continue;
        }

        bool dup = false;
        for (auto& a : result) {
            if (a.s_addr == bcast.s_addr) { dup = true; break; }
        }
        if (!dup) result.push_back(bcast);
    }
    freeifaddrs(ifap);
#endif

    ZB_LOG_INFO("Broadcast targets:");
    for (auto& a : result) {
        char s[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &a, s, sizeof(s));
        ZB_LOG_INFO("  {}", s);
    }

    return result;
}

} // anonymous namespace

UdpDiscovery::UdpDiscovery() = default;

UdpDiscovery::~UdpDiscovery() {
    stop();
}

void UdpDiscovery::startServer(uint16_t port, const TokenHash& token,
                               const std::string& name, FoundCallback onClientResponse) {
    stop();

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        ZB_LOG_ERROR("UDP socket() failed: {}", WSAGetLastError());
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ZB_LOG_ERROR("UDP bind() failed: {}", WSAGetLastError());
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return;
    }

    DWORD timeoutMs = 1000;
    setRcvTimeout(sock_, static_cast<int>(timeoutMs));

    const std::string tokenHex = tokenToHex(token);
    const std::string announce = nlohmann::json{
        {"magic", "ZB_DISC"},
        {"role", "server"},
        {"token_hash", tokenHex},
        {"server_name", name}
    }.dump();

    BOOL bcastFlag = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcastFlag), sizeof(bcastFlag));

    auto bcastAddrs = collectBroadcastAddresses();
    std::vector<sockaddr_in> targets;
    for (auto& ba : bcastAddrs) {
        sockaddr_in t{};
        t.sin_family = AF_INET;
        t.sin_port = htons(port);
        t.sin_addr = ba;
        targets.push_back(t);
    }

    running_ = true;
    thread_ = std::thread([this, announce, targets, onClientResponse] {
        auto nextAnnounce = std::chrono::steady_clock::now();
        while (running_.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now >= nextAnnounce) {
                for (auto& t : targets) {
                    sendto(sock_, announce.c_str(), static_cast<int>(announce.size()), 0,
                           reinterpret_cast<const sockaddr*>(&t), sizeof(t));
                }
                nextAnnounce = now + std::chrono::seconds(2);
            }

            char rbuf[1024];
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int n = recvfrom(sock_, rbuf, sizeof(rbuf) - 1, 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n > 0) {
                rbuf[n] = '\0';
                try {
                    auto j = nlohmann::json::parse(rbuf);
                    if (j.value("magic", "") == "ZB_DISC" && j.value("role", "") == "client") {
                        char ip[INET_ADDRSTRLEN]{};
                        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                        if (onClientResponse) onClientResponse(ip);
                    }
                } catch (...) {
                    // ignore malformed packets
                }
            }
        }
    });
}

void UdpDiscovery::startClient(uint16_t port, const TokenHash& token, FoundCallback onServerFound) {
    stop();

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        ZB_LOG_ERROR("UDP socket() failed: {}", WSAGetLastError());
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ZB_LOG_ERROR("UDP bind() failed: {}", WSAGetLastError());
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return;
    }

    DWORD timeoutMs = 2000;
    setRcvTimeout(sock_, static_cast<int>(timeoutMs));

    const std::string tokenHex = tokenToHex(token);
    const std::string response = nlohmann::json{
        {"magic", "ZB_DISC"},
        {"role", "client"},
        {"token_hash", tokenHex}
    }.dump();

    running_ = true;
    thread_ = std::thread([this, tokenHex, response, onServerFound] {
        while (running_.load()) {
            char rbuf[1024];
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int n = recvfrom(sock_, rbuf, sizeof(rbuf) - 1, 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) continue;
            rbuf[n] = '\0';
            try {
                auto j = nlohmann::json::parse(rbuf);
                if (j.value("magic", "") != "ZB_DISC") continue;
                if (j.value("role", "") != "server") continue;
                if (j.value("token_hash", "") != tokenHex) continue;

                // Unicast a response so the server knows we're here
                sendto(sock_, response.c_str(), static_cast<int>(response.size()), 0,
                       reinterpret_cast<const sockaddr*>(&from), fromLen);

                char ip[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                if (onServerFound) {
                    onServerFound(ip);
                }
                running_ = false;
                return;
            } catch (...) {
                // ignore malformed packets
            }
        }
    });
}

void UdpDiscovery::startAuto(uint16_t port, const TokenHash& token,
                             uint64_t nodeId, int rolePref,
                             AutoFoundCallback onFound) {
    stop();

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        ZB_LOG_ERROR("UDP socket() failed: {}", WSAGetLastError());
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ZB_LOG_ERROR("UDP bind() failed: {}", WSAGetLastError());
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return;
    }

    DWORD timeoutMs = 500;
    setRcvTimeout(sock_, static_cast<int>(timeoutMs));

    BOOL bcastFlag = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcastFlag), sizeof(bcastFlag));

    const std::string tokenHex = tokenToHex(token);
    const std::string announce = nlohmann::json{
        {"magic", "ZB_DISC"},
        {"role", "auto"},
        {"token_hash", tokenHex},
        {"node_id", nodeId},
        {"role_pref", rolePref}
    }.dump();

    ZB_LOG_INFO("UDP discovery started on port {}, nodeId={}, rolePref={}", port, nodeId, rolePref);

    auto bcastAddrs = collectBroadcastAddresses();
    std::vector<sockaddr_in> targets;
    for (auto& ba : bcastAddrs) {
        sockaddr_in t{};
        t.sin_family = AF_INET;
        t.sin_port = htons(port);
        t.sin_addr = ba;
        targets.push_back(t);
    }

    running_ = true;
    thread_ = std::thread([this, announce, targets, tokenHex, nodeId, rolePref, onFound] {
        auto nextAnnounce = std::chrono::steady_clock::now();
        auto foundTime = std::chrono::steady_clock::time_point{};
        uint64_t foundPeerId = 0;
        bool found = false;
        int announceCount = 0;

        while (running_.load()) {
            auto now = std::chrono::steady_clock::now();

            // After finding a peer, keep broadcasting for a grace period so
            // the other side also receives our packet (avoids the race where
            // one side closes its socket before the other side hears it).
            if (found) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - foundTime).count();
                if (elapsed >= 3000) {
                    ZB_LOG_INFO("Discovery grace period ended, exiting");
                    running_ = false;
                    return;
                }
            }

            if (now >= nextAnnounce) {
                for (auto& t : targets) {
                    int sent = sendto(sock_, announce.c_str(), static_cast<int>(announce.size()), 0,
                                      reinterpret_cast<const sockaddr*>(&t), sizeof(t));
                    if (sent == SOCKET_ERROR) {
                        char ip[INET_ADDRSTRLEN]{};
                        inet_ntop(AF_INET, &t.sin_addr, ip, sizeof(ip));
                        ZB_LOG_WARN("sendto {} failed: err={}", ip, WSAGetLastError());
                    }
                }
                ++announceCount;
                if (!found && (announceCount <= 3 || announceCount % 5 == 0)) {
                    ZB_LOG_INFO("Discovery broadcast #{} sent to {} address(es)", announceCount, targets.size());
                }
                // After found, broadcast faster (every 200ms) for the grace period
                nextAnnounce = found ? now + std::chrono::milliseconds(200)
                                     : now + std::chrono::seconds(1);
            }

            // Use shorter timeout when in grace period so we can exit promptly
            DWORD tv = found ? 200 : 500;
            setRcvTimeout(sock_, static_cast<int>(tv));

            char rbuf[1024];
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int n = recvfrom(sock_, rbuf, sizeof(rbuf) - 1, 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) continue;
            rbuf[n] = '\0';
            try {
                auto j = nlohmann::json::parse(rbuf);
                if (j.value("magic", "") != "ZB_DISC") continue;
                if (j.value("role", "") != "auto") continue;
                if (j.value("token_hash", "") != tokenHex) {
                    if (!found) {
                        ZB_LOG_WARN("Received discovery packet with mismatched token (different pairing code?)");
                    }
                    continue;
                }

                uint64_t peerId = 0;
                auto it = j.find("node_id");
                if (it == j.end() || !it->is_number_unsigned()) {
                    continue;
                }
                peerId = it->get<uint64_t>();
                if (peerId == nodeId) continue; // loopback of our own broadcast

                // If we already found this peer, keep broadcasting in grace period
                if (found && peerId == foundPeerId) {
                    continue;
                }

                int peerPref = j.value("role_pref", 0);

                char ip[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

                // Election:
                // 1=prefer server, 2=prefer client, 0=auto
                bool iAmServer;
                if (rolePref == 1 && peerPref != 1) {
                    iAmServer = true;
                } else if (rolePref == 2 && peerPref == 1) {
                    iAmServer = false;
                } else if (rolePref == 1 && peerPref == 2) {
                    iAmServer = true;
                } else if (rolePref == 2 && peerPref != 2) {
                    iAmServer = false;
                } else {
                    iAmServer = (peerId < nodeId);
                }

                AutoRole role = iAmServer ? AutoRole::Server : AutoRole::Client;
                ZB_LOG_INFO("Auto-discovered peer {} (peerId={}, selfId={}, pref={}/{}); elected {}",
                            ip, peerId, nodeId, rolePref, peerPref,
                            role == AutoRole::Server ? "server" : "client");

                found = true;
                foundPeerId = peerId;
                foundTime = std::chrono::steady_clock::now();

                if (onFound) onFound(ip, role);

                // Do NOT return here — continue broadcasting for the grace
                // period so the other side also receives our announcement.
            } catch (...) {
                // ignore malformed packets
            }
        }
        ZB_LOG_INFO("Discovery thread exiting");
    });
}

void UdpDiscovery::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

} // namespace zb
