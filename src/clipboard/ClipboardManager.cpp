#include "ClipboardManager.h"
#include "../core/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>

// Ensure DROPFILES is available (may be skipped by WIN32_LEAN_AND_MEAN).
#ifndef DROPFILES
typedef struct _DROPFILES {
    DWORD pFiles;
    POINT pt;
    BOOL  fNC;
    BOOL  fWide;
} DROPFILES, *LPDROPFILES;
#endif

// CF_DIBV5 may not be defined in all SDK configurations.
#ifndef CF_DIBV5
#define CF_DIBV5 17
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>

#pragma comment(lib, "user32.lib")

namespace zb {

// WM_CLIPBOARDUPDATE is defined in modern SDKs; guard for safety.
#ifndef WM_CLIPBOARDUPDATE
#define WM_CLIPBOARDUPDATE 0x031D
#endif

namespace {

const wchar_t* kListenerWndClass = L"ZeroBordersClipboardListener";

// OpenClipboard can fail if another process is briefly holding the clipboard
// lock (common right after Ctrl+C in Explorer). Retry a few times before
// giving up, otherwise we permanently miss the change notification.
bool openClipboardWithRetry(HWND owner, int maxAttempts = 5, int delayMs = 60) {
    for (int i = 0; i < maxAttempts; ++i) {
        if (OpenClipboard(owner)) return true;
        if (i < maxAttempts - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }
    return false;
}

LRESULT CALLBACK listenerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLIPBOARDUPDATE) {
        auto* mgr = reinterpret_cast<ClipboardManager*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (mgr) {
            // Post a custom message to defer processing out of the clipboard chain callback.
            PostMessage(hwnd, WM_USER + 1, 0, 0);
        }
        return 0;
    }
    if (msg == WM_USER + 1) {
        auto* mgr = reinterpret_cast<ClipboardManager*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (mgr) {
            // Use a friend-access trick: call a static trampoline.
            // We stored the manager pointer; invoke via a helper.
            // Actually we need a public method; let's use a workaround:
            // The manager's listenerThreadFunc pumps messages and handles WM_USER+1.
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Convert UTF-16 wide string to UTF-8.
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

// Convert UTF-8 string to UTF-16 wide string.
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

} // namespace

// We need a way for the window proc to trigger clipboard reading.
// Since listenerWndProc can't call private methods, we use a simple global
// per-instance trampoline. The listener thread itself processes WM_USER+1.

ClipboardManager::ClipboardManager() = default;

ClipboardManager::~ClipboardManager() {
    stop();
}

bool ClipboardManager::start(ChangeCallback cb) {
    if (running_.load()) return false;
    callback_ = std::move(cb);
    running_ = true;
    listenerThread_ = std::thread([this] { listenerThreadFunc(); });
    return true;
}

void ClipboardManager::stop() {
    if (!running_.exchange(false)) return;
    // Post WM_QUIT to the listener thread's message loop.
    if (hwnd_) {
        PostMessage(reinterpret_cast<HWND>(hwnd_), WM_QUIT, 0, 0);
    }
    if (listenerThread_.joinable()) listenerThread_.join();
    hwnd_ = nullptr;
}

void ClipboardManager::listenerThreadFunc() {
    // Register window class.
    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = listenerWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kListenerWndClass;
    RegisterClassW(&wc);

    // Create hidden message-only window.
    HWND hwnd = CreateWindowExW(0, kListenerWndClass, L"ZB Clipboard",
                                0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hwnd) {
        ZB_LOG_ERROR("Failed to create clipboard listener window: {}", GetLastError());
        running_ = false;
        return;
    }
    hwnd_ = hwnd;
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // Register as clipboard format listener.
    if (!AddClipboardFormatListener(hwnd)) {
        ZB_LOG_ERROR("AddClipboardFormatListener failed: {}", GetLastError());
        DestroyWindow(hwnd);
        hwnd_ = nullptr;
        running_ = false;
        return;
    }

    ZB_LOG_INFO("Clipboard listener started");

    MSG msg;
    while (running_.load()) {
        // PeekMessage so we can poll running_ without blocking forever.
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            // Handle WM_USER+1 (deferred clipboard change) directly.
            if (msg.message == WM_USER + 1) {
                onChangeDetected();
            } else {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
    }

    RemoveClipboardFormatListener(hwnd);
    DestroyWindow(hwnd);
    hwnd_ = nullptr;
    ZB_LOG_INFO("Clipboard listener stopped");
}

void ClipboardManager::onChangeDetected() {
    // De-duplicate: Explorer often fires WM_CLIPBOARDUPDATE multiple times
    // while populating formats. GetClipboardSequenceNumber only changes when
    // the clipboard content actually changes.
    DWORD seq = GetClipboardSequenceNumber();
    if (seq == lastSeqNo_) {
        return; // Same content as last read.
    }

    // Debounce: wait briefly so delayed-rendering formats (especially CF_HDROP)
    // become available after the first WM_CLIPBOARDUPDATE.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    ClipboardContent content;
    if (!readLocal(content)) {
        // Retry once after a longer delay; some apps advertise formats before
        // the data is actually ready.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if (!readLocal(content)) {
            // Even if we couldn't read it, record the sequence so we don't
            // keep retrying the same content on subsequent notifications.
            lastSeqNo_ = seq;
            ZB_LOG_DEBUG("Clipboard changed but no supported content found");
            return;
        }
    }
    lastSeqNo_ = seq;

    if (isEcho(content.digest)) {
        ZB_LOG_DEBUG("Clipboard change ignored (echo of our own write)");
        return;
    }
    const char* fmtName = "unknown";
    switch (content.format) {
        case ClipboardFormat::Text:  fmtName = "text"; break;
        case ClipboardFormat::Image: fmtName = "image"; break;
        case ClipboardFormat::Files: fmtName = "files"; break;
        default: break;
    }
    ZB_LOG_INFO("Local clipboard changed: format={}, digest={:.8}",
                fmtName, tokenToHex(content.digest));
    if (callback_) {
        try {
            callback_(content);
        } catch (const std::exception& e) {
            ZB_LOG_ERROR("Clipboard change callback exception: {}", e.what());
        } catch (...) {
            ZB_LOG_ERROR("Clipboard change callback unknown exception");
        }
    }
}

void ClipboardManager::syncNow() {
    if (!running_.load() || !callback_) return;
    ClipboardContent content;
    if (!readLocal(content)) return;
    if (isEcho(content.digest)) return;
    ZB_LOG_INFO("Initial clipboard sync on connect");
    try {
        callback_(content);
    } catch (const std::exception& e) {
        ZB_LOG_ERROR("Clipboard sync callback exception: {}", e.what());
    } catch (...) {
        ZB_LOG_ERROR("Clipboard sync callback unknown exception");
    }
}

bool ClipboardManager::readLocal(ClipboardContent& out) {
    if (!openClipboardWithRetry(nullptr)) {
        ZB_LOG_WARN("OpenClipboard failed after retries: {}", GetLastError());
        return false;
    }
    bool ok = false;

    // Enumerate available formats for diagnostics when nothing matched.
    auto logAvailableFormats = [](const char* tag) {
        std::string formats;
        UINT fmt = 0;
        int count = 0;
        while ((fmt = EnumClipboardFormats(fmt)) != 0 && count < 32) {
            char name[256] = {};
            if (GetClipboardFormatNameA(fmt, name, sizeof(name))) {
                formats += name;
            } else {
                // Standard format IDs
                switch (fmt) {
                    case CF_TEXT: formats += "CF_TEXT"; break;
                    case CF_BITMAP: formats += "CF_BITMAP"; break;
                    case CF_SYLK: formats += "CF_SYLK"; break;
                    case CF_DIF: formats += "CF_DIF"; break;
                    case CF_TIFF: formats += "CF_TIFF"; break;
                    case CF_OEMTEXT: formats += "CF_OEMTEXT"; break;
                    case CF_DIB: formats += "CF_DIB"; break;
                    case CF_PALETTE: formats += "CF_PALETTE"; break;
                    case CF_PENDATA: formats += "CF_PENDATA"; break;
                    case CF_RIFF: formats += "CF_RIFF"; break;
                    case CF_WAVE: formats += "CF_WAVE"; break;
                    case CF_UNICODETEXT: formats += "CF_UNICODETEXT"; break;
                    case CF_ENHMETAFILE: formats += "CF_ENHMETAFILE"; break;
                    case CF_HDROP: formats += "CF_HDROP"; break;
                    case CF_LOCALE: formats += "CF_LOCALE"; break;
                    case CF_DIBV5: formats += "CF_DIBV5"; break;
                    default: formats += std::to_string(fmt); break;
                }
            }
            formats += " ";
            ++count;
        }
        ZB_LOG_DEBUG("Clipboard formats ({}): {}", tag, formats);
    };

    // Check file drop (CF_HDROP) FIRST. When files are copied in Explorer,
    // the clipboard often also contains CF_UNICODETEXT with path strings.
    // If we check text first, we would send plain text instead of triggering
    // an actual file transfer.
    bool hasHdrop = IsClipboardFormatAvailable(CF_HDROP) != FALSE;
    if (hasHdrop) {
        HGLOBAL hData = GetClipboardData(CF_HDROP);
        if (!hData) {
            ZB_LOG_WARN("CF_HDROP available but GetClipboardData failed: {}",
                        GetLastError());
        } else {
            auto* drop = static_cast<HDROP>(GlobalLock(hData));
            if (!drop) {
                ZB_LOG_WARN("GlobalLock for CF_HDROP failed: {}", GetLastError());
            } else {
                UINT fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                ZB_LOG_DEBUG("CF_HDROP: {} files", fileCount);
                std::vector<std::string> paths;
                for (UINT i = 0; i < fileCount; ++i) {
                    // When cchBuf=0, DragQueryFileW returns the character count
                    // NOT including the null terminator. Allocate len+1 and pass
                    // len+1 as the buffer capacity, otherwise the last character
                    // of the path gets truncated to make room for the null.
                    UINT len = DragQueryFileW(drop, i, nullptr, 0);
                    if (len == 0) continue;
                    std::wstring wpath(len + 1, L'\0');
                    UINT copied = DragQueryFileW(drop, i, wpath.data(), len + 1);
                    if (copied > 0) {
                        wpath.resize(copied); // strip trailing null
                        paths.push_back(wideToUtf8(wpath));
                    }
                }
                GlobalUnlock(hData);
                if (!paths.empty()) {
                    out.format = ClipboardFormat::Files;
                    out.filePaths = std::move(paths);
                    std::string concat;
                    for (const auto& p : out.filePaths) concat += p + ";";
                    out.digest = sha256(concat);
                    ok = true;
                } else {
                    ZB_LOG_WARN("CF_HDROP present but no file paths extracted");
                }
            }
        }
    }

    // Try DIBV5 first (preferred by modern apps), then DIB.
    UINT imgFormat = 0;
    if (IsClipboardFormatAvailable(CF_DIBV5)) {
        imgFormat = CF_DIBV5;
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        imgFormat = CF_DIB;
    }

    if (!ok && imgFormat) {
        HGLOBAL hData = GetClipboardData(imgFormat);
        if (hData) {
            auto* raw = static_cast<const uint8_t*>(GlobalLock(hData));
            SIZE_T dataSize = GlobalSize(hData);
            if (raw && dataSize >= sizeof(BITMAPINFOHEADER)) {
                out.format = ClipboardFormat::Image;
                out.pngData.assign(raw, raw + dataSize);
                auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(raw);
                out.imageWidth = bih->biWidth;
                // biHeight can be negative for top-down DIBs.
                out.imageHeight = (bih->biHeight < 0) ? -bih->biHeight : bih->biHeight;
                out.digest = sha256(std::string_view(reinterpret_cast<const char*>(out.pngData.data()), out.pngData.size()));
                ok = true;
            }
            GlobalUnlock(hData);
        }
    }

    // Try text last.
    if (!ok && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HGLOBAL hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            auto* raw = static_cast<const wchar_t*>(GlobalLock(hData));
            if (raw) {
                size_t len = 0;
                while (raw[len]) ++len;
                std::wstring w(raw, len);
                GlobalUnlock(hData);
                out.format = ClipboardFormat::Text;
                out.text = wideToUtf8(w);
                out.digest = sha256(out.text);
                ok = !out.text.empty();
            }
        }
    }

    // Log available formats if we couldn't find anything supported.
    if (!ok) {
        logAvailableFormats("unsupported");
    }

    CloseClipboard();
    return ok;
}

bool ClipboardManager::setRemoteText(const std::string& utf8) {
    std::wstring w = utf8ToWide(utf8);
    size_t byteSize = (w.size() + 1) * sizeof(wchar_t);

    HGLOBAL hData = GlobalAlloc(GMEM_MOVEABLE, byteSize);
    if (!hData) return false;

    auto* dst = static_cast<wchar_t*>(GlobalLock(hData));
    if (!dst) { GlobalFree(hData); return false; }
    std::memcpy(dst, w.data(), byteSize);
    GlobalUnlock(hData);

    TokenHash digest = sha256(utf8);

    // Record the digest BEFORE touching the clipboard so that the
    // WM_CLIPBOARDUPDATE triggered by CloseClipboard is guaranteed to be
    // recognized as our own echo by the listener thread.
    recordWrite(digest);

    if (!openClipboardWithRetry(nullptr)) { GlobalFree(hData); return false; }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hData);
    CloseClipboard();

