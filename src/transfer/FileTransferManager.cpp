#include "FileTransferManager.h"
#include "../core/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

namespace zb {

namespace {

// On Windows, std::filesystem::path and std::ifstream treat narrow
// std::string paths as the system ANSI code page, not UTF-8. All paths
// flowing through this class are UTF-8 (from the clipboard / network),
// so we must convert to UTF-16 before touching the filesystem.
std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                  static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

// Construct a fs::path from a UTF-8 narrow string (correct on Windows).
fs::path pathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(utf8ToWide(utf8));
#else
    return fs::path(utf8);
#endif
}

// Convert a fs::path to a UTF-8 narrow string.
std::string pathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return wideToUtf8(p.wstring());
#else
    return p.string();
#endif
}

} // namespace

FileTransferManager::FileTransferManager() = default;

FileTransferManager::~FileTransferManager() {
    cancelAll();
}

void FileTransferManager::cancelAll() {
    // Signal all senders to stop and wake flow-control waits. Move threads
    // out and join them BEFORE destroying the SenderState objects so that no
    // thread is waiting on a condition_variable that gets destroyed.
    std::vector<std::thread> threadsToJoin;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [id, sender] : senders_) {
            sender.active = false;
            sender.ackCv.notify_all();
        }
        for (auto& [id, sender] : senders_) {
            if (sender.thread.joinable()) {
                threadsToJoin.push_back(std::move(sender.thread));
            }
        }
        for (auto& [id, recv] : receivers_) {
            for (auto& f : recv.files) {
                if (f.is_open()) f.close();
            }
        }
        receivers_.clear();
    }
    for (auto& t : threadsToJoin) {
        t.join();
    }
    // Now that no sender thread is running, it is safe to destroy states.
    std::lock_guard<std::mutex> lk(mutex_);
    senders_.clear();
}

void FileTransferManager::cleanupFinishedSenders() {
    // Must be called with mutex_ held.
    for (auto it = senders_.begin(); it != senders_.end();) {
        if (it->second.finished.load() && it->second.thread.joinable()) {
            it->second.thread.join();
            it = senders_.erase(it);
        } else {
            ++it;
        }
    }
}

bool FileTransferManager::buildEntries(const std::vector<std::string>& paths,
                                        std::vector<TransferEntry>& entries,
                                        std::vector<std::string>& entryPaths,
                                        uint64_t& totalSize) {
    totalSize = 0;
    for (const auto& p : paths) {
        fs::path fp = pathFromUtf8(p);
        std::error_code ec;
        if (!fs::exists(fp, ec)) {
            ZB_LOG_WARN("File not found: {}", p);
            continue;
        }

        if (fs::is_directory(fp, ec)) {
            // Recursive: add directory entry then all contents with relative paths.
            std::string dirName = pathToUtf8(fp.filename());
            entries.push_back({dirName, 0, true});
            entryPaths.push_back(pathToUtf8(fp));

            fs::recursive_directory_iterator it(fp, fs::directory_options::skip_permission_denied, ec);
            if (ec) {
                ZB_LOG_WARN("Cannot iterate directory {}: {}", p, ec.message());
                continue;
            }
            for (const auto& entry : it) {
                fs::path relPath = fs::relative(entry.path(), fp, ec);
                if (ec) continue;
                std::string rel = dirName + "/" + pathToUtf8(relPath);
                if (entry.is_directory(ec)) {
                    entries.push_back({rel, 0, true});
                    entryPaths.push_back(pathToUtf8(entry.path()));
                } else if (entry.is_regular_file(ec)) {
                    uint64_t sz = static_cast<uint64_t>(entry.file_size(ec));
                    entries.push_back({rel, sz, false});
                    entryPaths.push_back(pathToUtf8(entry.path()));
                    totalSize += sz;
                }
            }
        } else if (fs::is_regular_file(fp, ec)) {
            uint64_t sz = static_cast<uint64_t>(fs::file_size(fp, ec));
            std::string name = pathToUtf8(fp.filename());
            entries.push_back({name, sz, false});
            entryPaths.push_back(p);
            totalSize += sz;
        }
    }
    return !entries.empty();
}

