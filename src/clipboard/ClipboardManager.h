#pragma once

#include "../core/Hash.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace zb {

enum class ClipboardFormat : uint8_t {
    None   = 0,
    Text   = 1,
    Image  = 2,  // Raw DIB/DIBV5 clipboard data
    Files  = 3,  // File drop (CF_HDROP)
};

struct ClipboardContent {
    ClipboardFormat format = ClipboardFormat::None;
    std::string text;              // UTF-8 text (when format == Text)
    std::vector<uint8_t> pngData;  // Raw DIB/DIBV5 bytes (when format == Image)
    int imageWidth = 0;
    int imageHeight = 0;
    std::vector<std::string> filePaths; // UTF-8 paths (when format == Files)
    TokenHash digest{};            // SHA-256 of content for echo prevention
};

class ClipboardManager {
public:
    using ChangeCallback = std::function<void(const ClipboardContent&)>;

    ClipboardManager();
    ~ClipboardManager();

    // Start listening on a dedicated hidden window thread.
    bool start(ChangeCallback cb);
    void stop();

    // Read the current local clipboard and fire the callback as if a change
    // just occurred. Used to sync clipboard state when a new connection is
    // established.
    void syncNow();

    // Write remote content to local clipboard (with echo prevention).
    bool setRemoteText(const std::string& utf8);
    bool setRemoteImage(const std::vector<uint8_t>& dibData, int w, int h);
    bool setRemoteFiles(const std::vector<std::string>& paths);

    // Read current clipboard content (returns false if empty/unsupported).
    bool readLocal(ClipboardContent& out);

private:
    void listenerThreadFunc();
    void onChangeDetected();

    // Echo prevention: check if digest matches a recent write we made.
    bool isEcho(const TokenHash& digest) const;
    void recordWrite(const TokenHash& digest);

    ChangeCallback callback_;

    std::thread listenerThread_;
    std::atomic<bool> running_{false};

    // Hidden window for clipboard listener.
    void* hwnd_ = nullptr;  // HWND

    // Last processed clipboard sequence number (for de-duplicating rapid
    // WM_CLIPBOARDUPDATE notifications that fire while formats are populated).
    uint32_t lastSeqNo_ = 0;

    // Echo prevention: ring buffer of recent write digests.
    static constexpr size_t kEchoCacheSize = 4;
    TokenHash echoCache_[kEchoCacheSize]{};
    size_t echoCacheIdx_ = 0;
    bool echoCacheFull_ = false;
};

} // namespace zb
