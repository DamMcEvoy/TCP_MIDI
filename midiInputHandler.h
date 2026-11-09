/*
midiInputHandler is intentionally narrow: it owns libremidi::midi_in, 
tracks whether the input port is open, 
and exposes a Inpu callback for the scheduler to use. 
That keeps playout device I/O out of both the transport layer and the timing layer.
*/

#ifndef MIDI_INPUT_HANDLER_H
#define MIDI_INPUT_HANDLER_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <libremidi/libremidi.hpp>

class MidiInputHandler {
public:
    using InputCallback = std::function<void(const std::vector<unsigned char>&)>;
    using MessageReceivedCallback = std::function<void(const std::string&)>;

    enum class ClockMessageType {
        TimingClock,
        Start,
        Continue,
        Stop,
        SongPositionPointer,
        SongSelect,
        TuneRequest,
        ActiveSensing,
        SystemReset,
        Unknown
    };

    struct ClockMessage {
        ClockMessageType type;
        std::vector<unsigned char> bytes;
        int songPosition = -1;
        int songSelect = -1;
    };

    using ClockCallback = std::function<void(const ClockMessage&)>;

    MidiInputHandler();
    ~MidiInputHandler();

    bool openPort(const libremidi::input_port& port);
    void closePort();
    bool isOpen() const;

    void setInputCallback(InputCallback callback);
    void setMessageReceivedCallback(MessageReceivedCallback callback);
    void setClockCallback(ClockCallback callback);

private:
    bool open_;
    InputCallback inputCallback_;
    MessageReceivedCallback messageReceivedCallback_;
    ClockCallback clockCallback_;
    std::unique_ptr<libremidi::midi_in> midiIn_;
    libremidi::input_configuration config_;

    static bool isClockRelatedMessage(const std::vector<unsigned char>& midiMessage);
    static ClockMessage parseClockMessage(const std::vector<unsigned char>& midiMessage);
    static ClockMessageType classifyClockMessage(unsigned char status);
    std::string formatMidiMessage(const std::vector<unsigned char>& midiMessage) const;
};

#endif // MIDI_INPUT_HANDLER_H