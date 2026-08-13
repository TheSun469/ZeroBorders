#include "Protocol.h"

#include <cstring>

namespace zb {

// ---- Writer ----

void Writer::u16(uint16_t v) {
    buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf_.push_back(static_cast<uint8_t>(v & 0xFF));
}

void Writer::u32(uint32_t v) {
    buf_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf_.push_back(static_cast<uint8_t>(v & 0xFF));
}

void Writer::u64(uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        buf_.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

void Writer::bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
}

void Writer::string(const std::string& s) {
    u16(static_cast<uint16_t>(s.size()));
    bytes(s.data(), s.size());
}

// ---- Reader ----

uint8_t Reader::u8() {
    if (p_ >= end_) return 0;
    return *p_++;
}

uint16_t Reader::u16() {
    if (end_ - p_ < 2) { p_ = end_; return 0; }
    uint16_t v = static_cast<uint16_t>(p_[0]) << 8 | p_[1];
    p_ += 2;
    return v;
}

uint32_t Reader::u32() {
    if (end_ - p_ < 4) { p_ = end_; return 0; }
    uint32_t v = (static_cast<uint32_t>(p_[0]) << 24) |
                 (static_cast<uint32_t>(p_[1]) << 16) |
                 (static_cast<uint32_t>(p_[2]) << 8)  |
                 static_cast<uint32_t>(p_[3]);
    p_ += 4;
    return v;
}

uint64_t Reader::u64() {
    if (end_ - p_ < 8) { p_ = end_; return 0; }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p_[i];
    }
    p_ += 8;
    return v;
}

bool Reader::bytes(void* out, size_t n) {
    if (end_ - p_ < static_cast<ptrdiff_t>(n)) { p_ = end_; return false; }
    std::memcpy(out, p_, n);
    p_ += n;
    return true;
}

bool Reader::string(std::string& out) {
    uint16_t len = u16();
    if (end_ - p_ < len) { p_ = end_; return false; }
    out.assign(reinterpret_cast<const char*>(p_), len);
    p_ += len;
    return true;
}

// ---- Payload serializers ----

std::vector<uint8_t> serializeHello(const HelloMsg& m) {
    Writer w;
    w.u8(m.version);
    w.u32(m.capabilities);
    w.bytes(m.token.data(), m.token.size());
    return w.take();
}

bool parseHello(const std::vector<uint8_t>& d, HelloMsg& out) {
    Reader r(d);
    out.version = r.u8();
    out.capabilities = r.u32();
    if (!r.bytes(out.token.data(), out.token.size())) return false;
    return r.ok();
}

std::vector<uint8_t> serializeWelcome(const WelcomeMsg& m) {
    Writer w;
    w.u8(m.result);
    w.u32(m.screenWidth);
    w.u32(m.screenHeight);
    w.u32(m.capabilities);
    return w.take();
}

bool parseWelcome(const std::vector<uint8_t>& d, WelcomeMsg& out) {
    Reader r(d);
    out.result = r.u8();
    out.screenWidth = r.u32();
    out.screenHeight = r.u32();
    out.capabilities = r.u32();
    return r.ok();
}

std::vector<uint8_t> serializeDataHello(const DataHelloMsg& m) {
    Writer w;
    w.u8(m.version);
    w.bytes(m.token.data(), m.token.size());
    return w.take();
}

bool parseDataHello(const std::vector<uint8_t>& d, DataHelloMsg& out) {
    Reader r(d);
    out.version = r.u8();
    if (!r.bytes(out.token.data(), out.token.size())) return false;
    return r.ok();
}

std::vector<uint8_t> serializeGoodbye(GoodbyeReason r) {
    Writer w;
    w.u8(static_cast<uint8_t>(r));
    return w.take();
}

bool parseGoodbye(const std::vector<uint8_t>& d, GoodbyeReason& out) {
    if (d.empty()) return false;
    out = static_cast<GoodbyeReason>(d[0]);
    return true;
}

std::vector<uint8_t> serializePingPong(uint64_t timestamp) {
    Writer w;
    w.u64(timestamp);
    return w.take();
}

bool parsePingPong(const std::vector<uint8_t>& d, uint64_t& timestamp) {
    Reader r(d);
    timestamp = r.u64();
    return r.ok();
}

// ---- Input event serialization ----

std::vector<uint8_t> serializeInputEvent(const InputEvent& ev) {
    Writer w;
    w.u8(static_cast<uint8_t>(ev.type));
    w.u64(ev.timestamp);
    switch (ev.type) {
        case EventType::MouseMove:
            w.i32(ev.mouseMove.x);
            w.i32(ev.mouseMove.y);
            break;
        case EventType::MouseButton:
            w.u8(static_cast<uint8_t>(ev.mouseButton.button));
            w.u8(ev.mouseButton.pressed ? 1 : 0);
            w.i32(ev.mouseButton.x);
            w.i32(ev.mouseButton.y);
            break;
        case EventType::MouseWheel:
            w.i32(ev.mouseWheel.delta);
            w.u8(ev.mouseWheel.horizontal ? 1 : 0);
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            w.u16(ev.key.vkCode);
            w.u16(ev.key.scanCode);
            w.u8(ev.key.extended ? 1 : 0);
            break;
        default:
            break;
    }
    return w.take();
}

