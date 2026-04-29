#include "AppController.h"

#include <iostream>
#include <utility>

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
    receiveScheduler_ = std::make_unique<ReceiveScheduler>(*timeSync_);
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

    midiInputHandler_->setInputCallback([this](const std::vector<uint8_t>& midiMessage) {
        if (!transportClient_ || !transportClient_->isConnected()) {
            std::cerr << "[AppController] Cannot send MIDI, transport disconnected." << std::endl;
            return;
        }

        if (!transportClient_->sendMidiMessage(midiMessage)) {
            std::cerr << "[AppController] Failed to send MIDI message to transport." << std::endl;
        }
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
        if (receiveScheduler_) {
            receiveScheduler_->enqueue(event);
        }
    });

    receiveScheduler_->setOutputCallback([this](const std::vector<uint8_t>& midiMessage) {
        if (!midiOutputHandler_) {
            return;
        }

        if (!midiOutputHandler_->sendMessage(midiMessage)) {
            std::cerr << "[AppController] Failed to send MIDI message to output device." << std::endl;
        }
    });

    callbacksWired_ = true;
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
    if (receiveScheduler_) {
        receiveScheduler_->stop();
        receiveScheduler_->reset();
        receiveScheduler_->setOutputCallback(nullptr);
    }

    if (midiInputHandler_) {
        midiInputHandler_->setInputCallback(nullptr);
    }

    if (midiOutputHandler_) {
        midiOutputHandler_->setMessageSentCallback(nullptr);
    }

    if (midiInputHandler_) {
        midiInputHandler_->setMessageReceivedCallback(nullptr);
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