uint64_t FileTransferManager::sendFiles(const std::vector<std::string>& localPaths,
                                        uint8_t flags,
                                        const std::string& destDir) {
    std::vector<TransferEntry> entries;
    std::vector<std::string> entryPaths;
    uint64_t totalSize = 0;
    if (!buildEntries(localPaths, entries, entryPaths, totalSize)) {
        ZB_LOG_ERROR("No valid files to send");
        return 0;
    }

    uint64_t id = nextId();

    FileOfferMsg offer;
    offer.transferId = id;
    offer.entryCount = static_cast<uint32_t>(entries.size());
    offer.totalSize = totalSize;
    offer.entries = entries;
    offer.flags = flags;
    offer.destDir = destDir;

    // Store sender state for when accept arrives.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cleanupFinishedSenders();
        auto& s = senders_[id];
        s.entries = entries;
        s.entryPaths = entryPaths;
        s.active = false; // Will activate on accept.
        s.finished = false;
        s.ackedBytes = 0;
        auto now = std::chrono::steady_clock::now();
        s.offerTime = now;
        s.lastAckTime = now;
    }

    ZB_LOG_INFO("Sending file offer: id={}, {} entries, {} bytes total",
                id, entries.size(), totalSize);

    if (sendCb_) {
        if (!sendCb_(MsgType::FileOffer, serializeFileOffer(offer))) {
            ZB_LOG_ERROR("Failed to send file offer for id={}", id);
            std::lock_guard<std::mutex> lk(mutex_);
            senders_.erase(id);
            if (completeCb_) completeCb_(id, false, "连接已断开，无法发送文件请求");
            return 0;
        }
    }

    if (offerCb_) offerCb_(id, entries, totalSize, destDir, flags);

    // Start a detached watchdog that cancels the transfer if the receiver
    // never responds with FileAccept within the timeout. Without this the
    // sender would wait forever when the peer is unresponsive.
    std::thread([this, id]() {
        std::this_thread::sleep_for(std::chrono::seconds(kAcceptTimeoutSec));
        bool shouldFail = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = senders_.find(id);
            if (it != senders_.end() && !it->second.active.load() &&
                !it->second.finished.load()) {
                shouldFail = true;
            }
        }
        if (shouldFail) {
            ZB_LOG_WARN("Transfer {} timed out waiting for accept", id);
            {
                std::lock_guard<std::mutex> lk(mutex_);
                senders_.erase(id);
            }
            if (completeCb_) completeCb_(id, false, "等待对端接收超时");
        }
    }).detach();

    return id;
}

