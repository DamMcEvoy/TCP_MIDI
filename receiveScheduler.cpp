#include "receiveScheduler.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>

namespace {
constexpr int64_t kDefaultJitterBufferNs = 80'000'000; // 80 ms
}

ReceiveScheduler::ReceiveScheduler(TimeSync& timeSync)
    : timeSync_(timeSync),
      outputCallback_(nullptr),
      running_(false) {}

ReceiveScheduler::~ReceiveScheduler() {
    stop();
}

void ReceiveScheduler::setOutputCallback(OutputCallback callback) {
    std::lock_guard lock(mutex_);
    outputCallback_ = std::move(callback);
}

void ReceiveScheduler::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    workerThread_ = std::thread(&ReceiveScheduler::workerLoop, this);
}

void ReceiveScheduler::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    cv_.notify_all();

    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void ReceiveScheduler::reset() {
    std::lock_guard lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();
    }
    timeSync_.reset();
    cv_.notify_all();
}

void ReceiveScheduler::enqueue(const TimedMidiEvent& event) {
    if ((event.flags & 0x0002) != 0) {
        // Parse JR clock payload: type(2) + drift_ppm(4) + server_clock_ns(8)
        if (event.midiMessage.size() >= 14) {
            const int32_t driftPpm =
                (static_cast<int32_t>(event.midiMessage[2]) << 24) |
                (static_cast<int32_t>(event.midiMessage[3]) << 16) |
                (static_cast<int32_t>(event.midiMessage[4]) << 8)  |
                 static_cast<int32_t>(event.midiMessage[5]);

            uint64_t serverClockNs = 0;
            for (int i = 6; i < 14; ++i) {
                serverClockNs = (serverClockNs << 8) | event.midiMessage[i];
            }

            timeSync_.updateJrClock(serverClockNs, driftPpm);

            //std::cerr << "[ReceiveScheduler] JR Clock frame applied seq=" << event.sequence
            //          << " serverClockNs=" << serverClockNs
            //          << " driftPpm=" << driftPpm << std::endl;
        } else {
            std::cerr << "[ReceiveScheduler] JR Clock frame too short. bytes="
                      << event.midiMessage.size() << std::endl;
        }
        return;
    }

    std::chrono::steady_clock::time_point playAt;
    int64_t jrOffsetNs = 0;

    if (timeSync_.hasJrClock()) {
        const uint64_t extrapolatedJrClockNs = timeSync_.extrapolate_ns();
        jrOffsetNs = static_cast<int64_t>(event.serverTimestampNs)
                   - static_cast<int64_t>(extrapolatedJrClockNs);

        playAt = timeSync_.extrapolate_ns(jrOffsetNs + kDefaultJitterBufferNs);
    } else {
        playAt = timeSync_.computePlayTime(
            event.serverTimestampNs + static_cast<uint64_t>(kDefaultJitterBufferNs));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(ScheduledMidiMessage{
            event.sequence,
            event.serverTimestampNs,
            playAt,
            event.midiMessage
        });

        std::cerr << "[ReceiveScheduler] Enqueued seq=" << event.sequence
                  << " jrOffsetNs=" << jrOffsetNs
                  << " hasJrClock=" << (timeSync_.hasJrClock() ? "yes" : "no")
                  << " queueDepth=" << queue_.size() << std::endl;
    }

    cv_.notify_one();
}

std::size_t ReceiveScheduler::queueDepth() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void ReceiveScheduler::workerLoop() {
    while (running_.load()) {
        std::unique_lock lock(mutex_);

        cv_.wait(lock, [this] {
            return !running_.load() || !queue_.empty();
        });

        while (running_.load() && !queue_.empty()) {
            const auto next = queue_.top();
            const auto now = std::chrono::steady_clock::now();

            if (next.playAt > now) {
                cv_.wait_until(lock, next.playAt, [this, &next] {
                    if (!running_.load() || queue_.empty()) {
                        return true;
                    }

                    const auto& top = queue_.top();
                    if (top.playAt < next.playAt) {
                        return true;
                    }
                    if (top.playAt == next.playAt && top.sequence < next.sequence) {
                        return true;
                    }
                    return false;
                });
                continue;
            }

            queue_.pop();
            auto callback = outputCallback_;

            std::cerr << "[ReceiveScheduler] Releasing seq=" << next.sequence
                      << " remainingQueueDepth=" << queue_.size() << std::endl;

            lock.unlock();

            if (callback) {
                callback(next.midiMessage);
            } else {
                std::cerr << "[ReceiveScheduler] No output callback registered." << std::endl;
            }

            lock.lock();
        }
    }
}
