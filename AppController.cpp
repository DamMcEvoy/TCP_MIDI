#include "AppController.h"
#include "clockState.h"
#include <iostream>
#include <sstream>
#include <utility>
#include <chrono>
#include <thread>

AppController::AppController(const std::string& serverIp, int serverPort)
    : serverIp_(serverIp),
      serverPort_(serverPort) {}

AppController::~AppController() {
    shutdown();
}

bool AppController::initialize() {
    if (initialized_) {
        return true;
    }

    transportClient_ = std::make_unique<TransportClient>(serverIp_, serverPort_);
    timeSync_ = std::make_unique<TimeSync>();
    clockState_ = std::make_unique<ClockState>();
    receiveScheduler_ = std::make_unique<ReceiveScheduler>(*timeSync_, *clockState_);
    midiInputHandler_ = std::make_unique<MidiInputHandler>();
    midiOutputHandler_ = std::make_unique<MidiOutputHandler>();

    libremidi::observer_configuration obsConfig{};
    obsConfig.track_hardware = true;
    obsConfig.track_virtual = true;
    obsConfig.track_any = true;
    observer_ = std::make_unique<libremidi::observer>(obsConfig, libremidi::midi1::default_api());

    initialized_ = true;
    callbacksWired_ = false;
    streaming_ = false;
    hasSelectedInputPort_ = false;
    hasSelectedOutputPort_ = false;

    return true;
}

void AppController::clearGuiLogCallbacks() {
    setSentMidiLogCallback(nullptr);
    setReceivedMidiLogCallback(nullptr);
}

void AppController::setServerConfig(const std::string& serverIp, int serverPort) {
    serverIp_ = serverIp;
    serverPort_ = serverPort;

    if (!initialized_) {
        return;
    }

    if (transportClient_ && transportClient_->isConnected()) {
        std::cerr << "[AppController] Cannot change server config while connected." << std::endl;
        return;
    }

    transportClient_ = std::make_unique<TransportClient>(serverIp_, serverPort_);
}

bool AppController::connect() {
    if (!initialized_ || !transportClient_) {
        std::cerr << "[AppController] Cannot connect before initialization." << std::endl;
        return false;
    }

    if (transportClient_->isConnected()) {
        return true;
    }

    if (!transportClient_->connectToServer()) {
        std::cerr << "[AppController] Failed to connect to server." << std::endl;
        return false;
    }

    std::string clientId;
    if (!transportClient_->getClientId(clientId)) {
        std::cerr << "[AppController] Failed to get client ID." << std::endl;
        transportClient_->disconnect();
        return false;
    }

    std::cout << "Client ID: " << clientId << std::endl;
    return true;
}

void AppController::disconnect() {
    stopStreaming();

    if (transportClient_) {
        transportClient_->setReceiveCallback(nullptr);
        transportClient_->disconnect();
    }

    callbacksWired_ = false;
}

bool AppController::isConnected() const {
    return transportClient_ && transportClient_->isConnected();
}

std::vector<std::string> AppController::getInputPortNames() const {
    std::vector<std::string> names;

    if (!observer_) {
        return names;
    }

    const auto inputPorts = observer_->get_input_ports();
    names.reserve(inputPorts.size());

    for (const auto& port : inputPorts) {
        names.push_back(port.port_name);
    }

    return names;
}

std::vector<std::string> AppController::getOutputPortNames() const {
    std::vector<std::string> names;

    if (!observer_) {
        return names;
    }

    const auto outputPorts = observer_->get_output_ports();
    names.reserve(outputPorts.size());

    for (const auto& port : outputPorts) {
        names.push_back(port.port_name);
    }

    return names;
}

bool AppController::selectInputPort(std::size_t index) {
    if (!observer_) {
        return false;
    }

    const auto inputPorts = observer_->get_input_ports();
    if (index >= inputPorts.size()) {
        std::cerr << "[AppController] Invalid input port selection." << std::endl;
        return false;
    }

    selectedInputPort_ = inputPorts[index];
    hasSelectedInputPort_ = true;
    return true;
}

bool AppController::selectOutputPort(std::size_t index) {
    if (!observer_) {
        return false;
    }

    const auto outputPorts = observer_->get_output_ports();
    if (index >= outputPorts.size()) {
        std::cerr << "[AppController] Invalid output port selection." << std::endl;
        return false;
    }

    selectedOutputPort_ = outputPorts[index];
    hasSelectedOutputPort_ = true;
    return true;
}