void FileTransferManager::accept(uint64_t id, const std::string& destDir) {
    std::vector<TransferEntry> entries;
    uint64_t total = 0;
    uint8_t flags = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pendingOffers_.find(id);
        if (it != pendingOffers_.end()) {
            entries = std::move(it->second.entries);
            total = it->second.totalSize;
            flags = it->second.flags;
            pendingOffers_.erase(it);
        }
    }

    if (!entries.empty()) {
        std::string dir = destDir;
        if (dir.empty()) dir = defaultReceiveDir_;

        // Validate that the directory is writable. System directories like
        // C:\Users require elevation and will silently fail all file creation.
        std::error_code ec;
        fs::path dirPath;
        if (!dir.empty()) {
            dirPath = pathFromUtf8(dir);
            fs::create_directories(dirPath, ec);
            // Probe writability by creating and removing a temp file.
            if (!ec) {
                auto probe = dirPath / L".zb_write_test";
                std::ofstream test(probe, std::ios::binary);
                bool writable = test.is_open();
                test.close();
                if (writable) {
                    std::error_code rmEc;
                    fs::remove(probe, rmEc);
                } else {
                    ZB_LOG_WARN("Receive dir {} is not writable, falling back", dir);
                    dir.clear();
                }
            } else {
                ZB_LOG_WARN("Cannot create receive dir {}: {}, falling back",
                            dir, ec.message());
                dir.clear();
                ec.clear();
            }
        }

        if (dir.empty()) {
#ifdef _WIN32
            // Use the wide-char API so the path is always correct regardless
            // of the ANSI code page (std::getenv returns ANSI).
            wchar_t wtemp[MAX_PATH]{};
            DWORD n = GetEnvironmentVariableW(L"TEMP", wtemp, MAX_PATH);
            if (n > 0 && n < MAX_PATH) {
                dir = wideToUtf8(wtemp) + "\\ZeroBorders";
            }
#else
            const char* temp = std::getenv("TEMP");
            if (temp) dir = std::string(temp) + "/ZeroBorders";
#endif
            dirPath = pathFromUtf8(dir);
            fs::create_directories(dirPath, ec);
            if (ec) {
                ZB_LOG_ERROR("Cannot create fallback receive dir {}: {}", dir, ec.message());
                reject(id);
                return;
            }
        }
        ZB_LOG_INFO("Using receive directory: {}", dir);

        int openFailures = 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto& recv = receivers_[id];
            recv.destDir = dir;
            recv.entries = entries;
            recv.receivedBytes.resize(entries.size(), 0);
            recv.files.resize(entries.size());
            recv.totalSize = total;
            recv.totalReceived = 0;
            recv.flags = flags;
        }

        for (uint32_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            std::string sanitized = sanitizeFilename(e.relativePath);
            if (e.isDirectory) {
                fs::path entryDir = dirPath / pathFromUtf8(sanitized);
                fs::create_directories(entryDir, ec);
                if (ec) ZB_LOG_WARN("Cannot create dir {}: {}",
                                    pathToUtf8(entryDir), ec.message());
            } else {
                fs::path filePath = dirPath / pathFromUtf8(sanitized);
                fs::create_directories(filePath.parent_path(), ec);
                std::string resolved = resolveCollision(pathToUtf8(filePath));

                std::lock_guard<std::mutex> lk(mutex_);
                auto it2 = receivers_.find(id);
                if (it2 != receivers_.end()) {
#ifdef _WIN32
                    it2->second.files[i].open(pathFromUtf8(resolved),
                                              std::ios::binary | std::ios::trunc);
#else
                    it2->second.files[i].open(resolved,
                                              std::ios::binary | std::ios::trunc);
#endif
                    if (!it2->second.files[i].is_open()) {
                        ZB_LOG_ERROR("Cannot create file: {}", resolved);
                        ++openFailures;
                    }
                }
            }
        }

        // If every file failed to open, reject the transfer instead of
        // pretending it succeeded.
        int fileCount = 0;
        for (const auto& e : entries) if (!e.isDirectory) ++fileCount;
        if (fileCount > 0 && openFailures == fileCount) {
            ZB_LOG_ERROR("All {} files failed to create, rejecting transfer {}", fileCount, id);
            {
                std::lock_guard<std::mutex> lk(mutex_);
                receivers_.erase(id);
            }
            FileAcceptMsg rej;
            rej.transferId = id;
            rej.result = 1;
            rej.destDir = "无法在接收目录创建文件";
            if (sendCb_) sendCb_(MsgType::FileAccept, serializeFileAccept(rej));
            return;
        }
    }

    FileAcceptMsg msg;
    msg.transferId = id;
    msg.result = 0;
    msg.destDir = destDir;

    ZB_LOG_INFO("Accepting transfer {} -> {}", id, destDir);

    if (sendCb_) {
        sendCb_(MsgType::FileAccept, serializeFileAccept(msg));
    }
}

void FileTransferManager::reject(uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pendingOffers_.erase(id);
        receivers_.erase(id);
    }

    FileAcceptMsg msg;
    msg.transferId = id;
    msg.result = 1;

    ZB_LOG_INFO("Rejecting transfer {}", id);

    if (sendCb_) {
        sendCb_(MsgType::FileAccept, serializeFileAccept(msg));
    }
}