    ZB_LOG_INFO("Remote text set to clipboard ({} chars)", w.size());
    return true;
}

bool ClipboardManager::setRemoteImage(const std::vector<uint8_t>& dibData, int w, int h) {
    if (dibData.empty()) return false;

    HGLOBAL hData = GlobalAlloc(GMEM_MOVEABLE, dibData.size());
    if (!hData) return false;

    auto* dst = static_cast<uint8_t*>(GlobalLock(hData));
    if (!dst) { GlobalFree(hData); return false; }
    std::memcpy(dst, dibData.data(), dibData.size());
    GlobalUnlock(hData);

    TokenHash digest = sha256(std::string_view(reinterpret_cast<const char*>(dibData.data()), dibData.size()));

    recordWrite(digest);

    if (!openClipboardWithRetry(nullptr)) { GlobalFree(hData); return false; }
    EmptyClipboard();

    // Detect whether the blob is a DIBV5 or plain DIB based on the header size.
    UINT format = CF_DIB;
    if (dibData.size() >= sizeof(BITMAPV5HEADER)) {
        auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dibData.data());
        if (bih->biSize >= sizeof(BITMAPV5HEADER)) {
            format = CF_DIBV5;
        }
    }
    SetClipboardData(format, hData);

    // Also set CF_DIB for apps that don't support CF_DIBV5. We need a
    // separate allocation because SetClipboardData takes ownership.
    if (format == CF_DIBV5) {
        HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, dibData.size());
        if (hDib) {
            auto* pDib = static_cast<uint8_t*>(GlobalLock(hDib));
            if (pDib) {
                std::memcpy(pDib, dibData.data(), dibData.size());
                GlobalUnlock(hDib);
                SetClipboardData(CF_DIB, hDib);
            } else {
                GlobalFree(hDib);
            }
        }
    }

    CloseClipboard();

    ZB_LOG_INFO("Remote image set to clipboard ({}x{}, {} bytes, format={})",
                w, h, dibData.size(), format == CF_DIBV5 ? "DIBV5" : "DIB");
    return true;
}

