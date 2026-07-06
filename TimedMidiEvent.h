#ifndef TIMED_MIDI_EVENT_H
#define TIMED_MIDI_EVENT_H

#include <chrono>
#include <cstdint>
#include <vector>

struct TimedMidiEvent {
    uint32_t sequence = 0; //unsigned fixed width 32 bit integer
    uint64_t serverTimestampNs = 0;
    uint16_t flags = 0;
    std::vector<uint8_t> midiMessage;

    //local arrival timestamp on the receiving client
    std::chrono::steady_clock::time_point arrivalLocalTime{};

    //optional JR clock metadata, if the received frame carries it
    bool hasJrClock = false;
    uint64_t jrServerClockNs = 0;
    int32_t jrFreqPpm = 0;

    // optional transport / recovery metadata for richer frame packets
    uint64_t transportEpoch = 0;
    bool transportRunning = false;
    bool hasSongPosition = false;
    int songPositionBeats = -1;
    bool transportDiscontinuity = false;

    //optional sender timing context
    double senderTempoBpm = 0.0;
    uint32_t senderPpqn = 24;

    //optional journal payload
    uint16_t journalType = 0;
    std::vector<uint8_t> journalBytes;

};

#endif // TIMED_MIDI_EVENT_H