void FileTransferManager::handleMessage(MsgType type, const std::vector<uint8_t>& payload) {
    switch (type) {
        case MsgType::FileOffer: {
            FileOfferMsg msg;
            if (!parseFileOffer(payload, msg)) return;

            ZB_LOG_INFO("Received file offer: id={}, {} entries, {} bytes, flags={}, destDir={}",
                        msg.transferId, msg.entryCount, msg.totalSize, msg.flags,
                        msg.destDir.empty() ? "(default)" : msg.destDir);

            try {
                if (incomingOfferCb_) {
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        pendingOffers_[msg.transferId] = {
                            std::move(msg.entries), msg.totalSize, msg.flags,
                            std::move(msg.destDir)};
                    }
                    const std::vector<TransferEntry>* view = nullptr;
                    std::string offerDest;
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        auto it = pendingOffers_.find(msg.transferId);
                        if (it != pendingOffers_.end()) {
                            view = &it->second.entries;
                            offerDest = it->second.destDir;
                        }
                    }
                    if (view) incomingOfferCb_(msg.transferId, *view, msg.totalSize,
                                               offerDest, msg.flags);
                    return;
                }

                // No incoming callback — auto-accept. Use the offer's destDir
                // if provided, otherwise fall back to default/temp.
                std::string destDir = msg.destDir.empty() ? defaultReceiveDir_ : msg.destDir;
                std::error_code ec;
                if (!destDir.empty()) {
                    fs::create_directories(pathFromUtf8(destDir), ec);
                }
                if (destDir.empty() || ec) {
                    ec.clear();
#ifdef _WIN32
                    wchar_t wbuf[MAX_PATH]{};
                    if (GetEnvironmentVariableW(L"TEMP", wbuf, MAX_PATH) > 0) {
                        destDir = wideToUtf8(wbuf) + "\\ZeroBorders";
                        fs::create_directories(pathFromUtf8(destDir), ec);
                    }
                    if (ec || destDir.empty()) {
                        ec.clear();
                        if (GetEnvironmentVariableW(L"USERPROFILE", wbuf, MAX_PATH) > 0) {
                            destDir = wideToUtf8(wbuf) + "\\Downloads\\ZeroBorders";
                            fs::create_directories(pathFromUtf8(destDir), ec);
                        }
                    }
#else
                    const char* temp = std::getenv("TEMP");
                    const char* home = std::getenv("USERPROFILE");
                    if (temp) {
                        destDir = std::string(temp) + "/ZeroBorders";
                        fs::create_directories(destDir, ec);
                    }
                    if (ec || destDir.empty()) {
                        ec.clear();
                        if (home) {
                            destDir = std::string(home) + "/Downloads/ZeroBorders";
                            fs::create_directories(destDir, ec);
                        }
                    }
#endif
                    if (ec || destDir.empty()) {
                        ec.clear();
                        destDir = ".\\ZeroBorders_Received";
                        fs::create_directories(pathFromUtf8(destDir), ec);
                    }
                }
                if (ec) {
                    ZB_LOG_ERROR("Cannot create any dest dir: {}", ec.message());
                    FileAcceptMsg rej;
                    rej.transferId = msg.transferId;
                    rej.result = 1;
                    rej.destDir = "cannot create receive directory";
                    if (sendCb_) sendCb_(MsgType::FileAccept, serializeFileAccept(rej));
                    return;
                }

                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    pendingOffers_[msg.transferId] = {
                        std::move(msg.entries), msg.totalSize, msg.flags, destDir};
                }
                accept(msg.transferId, destDir);

                if (offerCb_) offerCb_(msg.transferId, {}, msg.totalSize, destDir, msg.flags);
            } catch (const std::exception& e) {
                ZB_LOG_ERROR("Exception while processing file offer: {}", e.what());
            }
            break;
        }

        case MsgType::FileAccept: {
            FileAcceptMsg msg;
            if (!parseFileAccept(payload, msg)) return;

            if (msg.result != 0) {
                ZB_LOG_WARN("Transfer {} rejected by receiver", msg.transferId);
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    cleanupFinishedSenders();
                    senders_.erase(msg.transferId);
                }
                if (completeCb_) completeCb_(msg.transferId, false, "对端拒绝接收");
                return;
            }

            // Start sender thread.
            std::vector<TransferEntry> entries;
            std::vector<std::string> entryPaths;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                cleanupFinishedSenders();
                auto it = senders_.find(msg.transferId);
                if (it == senders_.end()) return;
                entries = it->second.entries;
                entryPaths = it->second.entryPaths;
                it->second.active = true;
                it->second.finished = false;

                ZB_LOG_INFO("Transfer {} accepted, starting sender thread", msg.transferId);

                it->second.thread = std::thread([this, msg, entries, entryPaths, destDir = msg.destDir] {
                    senderThread(msg.transferId, entries, entryPaths, destDir);
                });
            }
            break;
        }

        case MsgType::FileChunk: {
            FileChunkMsg msg;
            if (!parseFileChunk(payload, msg)) return;
            handleFileChunk(msg);
            break;
        }

        case MsgType::FileChunkAck: {
            FileChunkAckMsg msg;
            if (!parseFileChunkAck(payload, msg)) return;
            // Update the sender's flow-control window and wake the sender
            // thread if it is waiting for room to send more chunks.
            SenderState* s = nullptr;
            uint64_t cumulativeAcked = 0;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                auto it = senders_.find(msg.transferId);
                if (it != senders_.end()) {
                    s = &it->second;
                    // Compute transfer-wide acked bytes: sum of completed
                    // (preceding) entries plus the current entry's offset.
                    for (uint32_t i = 0; i < msg.entryIndex && i < s->entries.size(); ++i) {
                        if (!s->entries[i].isDirectory)
                            cumulativeAcked += s->entries[i].size;
                    }
                    if (msg.entryIndex < s->entries.size())
                        cumulativeAcked += msg.ackedOffset;
                }
            }
            if (s) {
                std::lock_guard<std::mutex> a(s->ackMutex);
                if (cumulativeAcked > s->ackedBytes)
                    s->ackedBytes = cumulativeAcked;
                s->lastAckTime = std::chrono::steady_clock::now();
                s->ackCv.notify_all();
            }
            break;
        }

        case MsgType::TransferComplete: {
            TransferCompleteMsg msg;
            if (!parseTransferComplete(payload, msg)) return;

            ZB_LOG_INFO("Transfer {} complete: result={}", msg.transferId, msg.result);

            // Check if this was a clipboard transfer and collect file paths.
            bool isClipboard = false;
            std::vector<std::string> receivedPaths;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                auto it = receivers_.find(msg.transferId);
                if (it != receivers_.end()) {
                    for (auto& f : it->second.files) {
                        if (f.is_open()) f.close();
                    }
                    isClipboard = (it->second.flags & 1) != 0;
                    if (isClipboard && msg.result == 0) {
                        const auto& dir = it->second.destDir;
                        for (const auto& e : it->second.entries) {
                            if (!e.isDirectory) {
                                receivedPaths.push_back(pathToUtf8(
                                    pathFromUtf8(dir) /
                                    pathFromUtf8(sanitizeFilename(e.relativePath))));
                            }
                        }
                    }
                    receivers_.erase(it);
                }
            }

            if (completeCb_) completeCb_(msg.transferId, msg.result == 0, msg.message);

            if (isClipboard && msg.result == 0 && clipboardCompleteCb_ && !receivedPaths.empty()) {
                clipboardCompleteCb_(msg.transferId, receivedPaths);
            }
            break;
        }

        default:
            break;
    }
}

