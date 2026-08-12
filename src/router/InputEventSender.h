#pragma once

#include "../core/Event.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace zb {

// InputEventSender offloads network sending from the input-hook thread to a
// dedicated thread. Mouse-move events are coalesced: if a move is still
// pending when a new move arrives, the older one is replaced by the newer one.
// Button, wheel, and key events are sent in order after flushing any pending
// move, so clicks are never delayed behind a backlog of move events.
class InputEventSender {
public:
    using SendCallback = std::function<void(const InputEvent&)>;

    InputEventSender() = default;
    ~InputEventSender() { stop(); }

    InputEventSender(const InputEventSender&) = delete;
    InputEventSender& operator=(const InputEventSender&) = delete;

    void start(SendCallback cb) {
        sendCb_ = std::move(cb);
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // Submit an event for asynchronous sending. Mouse moves are coalesced;
    // all other events are delivered in FIFO order after pending moves.
    void submit(const InputEvent& ev) {
        if (!running_.load()) return;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (ev.type == EventType::MouseMove) {
                pendingMove_ = ev;
                hasPendingMove_ = true;
            } else {
                queue_.push_back(ev);
            }
        }
        cv_.notify_one();
    }

private:
    void run() {
        std::deque<InputEvent> localQueue;
        InputEvent moveEvent{};
        bool hasMove = false;

        while (running_.load()) {
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] {
                    return hasPendingMove_ || !queue_.empty() || !running_.load();
                });

                if (!running_.load() && !hasPendingMove_ && queue_.empty()) {
                    break;
                }

                if (hasPendingMove_) {
                    moveEvent = pendingMove_;
                    hasMove = true;
                    hasPendingMove_ = false;
                }
                if (!queue_.empty()) {
                    localQueue.swap(queue_);
                }
            }

            // Send coalesced move first, then FIFO events. Sending the move
            // before a button press ensures the cursor reaches the target
            // before the click is injected on the remote side.
            if (hasMove) {
                if (sendCb_) sendCb_(moveEvent);
                hasMove = false;
            }
            for (const auto& ev : localQueue) {
                if (sendCb_) sendCb_(ev);
            }
            localQueue.clear();
        }
    }

    SendCallback sendCb_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool hasPendingMove_ = false;
    InputEvent pendingMove_{};
    std::deque<InputEvent> queue_;
};

} // namespace zb
