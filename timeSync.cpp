#include "timeSync.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>

TimeSync::TimeSync()
    : anchorValid_(false),
      firstServerTimestampNs_(0),
      anchorLocalPlayTime_(std::chrono::steady_clock::time_point{}),
      initialJitterBuffer_(80),
      jrClockValid_(false),
      jrServerClockNs_(0),
      jrFreqPpm_(0),
      jrLocalArrival_(std::chrono::steady_clock::time_point{}) {}

void TimeSync::reset() {
    std::lock_guard lock(mutex_);
    anchorValid_ = false;
    firstServerTimestampNs_ = 0;
    anchorLocalPlayTime_ = std::chrono::steady_clock::time_point{};

    jrClockValid_ = false;
    jrServerClockNs_ = 0;
    jrFreqPpm_ = 0;
    jrLocalArrival_ = std::chrono::steady_clock::time_point{};
}

bool TimeSync::anchored() const {
    std::lock_guard lock(mutex_);
    return anchorValid_;
}

bool TimeSync::hasJrClock() const {
    std::lock_guard lock(mutex_);
    return jrClockValid_;
}

void TimeSync::updateJrClock(uint64_t serverClockNs, int32_t freqPpm) {
    std::lock_guard lock(mutex_);
    jrServerClockNs_ = serverClockNs;
    jrFreqPpm_ = freqPpm;
    jrLocalArrival_ = std::chrono::steady_clock::now();
    jrClockValid_ = true;

    //std::cerr << "[TimeSync] JR clock updated. serverClockNs=" << jrServerClockNs_
    //          << " freqPpm=" << jrFreqPpm_ << std::endl;
}

uint64_t TimeSync::extrapolate_ns() const {
    std::lock_guard lock(mutex_);

    if (!jrClockValid_) {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - jrLocalArrival_).count();

    const long double rate = 1.0L + (static_cast<long double>(jrFreqPpm_) / 1'000'000.0L);
    const long double advanced = static_cast<long double>(jrServerClockNs_) + static_cast<long double>(elapsedNs) * rate;

    if (advanced <= 0.0L) {
        return 0;
    }
    if (advanced >= static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }

    return static_cast<uint64_t>(advanced);
}

std::chrono::steady_clock::time_point TimeSync::extrapolate_ns(int64_t offsetNs) const {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();

    if (!jrClockValid_) {
        return now + std::chrono::nanoseconds(offsetNs);
    }

    const long double rate = 1.0L + (static_cast<long double>(jrFreqPpm_) / 1'000'000.0L);
    const long double adjustedOffset = static_cast<long double>(offsetNs) / rate;
    return now + std::chrono::nanoseconds(static_cast<int64_t>(adjustedOffset));
}

std::chrono::steady_clock::time_point TimeSync::computePlayTime(uint64_t serverTimestampNs) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();

    if (!anchorValid_) {
        firstServerTimestampNs_ = serverTimestampNs;
        anchorLocalPlayTime_ = now + initialJitterBuffer_;
        anchorValid_ = true;

        std::cerr << "[TimeSync] Clock anchor established. firstServerTimestampNs="
                  << firstServerTimestampNs_ << std::endl;
        return anchorLocalPlayTime_;
    }

    if (serverTimestampNs < firstServerTimestampNs_) {
        std::cerr << "[TimeSync] Received older-than-anchor timestamp. Scheduling immediately."
                  << std::endl;
        return now;
    }

    const uint64_t deltaNs = serverTimestampNs - firstServerTimestampNs_;
    return anchorLocalPlayTime_ + std::chrono::nanoseconds(deltaNs);
}
