/*
MidiOutputHandler is intentionally narrow: it owns libremidi::midi_in, 
tracks whether the input port is open, 
and exposes a Inpu callback for the scheduler to use. 
That keeps playout device I/O out of both the transport layer and the timing layer.
*/

#include "midiInputHandler.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <utility>

MidiInputHandler::MidiInputHandler()
    : open_(false),
      inputCallback_(nullptr),
      messageReceivedCallback_(nullptr),
      clockCallback_(nullptr),
      midiIn_(nullptr) {
    config_.on_message = [this](const libremidi::message& msg) {
        if (msg.bytes.empty()) {
            return;
        }

        if (clockCallback_ && isClockRelatedMessage(msg.bytes)) {
            clockCallback_(parseClockMessage(msg.bytes));
        }

        if (inputCallback_) {
            inputCallback_(msg.bytes);
        }

        if (messageReceivedCallback_) {
            messageReceivedCallback_(formatMidiMessage(msg.bytes));
        }
    };
}

MidiInputHandler::~MidiInputHandler() {
    closePort();
}

bool MidiInputHandler::openPort(const libremidi::input_port& port) {
    closePort();

    try {
        midiIn_ = std::make_unique<libremidi::midi_in>(config_);
        auto err = midiIn_->open_port(port);
        if (err != stdx::error{}) {
            std::cerr << "[MidiInputHandler] Failed to open input port: "
                      << err.message().data() << std::endl;
            midiIn_.reset();
            open_ = false;
            return false;
        }

        open_ = true;
        std::cerr << "[MidiInputHandler] Opened input port: "
                  << port.port_name << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[MidiInputHandler] Exception opening input port: "
                  << e.what() << std::endl;
        midiIn_.reset();
        open_ = false;
        return false;
    } catch (...) {
        std::cerr << "[MidiInputHandler] Unknown exception opening input port."
                  << std::endl;
        midiIn_.reset();
        open_ = false;
        return false;
    }
}

void MidiInputHandler::closePort() {
    if (!midiIn_) {
        open_ = false;
        return;
    }

    try {
        midiIn_->close_port();
    } catch (const std::exception& e) {
        std::cerr << "[MidiInputHandler] Exception closing input port: "
                  << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[MidiInputHandler] Unknown exception closing input port."
                  << std::endl;
    }

    midiIn_.reset();
    open_ = false;
}

bool MidiInputHandler::isOpen() const {
    return open_;
}

void MidiInputHandler::setInputCallback(InputCallback callback) {
    inputCallback_ = std::move(callback);
}

void MidiInputHandler::setMessageReceivedCallback(MessageReceivedCallback callback) {
    messageReceivedCallback_ = std::move(callback);
}

void MidiInputHandler::setClockCallback(ClockCallback callback) {
    clockCallback_ = std::move(callback);
}

bool MidiInputHandler::isClockRelatedMessage(const std::vector<unsigned char>& midiMessage) {
    if (midiMessage.empty()) {
        return false;
    }

    switch (midiMessage[0]) {
        case 0xF2: // Song Position Pointer
        case 0xF3: // Song Select
        case 0xF6: // Tune Request
        case 0xF8: // Timing Clock
        case 0xFA: // Start
        case 0xFB: // Continue
        case 0xFC: // Stop
        case 0xFE: // Active Sensing
        case 0xFF: // System Reset
            return true;
        default:
            return false;
    }
}

MidiInputHandler::ClockMessage MidiInputHandler::parseClockMessage(
    const std::vector<unsigned char>& midiMessage) {
    ClockMessage message{
        classifyClockMessage(midiMessage.empty() ? 0x00 : midiMessage[0]),
        midiMessage,
        -1,
        -1
    };

    if (midiMessage.empty()) {
        return message;
    }

    if (midiMessage[0] == 0xF2 && midiMessage.size() >= 3) {
        message.songPosition =
            static_cast<int>(midiMessage[1]) |
            (static_cast<int>(midiMessage[2]) << 7);
    }

    if (midiMessage[0] == 0xF3 && midiMessage.size() >= 2) {
        message.songSelect = static_cast<int>(midiMessage[1]);
    }

    return message;
}

MidiInputHandler::ClockMessageType MidiInputHandler::classifyClockMessage(unsigned char status) {
    switch (status) {
        case 0xF8: return ClockMessageType::TimingClock;
        case 0xFA: return ClockMessageType::Start;
        case 0xFB: return ClockMessageType::Continue;
        case 0xFC: return ClockMessageType::Stop;
        case 0xF2: return ClockMessageType::SongPositionPointer;
        case 0xF3: return ClockMessageType::SongSelect;
        case 0xF6: return ClockMessageType::TuneRequest;
        case 0xFE: return ClockMessageType::ActiveSensing;
        case 0xFF: return ClockMessageType::SystemReset;
        default:   return ClockMessageType::Unknown;
    }
}

std::string MidiInputHandler::formatMidiMessage(
    const std::vector<unsigned char>& midiMessage) const {
    std::ostringstream oss;
    oss << "RX:";
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