bool ClipboardManager::setRemoteFiles(const std::vector<std::string>& paths) {
    if (paths.empty()) return false;

    // Build DROPFILES structure with wide strings
    std::wstring allPaths;
    for (const auto& p : paths) {
        allPaths += utf8ToWide(p);
        allPaths += L'\0';
    }
    allPaths += L'\0'; // double null terminator

    size_t totalSize = sizeof(DROPFILES) + allPaths.size() * sizeof(wchar_t);
    HGLOBAL hData = GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (!hData) return false;

    auto* drop = static_cast<DROPFILES*>(GlobalLock(hData));
    if (!drop) { GlobalFree(hData); return false; }
    std::memset(drop, 0, sizeof(DROPFILES));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES),
                allPaths.data(), allPaths.size() * sizeof(wchar_t));
    GlobalUnlock(hData);

    // Digest for echo prevention
    std::string concat;
    for (const auto& p : paths) concat += p + ";";
    TokenHash digest = sha256(concat);

    recordWrite(digest);

    if (!openClipboardWithRetry(nullptr)) { GlobalFree(hData); return false; }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, hData);
    CloseClipboard();

    ZB_LOG_INFO("Remote files set to clipboard ({} files)", paths.size());
    return true;
}

bool ClipboardManager::isEcho(const TokenHash& digest) const {
    size_t count = echoCacheFull_ ? kEchoCacheSize : echoCacheIdx_;
    for (size_t i = 0; i < count; ++i) {
        if (echoCache_[i] == digest) return true;
    }
    return false;
}

void ClipboardManager::recordWrite(const TokenHash& digest) {
    echoCache_[echoCacheIdx_] = digest;
    echoCacheIdx_ = (echoCacheIdx_ + 1) % kEchoCacheSize;
    if (echoCacheIdx_ == 0) echoCacheFull_ = true;
}

} // namespace zb