bool AppController::openSelectedPorts() {
    if (!midiInputHandler_ || !midiOutputHandler_) {
        std::cerr << "[AppController] MIDI handlers are not initialized." << std::endl;
        return false;
    }

    if (!hasSelectedInputPort_ || !hasSelectedOutputPort_) {
        std::cerr << "[AppController] MIDI ports have not been selected." << std::endl;
        return false;
    }

    if (!midiInputHandler_->openPort(selectedInputPort_)) {
        std::cerr << "[AppController] Failed to open selected MIDI input port." << std::endl;
        return false;
    }

    if (!midiOutputHandler_->openPort(selectedOutputPort_)) {
        std::cerr << "[AppController] Failed to open selected MIDI output port." << std::endl;
        midiInputHandler_->closePort();
        return false;
    }

    return true;
}

void AppController::closeSelectedPorts() {
    if (midiInputHandler_) {
        midiInputHandler_->closePort();
    }

    if (midiOutputHandler_) {
        midiOutputHandler_->closePort();
    }
}

void AppController::wireCallbacks() {
    if (!transportClient_ || !receiveScheduler_ || !midiInputHandler_ || !midiOutputHandler_) {
        std::cerr << "[AppController] Cannot wire callbacks, components missing." << std::endl;
        return;
    }

    midiInputHandler_->setInputCallback([this](const libremidi::midi_bytes& midiMessage) {
        if (!transportClient_ || !transportClient_->isConnected()) {
            std::cerr << "[AppController] Cannot send MIDI, transport disconnected." << std::endl;
            return;
        }

        if (!transportClient_->sendMidiMessage(midiMessage)) {
            std::cerr << "[AppController] Failed to send MIDI message to transport." << std::endl;
        }
    });

    midiInputHandler_->setClockCallback([this](const MidiInputHandler::ClockMessage& clockMessage) {
        handleClockMessage(clockMessage);
    });

    midiInputHandler_->setMessageReceivedCallback([this](const std::string& message) {
        if (receivedMidiLogCallback_) {
            receivedMidiLogCallback_(message);
        }
    });

    midiOutputHandler_->setMessageSentCallback([this](const std::string& message) {
        if (sentMidiLogCallback_) {
            sentMidiLogCallback_(message);
        }
    });

    transportClient_->setReceiveCallback([this](const TimedMidiEvent& event) {
        if (timeSync_ && event.hasJrClock) {
            timeSync_->updateJrClock(event.jrServerClockNs, event.jrFreqPpm);
        }

        if (receiveScheduler_) {
            receiveScheduler_->enqueue(event);
        }
    });

    receiveScheduler_->setOutputCallback([this](const ScheduledMidiMessage& scheduled) {
        sendScheduledMidi(scheduled);
    });

    callbacksWired_ = true;
}

void AppController::sendScheduledMidi(const ScheduledMidiMessage& scheduled) {
    if (!midiOutputHandler_) {
        return;
    }

    const auto targetSendTime =
        scheduled.playAt - std::chrono::nanoseconds(kOutputLeadNs);

    auto now = std::chrono::steady_clock::now();

    if (now < targetSendTime) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::nanoseconds>(targetSendTime - now);

        if (remaining > std::chrono::microseconds(200)) {
            std::this_thread::sleep_until(targetSendTime - std::chrono::microseconds(100));
        }

        while (std::chrono::steady_clock::now() < targetSendTime) {
        }
    }

    now = std::chrono::steady_clock::now();

    const auto sendDeltaNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - scheduled.playAt).count();

    const auto leadAppliedNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(scheduled.playAt - now).count();

    std::cerr << "[AppController] Output send seq=" << scheduled.sequence
              << " sendDeltaNs=" << sendDeltaNs
              << " leadAppliedNs=" << leadAppliedNs
              << " outputLeadNs=" << kOutputLeadNs
              << " estimatedWaitNs=" << scheduled.estimatedWaitNs
              << std::endl;

    if (!midiOutputHandler_->sendMessage(scheduled.midiMessage)) {
        std::cerr << "[AppController] Failed to send MIDI message to output device." << std::endl;
    }
}

