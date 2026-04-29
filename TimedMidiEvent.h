#ifndef TIMED_MIDI_EVENT_H
#define TIMED_MIDI_EVENT_H

#include <cstdint>
#include <vector>

struct TimedMidiEvent {
    uint32_t sequence = 0;
    uint64_t serverTimestampNs = 0;
    uint16_t flags = 0;
    std::vector<unsigned char> midiMessage;
};

#endif // TIMED_MIDI_EVENT_H
