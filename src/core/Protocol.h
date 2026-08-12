#pragma once

#include "Types.h"
#include "Event.h"
#include "ScreenLayout.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zb {

enum class MsgType : uint8_t {
    // Control channel
    Hello            = 0x01,
    Welcome          = 0x02,
    InputEvent       = 0x03,
    CursorEnter      = 0x04,
    CursorLeave      = 0x05,
    Ping             = 0x06,
    Pong             = 0x07,
    Goodbye          = 0x08,
    LayoutSync       = 0x09,  // 屏幕相对布局变更（双向同步）

    ClipboardText    = 0x10,
    ClipboardNotify  = 0x11,
    FileOffer        = 0x12,
    FileAccept       = 0x13,
    TransferProgress = 0x14,
    TransferComplete = 0x15,

    // Data channel
    DataHello        = 0x20,
    ClipboardImage   = 0x21,
    ClipboardData    = 0x22,
    FileChunk        = 0x23,
    FileChunkAck     = 0x24,
};

// ---- Payload structs (Phase 1) ----

struct HelloMsg {
    uint8_t version = kProtocolVersion;
    uint32_t capabilities = 0;
    TokenHash token{};
};

struct WelcomeMsg {
    uint8_t result = 0;          // 0 = ok, 1 = bad token, 2 = bad version
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
    uint32_t capabilities = 0;
};

struct DataHelloMsg {
    uint8_t version = kProtocolVersion;
    TokenHash token{};
};

enum class GoodbyeReason : uint8_t {
    Normal = 0,
    Timeout = 1,
    Error = 2,
};

// ---- Serialization helpers ----

class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void bytes(const void* p, size_t n);
    void string(const std::string& s);
    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t> take() { return std::move(buf_); }
private:
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    explicit Reader(const std::vector<uint8_t>& v) : p_(v.data()), end_(v.data() + v.size()) {}

    bool ok() const { return p_ <= end_; }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int32_t i32() { return static_cast<int32_t>(u32()); }
    bool bytes(void* out, size_t n);
    bool string(std::string& out);
private:
    const uint8_t* p_;
    const uint8_t* end_;
};

// ---- Payload serializers (Phase 1) ----

std::vector<uint8_t> serializeHello(const HelloMsg& m);
bool parseHello(const std::vector<uint8_t>& d, HelloMsg& out);

std::vector<uint8_t> serializeWelcome(const WelcomeMsg& m);
bool parseWelcome(const std::vector<uint8_t>& d, WelcomeMsg& out);

std::vector<uint8_t> serializeDataHello(const DataHelloMsg& m);
bool parseDataHello(const std::vector<uint8_t>& d, DataHelloMsg& out);

std::vector<uint8_t> serializeGoodbye(GoodbyeReason r);
bool parseGoodbye(const std::vector<uint8_t>& d, GoodbyeReason& out);

std::vector<uint8_t> serializePingPong(uint64_t timestamp);
bool parsePingPong(const std::vector<uint8_t>& d, uint64_t& timestamp);

// ---- Input event serialization (Phase 2) ----

std::vector<uint8_t> serializeInputEvent(const InputEvent& ev);
bool parseInputEvent(const std::vector<uint8_t>& d, InputEvent& out);

// ---- Cursor handoff serialization (Phase 3) ----

struct CursorEnterMsg {
    int32_t x;      // Entry point on client screen
    int32_t y;
};

struct CursorLeaveMsg {
    Edge edge;      // Which client edge was crossed
};

std::vector<uint8_t> serializeCursorEnter(const CursorEnterMsg& m);
bool parseCursorEnter(const std::vector<uint8_t>& d, CursorEnterMsg& out);

std::vector<uint8_t> serializeCursorLeave(const CursorLeaveMsg& m);
bool parseCursorLeave(const std::vector<uint8_t>& d, CursorLeaveMsg& out);

