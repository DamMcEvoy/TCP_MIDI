#ifndef TIMED_MIDI_EVENT_H
#define TIMED_MIDI_EVENT_H

#include <chrono>
#include <cstdint>
#include <vector>

namespace MidiFrameFlags {
constexpr uint16_t RawMidi = 0x0001u;
constexpr uint16_t JrClock = 0x0002u;
constexpr uint16_t RecoveryJournal = 0x0004u;
}

struct TimedMidiEvent {
    std::vector<uint8_t> midiMessage;
    std::vector<uint8_t> journalBytes;

    uint32_t sequence = 0;
    uint64_t serverTimestampNs = 0;
    uint16_t flags = 0;

    std::chrono::steady_clock::time_point arrivalLocalTime{};

    bool hasJrClock = false;
    int32_t jrFreqPpm = 0;
    uint64_t jrServerClockNs = 0;

    uint8_t journalType = 0;
    uint32_t transportEpoch = 0;
    bool transportRunning = false;
    bool hasSongPosition = false;
    int32_t songPositionBeats = -1;
    bool transportDiscontinuity = false;

    [[nodiscard]] bool isRawMidi() const noexcept {
        return (flags & MidiFrameFlags::RawMidi) != 0;
    }

    [[nodiscard]] bool isJrClockOnly() const noexcept {
        return (flags & MidiFrameFlags::JrClock) != 0 &&
               (flags & MidiFrameFlags::RawMidi) == 0 &&
               (flags & MidiFrameFlags::RecoveryJournal) == 0;
    }

    [[nodiscard]] bool hasRecoveryJournal() const noexcept {
        return (flags & MidiFrameFlags::RecoveryJournal) != 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return midiMessage.empty() &&
               journalBytes.empty() &&
               !hasJrClock;
    }
};

#endif // TIMED_MIDI_EVENT_H