void FileTransferManager::handleFileChunk(const FileChunkMsg& msg) {
    uint64_t ackedOffset = 0;
    uint64_t totalReceived = 0;
    uint64_t totalSize = 0;
    bool needAck = false;

    {
        std::lock_guard<std::mutex> lk(mutex_);

        auto it = receivers_.find(msg.transferId);
        if (it == receivers_.end()) return;

        auto& recv = it->second;
        if (msg.entryIndex >= recv.files.size()) return;

        auto& file = recv.files[msg.entryIndex];
        if (file.is_open()) {
            file.seekp(static_cast<std::streamoff>(msg.offset), std::ios::beg);
            file.write(reinterpret_cast<const char*>(msg.data.data()),
                       static_cast<std::streamsize>(msg.data.size()));
        }

        recv.receivedBytes[msg.entryIndex] = msg.offset + msg.data.size();
        recv.totalReceived += msg.data.size();

        ackedOffset = recv.receivedBytes[msg.entryIndex];
        totalReceived = recv.totalReceived;
        totalSize = recv.totalSize;
        needAck = true;
    }

    // Send ACK and progress outside the lock to avoid deadlocks with network IO.
    if (needAck && sendCb_) {
        FileChunkAckMsg ack;
        ack.transferId = msg.transferId;
        ack.entryIndex = msg.entryIndex;
        ack.ackedOffset = ackedOffset;
        sendCb_(MsgType::FileChunkAck, serializeFileChunkAck(ack));
    }

    if (needAck && progressCb_) {
        progressCb_(msg.transferId, totalReceived, totalSize);
    }
}

