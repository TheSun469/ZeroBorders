#pragma once

#include "AppConfig.h"

#include <string>

namespace zb {

// Loads and saves AppConfig from %APPDATA%/ZeroBorders/config.json.
// All methods are safe to call without prior existence of the file;
// load() returns a default-constructed config if none is found.
class ConfigManager {
public:
    static ConfigManager& instance();

    // Returns the absolute path to the config directory (ensured to exist).
    std::string configDir() const;
    // Returns the absolute path to config.json.
    std::string configPath() const;

    // Load configuration from disk. Returns true if a file was loaded.
    bool load(AppConfig& out) const;
    // Save configuration to disk. Returns true on success.
    bool save(const AppConfig& cfg) const;

    // Windows auto-start (HKCU\...\Run). Returns true if currently enabled.
    bool isAutoStartEnabled() const;
    // Enable or disable auto-start for the current user.
    bool setAutoStart(bool enabled) const;

private:
    ConfigManager() = default;
};

} // namespace zb
