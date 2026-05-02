#include "midiOutputHandler.h"

/*
MidiOutputHandler is intentionally narrow: it owns libremidi::midi_out, 
tracks whether the output port is open, 
and exposes a simple sendMessage() method for the scheduler callback to use. 
That keeps playout device I/O out of both the transport layer and the timing layer.
*/

#include <iostream>
#include <iomanip>
#include <sstream>
#include <utility>

MidiOutputHandler::MidiOutputHandler()
    : open_(false),
      midiOut_(std::make_unique<libremidi::midi_out>()) {}

MidiOutputHandler::~MidiOutputHandler() {
    closePort();
}

bool MidiOutputHandler::openPort(const libremidi::output_port& port) {
    closePort();

    try {
        auto err = midiOut_->open_port(port);
        if (err != stdx::error{}) {
            std::cerr << "[MidiOutputHandler] Failed to open output port: "
                      << err.message().data() << std::endl;
            open_ = false;
            return false;
        }

        open_ = true;
        std::cerr << "[MidiOutputHandler] Opened output port: "
                  << port.port_name << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[MidiOutputHandler] Exception opening output port: "
                  << e.what() << std::endl;
        open_ = false;
        return false;
    } catch (...) {
        std::cerr << "[MidiOutputHandler] Unknown exception opening output port."
                  << std::endl;
        open_ = false;
        return false;
    }
}

void MidiOutputHandler::closePort() {
    if (!midiOut_) {
        open_ = false;
        return;
    }

    try {
        midiOut_->close_port();
    } catch (const std::exception& e) {
        std::cerr << "[MidiOutputHandler] Exception closing output port: "
                  << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[MidiOutputHandler] Unknown exception closing output port."
                  << std::endl;
    }

    open_ = false;
}

bool MidiOutputHandler::isOpen() const {
    return open_;
}

bool MidiOutputHandler::sendMessage(const std::vector<unsigned char>& midiMessage) {
    if (!open_ || !midiOut_) {
        std::cerr << "[MidiOutputHandler] Output port is not open." << std::endl;
        return false;
    }

    if (midiMessage.empty()) {
        std::cerr << "[MidiOutputHandler] Ignoring empty MIDI message." << std::endl;
        return false;
    }

    try {
        midiOut_->send_message(midiMessage);

        if (messageSentCallback_) {
            messageSentCallback_(formatMidiMessage(midiMessage));
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[MidiOutputHandler] Failed to send MIDI message: "
                  << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[MidiOutputHandler] Unknown failure sending MIDI message."
                  << std::endl;
        return false;
    }
}

void MidiOutputHandler::setMessageSentCallback(std::function<void(const std::string&)> callback) {
    messageSentCallback_ = std::move(callback);
}

std::string MidiOutputHandler::formatMidiMessage(const std::vector<unsigned char>& midiMessage) const {
    std::ostringstream oss;
    oss << "TX:";
    for (unsigned char byte : midiMessage) {
        oss << ' '
            << std::uppercase
            << std::hex
            << std::setw(2)
            << std::setfill('0')
            << static_cast<int>(byte);
    }
    return oss.str();
}