#pragma once

#include "../core/Hash.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QImage>
#include <QUrl>

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
    Image  = 2,  // Image data (DIB on Windows, PNG on Linux)
    Files  = 3,  // File drop (CF_HDROP on Windows, URI list on Linux)
};

struct ClipboardContent {
    ClipboardFormat format = ClipboardFormat::None;
    std::string text;              // UTF-8 text (when format == Text)
    std::vector<uint8_t> pngData;  // PNG bytes (Linux) or raw DIB/DIBV5 bytes (Windows)
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

    // Start listening on a dedicated hidden window thread (Windows) or
    // via QClipboard signals (Linux).
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
    // Linux note: must be called from the GUI (main) thread because QClipboard
    // is not thread-safe.
    bool readLocal(ClipboardContent& out);

private:
#ifdef _WIN32
    void listenerThreadFunc();
    void onChangeDetected();
#else
    void onClipboardChanged();
#endif

    // Echo prevention: check if digest matches a recent write we made.
    bool isEcho(const TokenHash& digest) const;
    void recordWrite(const TokenHash& digest);

    ChangeCallback callback_;
    std::atomic<bool> running_{false};

#ifdef _WIN32
    std::thread listenerThread_;

    // Hidden window for clipboard listener.
    void* hwnd_ = nullptr;  // HWND

    // Last processed clipboard sequence number (for de-duplicating rapid
    // WM_CLIPBOARDUPDATE notifications that fire while formats are populated).
    uint32_t lastSeqNo_ = 0;
#else
    // QClipboard instance obtained from QGuiApplication. Accessed only on the
    // GUI thread (QClipboard is not thread-safe).
    QClipboard* clipboard_ = nullptr;
#endif

    // Last digest sent to the peer. Windows fires multiple WM_CLIPBOARDUPDATE
    // events for a single copy operation (as different formats are populated
    // via delayed rendering). Without this check, the same file(s) would be
    // sent multiple times.
    TokenHash lastSentDigest_{};

    // Echo prevention: ring buffer of recent write digests.
    static constexpr size_t kEchoCacheSize = 4;
    TokenHash echoCache_[kEchoCacheSize]{};
    size_t echoCacheIdx_ = 0;
    bool echoCacheFull_ = false;
};

} // namespace zb
