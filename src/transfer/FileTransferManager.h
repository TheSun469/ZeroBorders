#pragma once

#include "../core/Protocol.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zb {

// Manages file transfer over the data channel.
// Sender side: sendFiles() → FileOffer → wait FileAccept → stream FileChunks.
// Receiver side: receives FileOffer → invokes incoming offer callback → the
// owner calls accept()/reject() after user confirmation; chunks then flow.
class FileTransferManager {
public:
    using SendDataCallback = std::function<void(MsgType, const std::vector<uint8_t>&)>;
    using OfferCallback = std::function<void(uint64_t id, const std::vector<TransferEntry>&, uint64_t total)>;
    using ProgressCallback = std::function<void(uint64_t id, uint64_t transferred, uint64_t total)>;
    using CompleteCallback = std::function<void(uint64_t id, bool ok, const std::string& message)>;
    using ClipboardCompleteCallback = std::function<void(uint64_t id, const std::vector<std::string>& paths)>;

    FileTransferManager();
    ~FileTransferManager();

    void setSendCallback(SendDataCallback cb) { sendCb_ = std::move(cb); }
    void onOffer(OfferCallback cb) { offerCb_ = std::move(cb); }
    void onProgress(ProgressCallback cb) { progressCb_ = std::move(cb); }
    void onComplete(CompleteCallback cb) { completeCb_ = std::move(cb); }
    void setClipboardCompleteCallback(ClipboardCompleteCallback cb) { clipboardCompleteCb_ = std::move(cb); }

    // If set, incoming FileOffer messages invoke this callback instead of
    // being auto-accepted. The owner MUST later call accept()/reject().
    void setIncomingOfferCallback(OfferCallback cb) { incomingOfferCb_ = std::move(cb); }

    // Set the default directory used when auto-accepting (no incoming callback
    // is registered). If empty, TEMP/ZeroBorders is used.
    void setDefaultReceiveDir(std::string dir) { defaultReceiveDir_ = std::move(dir); }

    // Sender: initiate a file transfer. Returns the transfer ID (0 on failure).
    // flags: bit 0 = clipboard transfer (auto-accept on receiver, put in clipboard).
    uint64_t sendFiles(const std::vector<std::string>& localPaths, uint8_t flags = 0);

    // Receiver: accept an offer and specify destination directory.
    void accept(uint64_t id, const std::string& destDir);
    void reject(uint64_t id);

    // Called when a data-channel message arrives.
    void handleMessage(MsgType type, const std::vector<uint8_t>& payload);

    // Cancel all in-progress transfers.
    void cancelAll();

private:
    static constexpr uint32_t kChunkSize = 64 * 1024;

    // Build file offer entries from local paths (recursively for directories).
    // entryPaths[i] is the local filesystem path corresponding to entries[i].
    bool buildEntries(const std::vector<std::string>& paths,
                      std::vector<TransferEntry>& entries,
                      std::vector<std::string>& entryPaths,
                      uint64_t& totalSize);

    // Join and remove sender threads that have finished.
    void cleanupFinishedSenders();

    // Sender thread: stream file chunks after offer is accepted.
    void senderThread(uint64_t transferId, const std::vector<TransferEntry>& entries,
                      const std::vector<std::string>& entryPaths,
                      const std::string& destDir);

    // Sanitize a filename: strip path separators, handle collisions.
    std::string sanitizeFilename(const std::string& name) const;
    std::string resolveCollision(const std::string& path) const;

    // Handle received file chunk on receiver side.
    void handleFileChunk(const FileChunkMsg& msg);

    // Generate next transfer ID.
    uint64_t nextId() { return ++nextId_; }

    struct ReceiverState {
        std::string destDir;
        std::vector<TransferEntry> entries;
        std::vector<uint64_t> receivedBytes;
        std::vector<std::ofstream> files;
        uint64_t totalReceived = 0;
        uint64_t totalSize = 0;
        uint8_t flags = 0;
    };

    struct PendingOffer {
        std::vector<TransferEntry> entries;
        uint64_t totalSize = 0;
        uint8_t flags = 0;
    };

    struct SenderState {
        std::vector<TransferEntry> entries;
        std::vector<std::string> entryPaths; // local path for each entry
        std::thread thread;
        std::atomic<bool> active{false};
        std::atomic<bool> finished{false};
    };

    std::atomic<uint64_t> nextId_{0};
    SendDataCallback sendCb_;
    OfferCallback offerCb_;
    ProgressCallback progressCb_;
    CompleteCallback completeCb_;
    OfferCallback incomingOfferCb_;
    ClipboardCompleteCallback clipboardCompleteCb_;
    std::string defaultReceiveDir_;

    std::mutex mutex_;
    std::unordered_map<uint64_t, ReceiverState> receivers_;
    std::unordered_map<uint64_t, SenderState> senders_;
    std::unordered_map<uint64_t, PendingOffer> pendingOffers_;
};

} // namespace zb
