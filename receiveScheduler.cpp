#include "receiveScheduler.h"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <utility>

namespace {
constexpr int64_t kDefaultJitterBufferNs = 80'000'000; // 80 ms
}

ReceiveScheduler::ReceiveScheduler(TimeSync& timeSync, ClockState& clockState)
    : timeSync_(timeSync),
      clockState_(clockState),
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
    flushQueuedEventsLocked("reset");
    timeSync_.reset();
    lastRunningState_ = false;
    haveLastSongPosition_ = false;
    lastSongPosition_ = 0;
    cv_.notify_all();
}

bool ReceiveScheduler::shouldFlushForTransportJump(const ClockState::Snapshot& snap) const {
    if (snap.running != lastRunningState_) {
        return true;
    }

    if (snap.hasSongPosition) {
        if (!haveLastSongPosition_) {
            return true;
        }
        if (snap.songPositionBeats != lastSongPosition_) {
            return true;
        }
    }

    return false;
}

bool ReceiveScheduler::shouldReleaseEvent(const ScheduledMidiMessage& msg,
                                          const ClockState::Snapshot& snap) const {
    if (snap.hasSongPosition && msg.hasSongPositionAtEnqueue) {
        if (snap.songPositionBeats != msg.songPositionAtEnqueue &&
            snap.pulseCount < msg.pulseCountAtEnqueue) {
            return false;
        }
    }
    return true;
}

void ReceiveScheduler::flushQueuedEventsLocked(const char* reason) {
    if (!queue_.empty()) {
        std::cerr << "[ReceiveScheduler] Flushing queue. reason=" << reason
                  << " flushed=" << queue_.size() << std::endl;
    }

    while (!queue_.empty()) {
        queue_.pop();
    }
}

void ReceiveScheduler::enqueue(const TimedMidiEvent& event) {
    if (event.hasJrClock) {
        if (event.midiMessage.empty()) {
            std::cerr << "[ReceiveScheduler] Received JR timing event with no MIDI payload." << std::endl;
        }
        return;
    }

    const auto snap = clockState_.snapshot();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (shouldFlushForTransportJump(snap)) {
            if (!snap.running) {
                flushQueuedEventsLocked("transport-stop");
            } else if (snap.hasSongPosition && haveLastSongPosition_ && snap.songPositionBeats != lastSongPosition_) {
                flushQueuedEventsLocked("song-position-change");
            }
        }

        lastRunningState_ = snap.running;
        if (snap.hasSongPosition) {
            haveLastSongPosition_ = true;
            lastSongPosition_ = snap.songPositionBeats;
        }
    }

    std::chrono::steady_clock::time_point playAt;
    int64_t jrOffsetNs = 0;

    constexpr auto kMaxFreshJrAge = std::chrono::milliseconds(250);
    const auto schedulingMode = 
        timeSync_.schedulingMode(
            std::chrono::duration_cast<std::chrono::nanoseconds>(kMaxFreshJrAge));
    const auto jrAge = timeSync_.jrSampleAge();

    if (timeSync_.hasJrClock() && 
        schedulingMode == TimeSync::SchedulingMode::Anchor) {
        std::cerr << "[ReceiveScheduler] JR clock stale. Falling back to anchor scheduling. "
            << "jrSampleAgeNs=" << jrAge.count()
            << " maxFreshJrAgeNs="
            << std::chrono::duration_cast<std::chrono::nanoseconds>(kMaxFreshJrAge).count()
            << std::endl;
    }

    const char* schedulingModeText = "none";

    switch (schedulingMode) {
    case TimeSync::SchedulingMode::FreshJr: {
        schedulingModeText = "fresh-jr";
        const uint64_t extrapolatedJrClocks = timeSync_.extrapolate_ns();
        jrOffsetNs = static_cast<int64_t>(event.serverTimestampNs) - static_cast<int64_t>(extrapolatedJrClocks);

        playAt = timeSync_.extrapolate_ns(jrOffsetNs + kDefaultJitterBufferNs);

        break;
    }

    case TimeSync::SchedulingMode::Anchor:
    schedulingModeText = "anchor";
    playAt = timeSync_.computePlayTime(
        event.serverTimestampNs + static_cast<uint64_t>(kDefaultJitterBufferNs));

        break;

    case TimeSync::SchedulingMode::None:
        schedulingModeText = "none";
        playAt = std::chrono::steady_clock::now() + std::chrono::nanoseconds(kDefaultJitterBufferNs);
        std::cerr << "[ReceiveScheduler] No valid timing model available. " 
        << "Using immediate local fallback scheduling. " 
        << std::endl;

        break;
    }

    const auto estimatedWaitNs = 
        std::chrono::duration_cast<std::chrono::nanoseconds>(playAt - event.arrivalLocalTime).count();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(ScheduledMidiMessage{
            event.sequence,
            event.serverTimestampNs,
            event.arrivalLocalTime,
            playAt,
            estimatedWaitNs,
            event.midiMessage,
            snap.running,
            snap.hasSongPosition,
            snap.pulseCount,
            snap.songPositionBeats
        });

        std::cerr << "[ReceiveScheduler] Enqueued seq=" << event.sequence
                  << " schedulingMode=" << schedulingModeText
                  << " jrOffsetNs=" << jrOffsetNs
                  << " estimatedWaitNs=" << estimatedWaitNs
                  << " jrSampleAgeNs=" << jrAge.count()
                  << " transportRunning=" << (snap.running ? "yes" : "no")
                  << " pulseCount=" << snap.pulseCount
                  << " queueDepth=" << queue_.size() 
                  << std::endl;
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
                if (top.sequence < next.sequence) {
                    return true;
                }

                return false;
            });
            continue;
        }

            queue_.pop();
            auto callback = outputCallback_;
            const auto snap = clockState_.snapshot();

            if (!shouldReleaseEvent(next, snap)) {
                std::cerr << "[ReceiveScheduler] Dropping seq=" << next.sequence
                          << " at release. transportRunning=" << (snap.running ? "yes" : "no")
                          << " pulseCount=" << snap.pulseCount
                          << " remainingQueueDepth=" << queue_.size() << std::endl;
                continue;
            }

            std::cerr << "[ReceiveScheduler] Releasing seq=" << next.sequence
                      << " remainingQueueDepth=" << queue_.size()
                      << " pulseCount=" << snap.pulseCount << std::endl;

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