// ---- Layout sync ----
// Sent over the control channel whenever the user changes the relative
// screen position on either device, so the peer updates its router/UI.
struct LayoutSyncMsg {
    ScreenLayout layout = ScreenLayout::LeftOf;
};

std::vector<uint8_t> serializeLayoutSync(const LayoutSyncMsg& m);
bool parseLayoutSync(const std::vector<uint8_t>& d, LayoutSyncMsg& out);

// ---- Clipboard serialization (Phase 4) ----

struct ClipboardTextMsg {
    std::string text;  // UTF-8 text, up to 64KB (control channel limit)
};

struct ClipboardImageMsg {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> data;  // Raw DIB bytes (CF_DIB format)
};

std::vector<uint8_t> serializeClipboardText(const ClipboardTextMsg& m);
bool parseClipboardText(const std::vector<uint8_t>& d, ClipboardTextMsg& out);

std::vector<uint8_t> serializeClipboardImage(const ClipboardImageMsg& m);
bool parseClipboardImage(const std::vector<uint8_t>& d, ClipboardImageMsg& out);

// ---- File transfer serialization (Phase 5) ----

struct TransferEntry {
    std::string relativePath;
    uint64_t size = 0;
    bool isDirectory = false;
};

struct FileOfferMsg {
    uint64_t transferId = 0;
    uint32_t entryCount = 0;
    uint64_t totalSize = 0;
    std::vector<TransferEntry> entries;
    uint8_t flags = 0;  // bit 0: clipboard transfer (auto-accept, put in clipboard)
};

struct FileAcceptMsg {
    uint64_t transferId = 0;
    uint8_t result = 0;       // 0 = accept, 1 = reject
    std::string destDir;
};

struct FileChunkMsg {
    uint64_t transferId = 0;
    uint32_t entryIndex = 0;  // which file in the offer
    uint64_t offset = 0;      // byte offset within the file
    std::vector<uint8_t> data;
};

struct FileChunkAckMsg {
    uint64_t transferId = 0;
    uint32_t entryIndex = 0;
    uint64_t ackedOffset = 0; // total bytes acknowledged for this entry
};

struct TransferCompleteMsg {
    uint64_t transferId = 0;
    uint8_t result = 0;       // 0 = ok, 1 = error
    std::string message;
};

std::vector<uint8_t> serializeFileOffer(const FileOfferMsg& m);
bool parseFileOffer(const std::vector<uint8_t>& d, FileOfferMsg& out);

std::vector<uint8_t> serializeFileAccept(const FileAcceptMsg& m);
bool parseFileAccept(const std::vector<uint8_t>& d, FileAcceptMsg& out);

std::vector<uint8_t> serializeFileChunk(const FileChunkMsg& m);
bool parseFileChunk(const std::vector<uint8_t>& d, FileChunkMsg& out);

std::vector<uint8_t> serializeFileChunkAck(const FileChunkAckMsg& m);
bool parseFileChunkAck(const std::vector<uint8_t>& d, FileChunkAckMsg& out);

std::vector<uint8_t> serializeTransferComplete(const TransferCompleteMsg& m);
bool parseTransferComplete(const std::vector<uint8_t>& d, TransferCompleteMsg& out);

// ---- Framing ----

std::vector<uint8_t> buildFrame(Channel ch, MsgType t, const uint8_t* p, size_t n);
inline std::vector<uint8_t> buildFrame(Channel ch, MsgType t, const std::vector<uint8_t>& payload) {
    return buildFrame(ch, t, payload.data(), payload.size());
}

class FrameParser {
public:
    explicit FrameParser(Channel ch) : channel_(ch) {}
    void feed(const uint8_t* data, size_t len);
    void onFrame(std::function<void(MsgType, const std::vector<uint8_t>&)> cb) { cb_ = std::move(cb); }
    void reset() { buf_.clear(); }

private:
    void parse();
    Channel channel_;
    std::vector<uint8_t> buf_;
    std::function<void(MsgType, const std::vector<uint8_t>&)> cb_;
};

} // namespace zb