bool AppController::startStreaming() {
    if (!initialized_) {
        std::cerr << "[AppController] Application not initialized." << std::endl;
        return false;
    }

    if (!isConnected()) {
        std::cerr << "[AppController] Cannot start streaming while disconnected." << std::endl;
        return false;
    }

    if (streaming_) {
        return true;
    }

    if (!openSelectedPorts()) {
        return false;
    }

    if (!callbacksWired_) {
        wireCallbacks();
    }

    if (!callbacksWired_) {
        closeSelectedPorts();
        return false;
    }

    receiveScheduler_->start();
    streaming_ = true;
    return true;
}

void AppController::stopStreaming() {
    if (transportClient_) {
        transportClient_->setReceiveCallback(nullptr);
    }

    if (receiveScheduler_) {
        receiveScheduler_->stop();
        receiveScheduler_->reset();
        receiveScheduler_->setOutputCallback(nullptr);
    }

    if (midiInputHandler_) {
        midiInputHandler_->setInputCallback(nullptr);
        midiInputHandler_->setClockCallback(nullptr);
        midiInputHandler_->setMessageReceivedCallback(nullptr);
    }

    if (midiOutputHandler_) {
        midiOutputHandler_->setMessageSentCallback(nullptr);
    }

    if (clockState_) {
        clockState_->reset();
    }

    closeSelectedPorts();
    streaming_ = false;
    callbacksWired_ = false;
}

bool AppController::isStreaming() const {
    return streaming_;
}

void AppController::setSentMidiLogCallback(std::function<void(const std::string&)> callback) {
    sentMidiLogCallback_ = std::move(callback);
}

void AppController::setReceivedMidiLogCallback(std::function<void(const std::string&)> callback) {
    receivedMidiLogCallback_ = std::move(callback);
}

void AppController::handleClockMessage(const MidiInputHandler::ClockMessage& clockMessage) {
    if (clockState_) {
        clockState_->handleClockMessage(clockMessage);
    }

    std::ostringstream oss;
    oss << "[Clock] " << clockMessageTypeToString(clockMessage.type);

    if (clockMessage.songPosition >= 0) {
        oss << " spp=" << clockMessage.songPosition;
    }

    if (clockMessage.songSelect >= 0) {
        oss << " song=" << clockMessage.songSelect;
    }

    if (clockState_) {
        const auto snap = clockState_->snapshot();
        oss << " running=" << (snap.running ? "yes" : "no")
            << " pulses=" << snap.pulseCount
            << " qn=" << snap.quarterNoteCount
            << " ppq=" << snap.transportRatePpq;
        if (snap.hasSongPosition) {
            oss << " sppState=" << snap.songPositionBeats;
        }
    }

    if (receivedMidiLogCallback_) {
        receivedMidiLogCallback_(oss.str());
    }
}

const char* AppController::clockMessageTypeToString(MidiInputHandler::ClockMessageType type) {
    switch (type) {
        case MidiInputHandler::ClockMessageType::TimingClock: return "TimingClock";
        case MidiInputHandler::ClockMessageType::Start: return "Start";
        case MidiInputHandler::ClockMessageType::Continue: return "Continue";
        case MidiInputHandler::ClockMessageType::Stop: return "Stop";
        case MidiInputHandler::ClockMessageType::SongPositionPointer: return "SongPositionPointer";
        case MidiInputHandler::ClockMessageType::SongSelect: return "SongSelect";
        case MidiInputHandler::ClockMessageType::TuneRequest: return "TuneRequest";
        case MidiInputHandler::ClockMessageType::ActiveSensing: return "ActiveSensing";
        case MidiInputHandler::ClockMessageType::SystemReset: return "SystemReset";
        default: return "Unknown";
    }
}

void AppController::shutdown() {
    if (!initialized_) {
        return;
    }

    stopStreaming();

    if (transportClient_) {
        transportClient_->setReceiveCallback(nullptr);
        transportClient_->disconnect();
    }

    observer_.reset();
    clockState_.reset();
    midiInputHandler_.reset();
    midiOutputHandler_.reset();
    receiveScheduler_.reset();
    timeSync_.reset();
    transportClient_.reset();

    hasSelectedInputPort_ = false;
    hasSelectedOutputPort_ = false;
    callbacksWired_ = false;
    streaming_ = false;
    initialized_ = false;
}
