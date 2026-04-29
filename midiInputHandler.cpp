#include "midiInputHandler.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <utility>

MidiInputHandler::MidiInputHandler()
    : open_(false),
      inputCallback_(nullptr),
      messageReceivedCallback_(nullptr),
      midiIn_(nullptr) {
    config_.on_message = [this](const libremidi::message& msg) {
        if (msg.bytes.empty()) {
            return;
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

std::string MidiInputHandler::formatMidiMessage(const std::vector<unsigned char>& midiMessage) const {
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