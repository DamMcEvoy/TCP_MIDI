#ifndef CLOCK_STATE_H
#define CLOCK_STATE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

#include "midiInputHandler.h"

class ClockState {
public:
    struct Snapshot {
        bool running = false;
        bool hasSongPosition = false;
        uint64_t pulseCount = 0;
        uint64_t quarterNoteCount = 0;
        uint64_t barBeatPulse = 0;
        int songPositionBeats = -1;
        double transportRatePpq = 24.0;
    };

    ClockState();

    void reset();
    void handleClockMessage(const MidiInputHandler::ClockMessage& clockMessage);
    Snapshot snapshot() const;

    bool isRunning() const;
    uint64_t pulseCount() const;
    double transportRatePpq() const;

private:
    static constexpr uint32_t kMidiClockPpq = 24;

    mutable std::mutex mutex_;
    bool running_ = false;
    bool hasSongPosition_ = false;
    uint64_t pulseCount_ = 0;
    int songPositionBeats_ = -1;
    double transportRatePpq_ = static_cast<double>(kMidiClockPpq);

    void applySongPositionPointer(int songPositionBeats);
};

#endif // CLOCK_STATE_H
