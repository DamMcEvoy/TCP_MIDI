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
      jrLocalArrival_(std::chrono::steady_clock::time_point{}),
      jrFilterInitialized_(false),
      filteredJrServerClockNs_(0.0L),
      filteredJrLocalArrival_(std::chrono::steady_clock::time_point{}) {} 


void TimeSync::reset() {
    std::lock_guard lock(mutex_);
    anchorValid_ = false;
    firstServerTimestampNs_ = 0;
    anchorLocalPlayTime_ = std::chrono::steady_clock::time_point{};

    jrClockValid_ = false;
    jrServerClockNs_ = 0;
    jrFreqPpm_ = 0;
    jrLocalArrival_ = std::chrono::steady_clock::time_point{};

    jrFilterInitialized_ = false;
    filteredJrServerClockNs_ = 0.0L;
    filteredJrLocalArrival_ = std::chrono::steady_clock::time_point{};
}

bool TimeSync::anchored() const {
    std::lock_guard lock(mutex_);
    return anchorValid_;
}

bool TimeSync::hasJrClock() const {
    std::lock_guard lock(mutex_);
    return jrClockValid_;
}

bool TimeSync::hasFreshJrClock(std::chrono::nanoseconds maxAge) const {
    std::lock_guard lock(mutex_);

    if (!jrClockValid_) {
        return false;
    }

    const auto age = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - jrLocalArrival_
    );

    return age <= maxAge;
}

TimeSync::Snapshot TimeSync::snapshot() const{
    std::lock_guard lock(mutex_);

    Snapshot snap;
    snap.anchorValid = anchorValid_;
    snap.firstServerTimestampNs = firstServerTimestampNs_;
    snap.jrClockValid = jrClockValid_;
    snap.jrServerClockNs = jrServerClockNs_;
    snap.jrFreqPpm = jrFreqPpm_;
    snap.jrLocalArrival = jrLocalArrival_;
    snap.initialJitterBuffer = initialJitterBuffer_;
    return snap;
}

std::chrono::nanoseconds TimeSync::jrSampleAge() const {
    std::lock_guard lock(mutex_);

    if (!jrClockValid_) {
        return std::chrono::nanoseconds{0};
    }

    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - jrLocalArrival_
    );
}

TimeSync::SchedulingMode
TimeSync::schedulingMode(std::chrono::nanoseconds maxFreshJrAge) const {
    std::lock_guard lock(mutex_);

    if (jrClockValid_) {
        const auto age = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - jrLocalArrival_);

        if (age <= maxFreshJrAge) {
            return SchedulingMode::FreshJr;
        }
    }

    if (anchorValid_) {
        return SchedulingMode::Anchor;
    }

    return SchedulingMode::None;
}

void TimeSync::updateJrClock(uint64_t serverClockNs, int32_t freqPpm) {
    std::lock_guard lock(mutex_);

    const auto now = std::chrono::steady_clock::now();

    jrServerClockNs_ = serverClockNs;
    jrFreqPpm_ = freqPpm;
    jrLocalArrival_ = now;
    jrClockValid_ = true;

    int64_t localDeltaNs = 0;
    long double filterBeforeCorrection = 0.0L;
    long double serverErrorNs = 0.0L;
    bool appliedCorrection = false;

    if (!jrFilterInitialized_) {
        filteredJrServerClockNs_ = static_cast<long double>(serverClockNs);
        filteredJrLocalArrival_ = now;
        jrFilterInitialized_ = true;
    } else {
        constexpr long double alpha = 0.2L;

        localDeltaNs = 
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - filteredJrLocalArrival_).count();
        
        filteredJrServerClockNs_ += static_cast<long double>(localDeltaNs);
        filterBeforeCorrection = filteredJrServerClockNs_;

        serverErrorNs = static_cast<long double>(serverClockNs) - filterBeforeCorrection;

        filteredJrServerClockNs_ += alpha * serverErrorNs;
        filteredJrLocalArrival_ = now;
        appliedCorrection = true;
    }

    if (!anchorValid_) {
        firstServerTimestampNs_ = serverClockNs;
        anchorLocalPlayTime_ = now + initialJitterBuffer_;
        anchorValid_ = true;
    }

    std::cerr << "[TimeSync] JR update"
          << " rawServerNs=" << serverClockNs
          << " localDeltaNs=" << localDeltaNs
          << " predictedFilteredNs=" << static_cast<uint64_t>(filterBeforeCorrection)
          << " serverErrorNs=" << static_cast<int64_t>(serverErrorNs)
          << " correctedFilteredNs=" << static_cast<uint64_t>(filteredJrServerClockNs_)
          << " freqPpm=" << jrFreqPpm_
          << " appliedCorrection=" << (appliedCorrection ? "yes" : "no")
          << std::endl;
}

uint64_t TimeSync::extrapolate_ns() const {
    std::lock_guard lock(mutex_);

    if (!jrClockValid_ || !jrFilterInitialized_) {
        return 0;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - filteredJrLocalArrival_).count();

    const long double rate = 1.0L + (static_cast<long double>(jrFreqPpm_) / 1'000'000.0L);
    const long double advanced = filteredJrServerClockNs_ + static_cast<long double>(elapsedNs) * rate;

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