bool parseInputEvent(const std::vector<uint8_t>& d, InputEvent& out) {
    Reader r(d);
    out.type = static_cast<EventType>(r.u8());
    out.timestamp = r.u64();
    switch (out.type) {
        case EventType::MouseMove:
            out.mouseMove.x = r.i32();
            out.mouseMove.y = r.i32();
            break;
        case EventType::MouseButton:
            out.mouseButton.button = static_cast<MouseButton>(r.u8());
            out.mouseButton.pressed = r.u8() != 0;
            out.mouseButton.x = r.i32();
            out.mouseButton.y = r.i32();
            break;
        case EventType::MouseWheel:
            out.mouseWheel.delta = r.i32();
            out.mouseWheel.horizontal = r.u8() != 0;
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            out.key.vkCode = r.u16();
            out.key.scanCode = r.u16();
            out.key.extended = r.u8() != 0;
            break;
        default:
            break;
    }
    return r.ok();
}

// ---- Cursor handoff serialization ----

std::vector<uint8_t> serializeCursorEnter(const CursorEnterMsg& m) {
    Writer w;
    w.i32(m.x);
    w.i32(m.y);
    return w.take();
}

bool parseCursorEnter(const std::vector<uint8_t>& d, CursorEnterMsg& out) {
    Reader r(d);
    out.x = r.i32();
    out.y = r.i32();
    return r.ok();
}

std::vector<uint8_t> serializeCursorLeave(const CursorLeaveMsg& m) {
    Writer w;
    w.u8(static_cast<uint8_t>(m.edge));
    return w.take();
}

bool parseCursorLeave(const std::vector<uint8_t>& d, CursorLeaveMsg& out) {
    if (d.empty()) return false;
    out.edge = static_cast<Edge>(d[0]);
    return true;
}

// ---- Layout sync ----

std::vector<uint8_t> serializeLayoutSync(const LayoutSyncMsg& m) {
    Writer w;
    w.u8(static_cast<uint8_t>(m.layout));
    return w.take();
}

bool parseLayoutSync(const std::vector<uint8_t>& d, LayoutSyncMsg& out) {
    if (d.empty()) return false;
    out.layout = static_cast<ScreenLayout>(d[0]);
    return true;
}

// ---- PathSync ----

std::vector<uint8_t> serializePathSync(const PathSyncMsg& m) {
    Writer w;
    w.string(m.receiveDir);
    return w.take();
}

bool parsePathSync(const std::vector<uint8_t>& d, PathSyncMsg& out) {
    Reader r(d.data(), d.size());
    return r.string(out.receiveDir);
}

// ---- Clipboard serialization ----

std::vector<uint8_t> serializeClipboardText(const ClipboardTextMsg& m) {
    Writer w;
    w.string(m.text);
    return w.take();
}

bool parseClipboardText(const std::vector<uint8_t>& d, ClipboardTextMsg& out) {
    Reader r(d);
    if (!r.string(out.text)) return false;
    return r.ok();
}

std::vector<uint8_t> serializeClipboardImage(const ClipboardImageMsg& m) {
    Writer w;
    w.u32(m.width);
    w.u32(m.height);
    w.u32(static_cast<uint32_t>(m.data.size()));
    w.bytes(m.data.data(), m.data.size());
    return w.take();
}

bool parseClipboardImage(const std::vector<uint8_t>& d, ClipboardImageMsg& out) {
    Reader r(d);
    out.width = r.u32();
    out.height = r.u32();
    uint32_t dataSize = r.u32();
    if (r.remaining() < dataSize) return false;
    out.data.resize(dataSize);
    if (!r.bytes(out.data.data(), dataSize)) return false;
    return r.ok();
}

// ---- File transfer serialization ----

std::vector<uint8_t> serializeFileOffer(const FileOfferMsg& m) {
    Writer w;
    w.u64(m.transferId);
    w.u32(m.entryCount);
    w.u64(m.totalSize);
    for (const auto& e : m.entries) {
        w.string(e.relativePath);
        w.u64(e.size);
        w.u8(e.isDirectory ? 1 : 0);
    }
    w.u8(m.flags);
    return w.take();
}

bool parseFileOffer(const std::vector<uint8_t>& d, FileOfferMsg& out) {
    Reader r(d);
    out.transferId = r.u64();
    out.entryCount = r.u32();
    out.totalSize = r.u64();
    for (uint32_t i = 0; i < out.entryCount; ++i) {
        TransferEntry e;
        if (!r.string(e.relativePath)) return false;
        e.size = r.u64();
        e.isDirectory = r.u8() != 0;
        out.entries.push_back(std::move(e));
    }
    out.flags = r.u8();
    return r.ok();
}

