#ifndef TIMESYNC_H
#define TIMESYNC_H

#include <chrono>
#include <cstdint>
#include <mutex>

class TimeSync {
public:
    TimeSync();

    void reset();
    bool anchored() const;
    bool hasJrClock() const;

    void updateJrClock(uint64_t serverClockNs, int32_t freqPpm);
    uint64_t extrapolate_ns() const;
    std::chrono::steady_clock::time_point extrapolate_ns(int64_t offsetNs) const;
    std::chrono::steady_clock::time_point computePlayTime(uint64_t serverTimestampNs);

private:
    mutable std::mutex mutex_;

    bool anchorValid_;
    uint64_t firstServerTimestampNs_;
    std::chrono::steady_clock::time_point anchorLocalPlayTime_;
    std::chrono::milliseconds initialJitterBuffer_;

    bool jrClockValid_;
    uint64_t jrServerClockNs_;
    int32_t jrFreqPpm_;
    std::chrono::steady_clock::time_point jrLocalArrival_;
};

#endif // TIMESYNC_H
