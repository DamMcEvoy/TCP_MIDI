/*
midiOutputHandler is intentionally narrow: it owns libremidi::midi_out, 
tracks whether the output port is open, 
and exposes a simple sendMessage() method for the scheduler callback to use. 
That keeps playout device I/O out of both the transport layer and the timing layer.
*/

#ifndef MIDI_OUTPUT_HANDLER_H
#define MIDI_OUTPUT_HANDLER_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <libremidi/libremidi.hpp>

class MidiOutputHandler {
public:
    MidiOutputHandler();
    ~MidiOutputHandler();

    bool openPort(const libremidi::output_port& port);
    void closePort();
    bool isOpen() const;
    bool sendMessage(const std::vector<unsigned char>& midiMessage);
    void setMessageSentCallback(std::function<void(const std::string&)> callback);

private:
    bool open_;
    std::unique_ptr<libremidi::midi_out> midiOut_;
    std::function<void(const std::string&)> messageSentCallback_;
    std::string formatMidiMessage(const std::vector<unsigned char>& midiMessage) const;
};

#endif // MIDI_OUTPUT_HANDLER_H