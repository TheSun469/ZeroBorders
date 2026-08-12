#include "ConfigManager.h"
#include "../core/Log.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace zb {

using json = nlohmann::json;

ConfigManager& ConfigManager::instance() {
    static ConfigManager mgr;
    return mgr;
}

std::string ConfigManager::configDir() const {
    const char* appdata = std::getenv("APPDATA");
    std::string dir = appdata ? std::string(appdata) + "\\ZeroBorders"
                              : ".\\ZeroBorders";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        ZB_LOG_WARN("Could not create config dir {}: {}", dir, ec.message());
    }
    return dir;
}

std::string ConfigManager::configPath() const {
    return configDir() + "\\config.json";
}

bool ConfigManager::load(AppConfig& out) const {
    std::string path = configPath();
    std::ifstream f(path);
    if (!f.is_open()) {
        ZB_LOG_INFO("No config file at {}, using defaults", path);
        return false;
    }

    try {
        json j;
        f >> j;

        if (j.contains("role")) out.role = static_cast<AppConfig::Role>(j["role"].get<int>());
        if (j.contains("rolePreference")) out.rolePreference = j["rolePreference"].get<int>();
        if (j.contains("pairingCode")) out.pairingCode = j["pairingCode"].get<std::string>();
        if (j.contains("serverName")) out.serverName = j["serverName"].get<std::string>();
        if (j.contains("host")) out.host = j["host"].get<std::string>();
        if (j.contains("layout")) out.layout = j["layout"].get<std::string>();
        if (j.contains("controlPort")) out.controlPort = j["controlPort"].get<uint16_t>();
        if (j.contains("dataPort")) out.dataPort = j["dataPort"].get<uint16_t>();
        if (j.contains("udpPort")) out.udpPort = j["udpPort"].get<uint16_t>();
        if (j.contains("receiveDir")) out.receiveDir = j["receiveDir"].get<std::string>();
        if (j.contains("startMinimized")) out.startMinimized = j["startMinimized"].get<bool>();

        ZB_LOG_INFO("Loaded config from {}", path);
        return true;
    } catch (const std::exception& e) {
        ZB_LOG_ERROR("Failed to parse config {}: {}", path, e.what());
        return false;
    }
}

bool ConfigManager::save(const AppConfig& cfg) const {
    std::string path = configPath();
    try {
        json j;
        j["role"] = static_cast<int>(cfg.role);
        j["rolePreference"] = cfg.rolePreference;
        j["pairingCode"] = cfg.pairingCode;
        j["serverName"] = cfg.serverName;
        j["host"] = cfg.host;
        j["layout"] = cfg.layout;
        j["controlPort"] = cfg.controlPort;
        j["dataPort"] = cfg.dataPort;
        j["udpPort"] = cfg.udpPort;
        j["receiveDir"] = cfg.receiveDir;
        j["startMinimized"] = cfg.startMinimized;

        std::ofstream f(path);
        if (!f.is_open()) {
            ZB_LOG_ERROR("Cannot open config for writing: {}", path);
            return false;
        }
        f << j.dump(4);
        ZB_LOG_INFO("Saved config to {}", path);
        return true;
    } catch (const std::exception& e) {
        ZB_LOG_ERROR("Failed to save config {}: {}", path, e.what());
        return false;
    }
}

bool ConfigManager::isAutoStartEnabled() const {
#ifndef _WIN32
    return false;
#else
    HKEY hKey = nullptr;
    bool enabled = false;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char data[MAX_PATH] = {};
        DWORD size = sizeof(data);
        if (RegQueryValueExA(hKey, "ZeroBorders", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(data), &size) == ERROR_SUCCESS) {
            enabled = true;
        }
        RegCloseKey(hKey);
    }
    return enabled;
#endif
}

bool ConfigManager::setAutoStart(bool enabled) const {
#ifndef _WIN32
    return false;
#else
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        ZB_LOG_ERROR("Cannot open Run registry key for writing");
        return false;
    }

    bool ok = false;
    if (enabled) {
        char exePath[MAX_PATH] = {};
        DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            // Quote the path in case it contains spaces.
            std::string value = "\"" + std::string(exePath) + "\"";
            ok = RegSetValueExA(hKey, "ZeroBorders", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(value.c_str()),
                                static_cast<DWORD>(value.size() + 1)) == ERROR_SUCCESS;
        }
    } else {
        ok = RegDeleteValueA(hKey, "ZeroBorders") == ERROR_SUCCESS ||
             RegDeleteValueA(hKey, "ZeroBorders") == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(hKey);
    ZB_LOG_INFO("Auto-start {}", ok ? (enabled ? "enabled" : "disabled") : "change failed");
    return ok;
#endif
}

} // namespace zb