void FileTransferManager::senderThread(uint64_t transferId,
                                        const std::vector<TransferEntry>& entries,
                                        const std::vector<std::string>& entryPaths,
                                        const std::string& /*destDir*/) {
    ZB_LOG_INFO("Sender thread started for transfer {}", transferId);

    // Obtain a stable pointer to our state. It remains valid for the lifetime
    // of this thread because cancelAll() joins the thread before erasing the
    // state, and the accept-timeout watchdog only erases inactive senders.
    SenderState* self = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = senders_.find(transferId);
        if (it != senders_.end()) self = &it->second;
    }
    if (!self) {
        ZB_LOG_ERROR("Sender thread: state for transfer {} vanished", transferId);
        return;
    }

    auto markFinished = [this, transferId]() {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = senders_.find(transferId);
        if (it != senders_.end()) {
            it->second.active = false;
            it->second.finished = true;
        }
    };

    auto isActive = [this, transferId]() -> bool {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = senders_.find(transferId);
        return it != senders_.end() && it->second.active.load();
    };

    bool success = false;
    std::string errMsg = "ok";

    try {
        uint64_t totalSent = 0;
        uint64_t totalSize = 0;
        for (const auto& e : entries) {
            if (!e.isDirectory) totalSize += e.size;
        }

        std::vector<uint8_t> buf(kChunkSize);

        for (uint32_t entryIdx = 0; entryIdx < entries.size(); ++entryIdx) {
            const auto& e = entries[entryIdx];
            if (e.isDirectory) continue;

            if (!isActive()) {
                ZB_LOG_WARN("Transfer {} cancelled", transferId);
                if (completeCb_) completeCb_(transferId, false, "已取消");
                return;
            }

            if (entryIdx >= entryPaths.size()) {
                ZB_LOG_ERROR("Entry index {} out of range for entryPaths", entryIdx);
                continue;
            }

#ifdef _WIN32
            std::ifstream file(pathFromUtf8(entryPaths[entryIdx]), std::ios::binary);
#else
            std::ifstream file(entryPaths[entryIdx], std::ios::binary);
#endif
            if (!file.is_open()) {
                ZB_LOG_ERROR("Cannot open file for sending: {}", entryPaths[entryIdx]);
                continue;
            }

            uint64_t offset = 0;
            while (file) {
                if (!isActive()) {
                    ZB_LOG_WARN("Transfer {} cancelled mid-stream", transferId);
                    markFinished();
                    if (completeCb_) completeCb_(transferId, false, "已取消");
                    return;
                }

                file.read(reinterpret_cast<char*>(buf.data()), kChunkSize);
                auto bytesRead = static_cast<uint32_t>(file.gcount());
                if (bytesRead == 0) break;

                // Flow control: wait until the receiver has acknowledged
                // enough data that our in-flight window has room. This also
                // acts as a liveness check — if no ACK arrives within the
                // timeout the transfer is aborted.
                {
                    std::unique_lock<std::mutex> a(self->ackMutex);
                    while (self->active.load() &&
                           (totalSent - self->ackedBytes) >= kSendWindowBytes) {
                        auto r = self->ackCv.wait_for(a,
                            std::chrono::seconds(kAckTimeoutSec));
                        if (r == std::cv_status::timeout) {
                            // Check whether we made progress to avoid a
                            // spurious timeout on very slow links.
                            if ((totalSent - self->ackedBytes) >= kSendWindowBytes) {
                                ZB_LOG_ERROR("Transfer {} ACK timeout", transferId);
                                markFinished();
                                if (completeCb_) completeCb_(transferId, false,
                                    "等待对端确认超时，连接可能已断开");
                                return;
                            }
                        }
                    }
                }

                FileChunkMsg chunk;
                chunk.transferId = transferId;
                chunk.entryIndex = entryIdx;
                chunk.offset = offset;
                chunk.data.assign(buf.data(), buf.data() + bytesRead);

                if (sendCb_) {
                    if (!sendCb_(MsgType::FileChunk, serializeFileChunk(chunk))) {
                        ZB_LOG_ERROR("Transfer {} send failed (connection lost)", transferId);
                        markFinished();
                        if (completeCb_) completeCb_(transferId, false,
                            "连接已断开，传输中断");
                        return;
                    }
                }

                offset += bytesRead;
                totalSent += bytesRead;

                if (progressCb_) {
                    progressCb_(transferId, totalSent, totalSize);
                }
            }

            file.close();
            ZB_LOG_INFO("Sent entry {}/{}: {} ({} bytes)",
                        entryIdx + 1, entries.size(), e.relativePath, e.size);
        }

        // Send completion.
        TransferCompleteMsg complete;
        complete.transferId = transferId;
        complete.result = 0;
        complete.message = "ok";

        if (sendCb_) {
            if (!sendCb_(MsgType::TransferComplete, serializeTransferComplete(complete))) {
                ZB_LOG_ERROR("Transfer {} failed to send completion", transferId);
                errMsg = "连接已断开，传输可能不完整";
            } else {
                success = true;
            }
        } else {
            success = true;
        }
    } catch (const std::exception& e) {
        ZB_LOG_ERROR("Sender thread exception for transfer {}: {}", transferId, e.what());
        errMsg = e.what();

        // Try to notify the receiver of failure.
        try {
            TransferCompleteMsg fail;
            fail.transferId = transferId;
            fail.result = 1;
            fail.message = "发送端错误";
            if (sendCb_) {
                sendCb_(MsgType::TransferComplete, serializeTransferComplete(fail));
            }
        } catch (...) {}
    } catch (...) {
        ZB_LOG_ERROR("Sender thread unknown exception for transfer {}", transferId);
        errMsg = "未知错误";
    }

    markFinished();
    if (completeCb_) completeCb_(transferId, success, errMsg);

    ZB_LOG_INFO("Sender thread finished for transfer {} (success={})", transferId, success);
}

