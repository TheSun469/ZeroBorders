#pragma once

#include <string>

namespace zb {

// Persistent application configuration loaded/saved as JSON.
struct AppConfig {
    // Which role this instance runs as.
    enum class Role : int { Server = 0, Client = 1 };
    Role role = Role::Server;

    // Role preference for auto-discovery: 0=auto, 1=prefer controller, 2=prefer controlled.
    int rolePreference = 0;

    // Shared pairing code (plaintext locally; only hash goes over network).
    std::string pairingCode = "test123";

    // Username as secondary authentication factor (combined with pairing code
    // to derive the token hash, preventing same-code collisions in LAN).
    std::string username = "user";

    // Server display name (hostname used when empty).
    std::string serverName;

    // Client: direct connect host (empty = use UDP discovery).
    std::string host;

    // Screen layout: "right_of", "left_of", "above", "below".
    std::string layout = "left_of";

    // Network ports.
    uint16_t controlPort = 24801;
    uint16_t dataPort = 24802;
    uint16_t udpPort = 24800;

    // Directory where received files are stored. Empty = auto (TEMP first).
    std::string receiveDir;

    // True if the window should start minimized to tray (Phase 7, stored now).
    bool startMinimized = false;
};

} // namespace zb
