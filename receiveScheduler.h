/*
ReceiveScheduler accepts a TimedMidiEvent, asks TimeSync for its local playAt time, 
pushes a ScheduledMidiMessage into a priority queue, and releases events in time order through a registered output callback. 
The receive side is cleanly split into timestamp mapping in TimeSync and playout orchestration in ReceiveScheduler
*/

#ifndef RECEIVE_SCHEDULER_H
#define RECEIVE_SCHEDULER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <vector>
#include <thread>

#include "timeSync.h"

#include "TimedMidiEvent.h"

struct ScheduledMidiMessage {
    uint32_t sequence = 0;
    uint64_t serverTimestampNs = 0;
    std::chrono::steady_clock::time_point playAt;
    std::vector<unsigned char> midiMessage;
};

struct ScheduledMidiCompare {
    bool operator()(const ScheduledMidiMessage& a, const ScheduledMidiMessage& b) const {
        if (a.playAt != b.playAt) {
            return a.playAt > b.playAt;
        }
        return a.sequence > b.sequence;
    }
};

class ReceiveScheduler {
public:
    using OutputCallback = std::function<void(const std::vector<unsigned char>&)>;

    explicit ReceiveScheduler(TimeSync& timeSync);
    ~ReceiveScheduler();

    void setOutputCallback(OutputCallback callback);
    void start();
    void stop();
    void reset();
    void enqueue(const TimedMidiEvent& event);
    std::size_t queueDepth() const;

private:
    void workerLoop();

    TimeSync& timeSync_;
    OutputCallback outputCallback_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::priority_queue<ScheduledMidiMessage,
                        std::vector<ScheduledMidiMessage>,
                        ScheduledMidiCompare> queue_;

    std::atomic<bool> running_;
    std::thread workerThread_;
};

#endif // RECEIVE_SCHEDULER_H