std::string FileTransferManager::sanitizeFilename(const std::string& name) const {
    // Strip leading drive letters and path separators to prevent path traversal.
    std::string result = name;
    // Replace backslashes with forward slashes for consistent handling.
    std::replace(result.begin(), result.end(), '\\', '/');

    // Remove any ".." components.
    std::string cleaned;
    size_t pos = 0;
    while (pos < result.size()) {
        size_t slash = result.find('/', pos);
        std::string component = (slash == std::string::npos)
            ? result.substr(pos)
            : result.substr(pos, slash - pos);

        if (component != ".." && !component.empty()) {
            if (!cleaned.empty()) cleaned += '/';
            cleaned += component;
        }

        if (slash == std::string::npos) break;
        pos = slash + 1;
    }

    return cleaned.empty() ? "file" : cleaned;
}

std::string FileTransferManager::resolveCollision(const std::string& path) const {
    fs::path p = pathFromUtf8(path);
    if (!fs::exists(p)) return path;

    std::string stem = pathToUtf8(p.stem());
    std::string ext = pathToUtf8(p.extension());
    fs::path parent = p.parent_path();

    for (int i = 1; i < 1000; ++i) {
        std::string newName = stem + " (" + std::to_string(i) + ")" + ext;
        fs::path newPath = parent / pathFromUtf8(newName);
        if (!fs::exists(newPath)) return pathToUtf8(newPath);
    }

    return path;
}

} // namespace zb