std::vector<uint8_t> serializeFileAccept(const FileAcceptMsg& m) {
    Writer w;
    w.u64(m.transferId);
    w.u8(m.result);
    w.string(m.destDir);
    return w.take();
}

bool parseFileAccept(const std::vector<uint8_t>& d, FileAcceptMsg& out) {
    Reader r(d);
    out.transferId = r.u64();
    out.result = r.u8();
    if (!r.string(out.destDir)) return false;
    return r.ok();
}

std::vector<uint8_t> serializeFileChunk(const FileChunkMsg& m) {
    Writer w;
    w.u64(m.transferId);
    w.u32(m.entryIndex);
    w.u64(m.offset);
    w.u32(static_cast<uint32_t>(m.data.size()));
    w.bytes(m.data.data(), m.data.size());
    return w.take();
}

bool parseFileChunk(const std::vector<uint8_t>& d, FileChunkMsg& out) {
    Reader r(d);
    out.transferId = r.u64();
    out.entryIndex = r.u32();
    out.offset = r.u64();
    uint32_t dataSize = r.u32();
    if (r.remaining() < dataSize) return false;
    out.data.resize(dataSize);
    if (!r.bytes(out.data.data(), dataSize)) return false;
    return r.ok();
}

std::vector<uint8_t> serializeFileChunkAck(const FileChunkAckMsg& m) {
    Writer w;
    w.u64(m.transferId);
    w.u32(m.entryIndex);
    w.u64(m.ackedOffset);
    return w.take();
}

bool parseFileChunkAck(const std::vector<uint8_t>& d, FileChunkAckMsg& out) {
    Reader r(d);
    out.transferId = r.u64();
    out.entryIndex = r.u32();
    out.ackedOffset = r.u64();
    return r.ok();
}

std::vector<uint8_t> serializeTransferComplete(const TransferCompleteMsg& m) {
    Writer w;
    w.u64(m.transferId);
    w.u8(m.result);
    w.string(m.message);
    return w.take();
}

bool parseTransferComplete(const std::vector<uint8_t>& d, TransferCompleteMsg& out) {
    Reader r(d);
    out.transferId = r.u64();
    out.result = r.u8();
    if (!r.string(out.message)) return false;
    return r.ok();
}

// ---- Framing ----

std::vector<uint8_t> buildFrame(Channel ch, MsgType t, const uint8_t* p, size_t n) {
    std::vector<uint8_t> frame;
    frame.reserve((ch == Channel::Control ? 5 : 7) + n);
    frame.push_back(kMagicByte0);
    frame.push_back(kMagicByte1);
    frame.push_back(static_cast<uint8_t>(t));
    if (ch == Channel::Control) {
        frame.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(n & 0xFF));
    } else {
        frame.push_back(static_cast<uint8_t>((n >> 24) & 0xFF));
        frame.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
        frame.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(n & 0xFF));
    }
    frame.insert(frame.end(), p, p + n);
    return frame;
}

void FrameParser::feed(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
    parse();
}

void FrameParser::parse() {
    const size_t headerLen = (channel_ == Channel::Control) ? 5 : 7;
    while (buf_.size() >= headerLen) {
        // Validate magic, resync if needed
        if (buf_[0] != kMagicByte0 || buf_[1] != kMagicByte1) {
            size_t skip = 1;
            for (size_t i = 1; i + 1 < buf_.size(); ++i) {
                if (buf_[i] == kMagicByte0 && buf_[i + 1] == kMagicByte1) {
                    skip = i;
                    break;
                }
            }
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(skip));
            continue;
        }

        const MsgType type = static_cast<MsgType>(buf_[2]);
        uint32_t payloadLen = 0;
        if (channel_ == Channel::Control) {
            payloadLen = (static_cast<uint32_t>(buf_[3]) << 8) | buf_[4];
            if (payloadLen > kMaxControlPayload) {
                // Oversized control frame, discard and resync
                buf_.erase(buf_.begin(), buf_.begin() + 2);
                continue;
            }
        } else {
            payloadLen = (static_cast<uint32_t>(buf_[3]) << 24) |
                         (static_cast<uint32_t>(buf_[4]) << 16) |
                         (static_cast<uint32_t>(buf_[5]) << 8)  |
                         static_cast<uint32_t>(buf_[6]);
            if (payloadLen > kMaxDataPayload) {
                buf_.erase(buf_.begin(), buf_.begin() + 2);
                continue;
            }
        }

        if (buf_.size() < headerLen + payloadLen) {
            break; // wait for more data
        }

        std::vector<uint8_t> payload(buf_.begin() + static_cast<ptrdiff_t>(headerLen),
                                     buf_.begin() + static_cast<ptrdiff_t>(headerLen + payloadLen));
        buf_.erase(buf_.begin(),
                   buf_.begin() + static_cast<ptrdiff_t>(headerLen + payloadLen));
        if (cb_) cb_(type, payload);
    }
}

} // namespace zb
