#ifndef RECEIVE_SCHEDULER_H
#define RECEIVE_SCHEDULER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include "clockState.h"
#include "timeSync.h"
#include "TimedMidiEvent.h"

struct ScheduledMidiMessage {
    uint32_t sequence = 0;
    uint64_t serverTimestampNs = 0;
    std::chrono::steady_clock::time_point arrivalLocalTime{};
    std::chrono::steady_clock::time_point playAt{};
    int64_t estimatedWaitNs = 0;
    std::vector<uint8_t> midiMessage;
    bool runningAtEnqueue = false;
    bool hasSongPositionAtEnqueue = false;
    uint64_t pulseCountAtEnqueue = 0;
    int songPositionAtEnqueue = -1;
};

struct ScheduledMidiCompare {
    bool operator()(const ScheduledMidiMessage& a, const ScheduledMidiMessage& b) const {
        if (a.sequence != b.sequence) {
            return a.sequence > b.sequence;
        }
        return a.playAt > b.playAt;
    }
};

class ReceiveScheduler {
public:
    using OutputCallback = std::function<void(const ScheduledMidiMessage&)>;

    ReceiveScheduler(TimeSync& timeSync, ClockState& clockState);
    ~ReceiveScheduler();

    void setOutputCallback(OutputCallback callback);
    void start();
    void stop();
    void reset();
    void enqueue(const TimedMidiEvent& event);
    std::size_t queueDepth() const;

private:
    bool shouldFlushForTransportJump(const ClockState::Snapshot& snap) const;
    bool shouldReleaseEvent(const ScheduledMidiMessage& msg, const ClockState::Snapshot& snap) const;
    void flushQueuedEventsLocked(const char* reason);
    void workerLoop();

    TimeSync& timeSync_;
    ClockState& clockState_;
    OutputCallback outputCallback_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<ScheduledMidiMessage,
                        std::vector<ScheduledMidiMessage>,
                        ScheduledMidiCompare> queue_;

    std::atomic<bool> running_;
    std::thread workerThread_;

    bool lastRunningState_ = false;
    bool haveLastSongPosition_ = false;
    int lastSongPosition_ = -1;
};

#endif // RECEIVE_SCHEDULER_H
