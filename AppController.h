#ifndef APP_CONTROLLER_H
#define APP_CONTROLLER_H

#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "transportClient.h"
#include "timeSync.h"
#include "receiveScheduler.h"
#include "midiInputHandler.h"
#include "midiOutputHandler.h"

class AppController {
public:
    AppController(const std::string& serverIp, int serverPort);
    ~AppController();

    bool initialize();
    void shutdown();

    void setServerConfig(const std::string& serverIp, int serverPort);

    bool connect();
    void disconnect();
    bool isConnected() const;

    std::vector<std::string> getInputPortNames() const;
    std::vector<std::string> getOutputPortNames() const;

    bool selectInputPort(std::size_t index);
    bool selectOutputPort(std::size_t index);

    bool startStreaming();
    void stopStreaming();
    bool isStreaming() const;
    void setSentMidiLogCallback(std::function<void(const std::string&)> callback);
    void setReceivedMidiLogCallback(std::function<void(const std::string&)> callback);
    void clearGuiLogCallbacks();
    

private:
    bool openSelectedPorts();
    void closeSelectedPorts();
    void wireCallbacks();

    std::string serverIp_;
    int serverPort_;

    std::unique_ptr<TransportClient> transportClient_;
    std::unique_ptr<TimeSync> timeSync_;
    std::unique_ptr<ReceiveScheduler> receiveScheduler_;
    std::unique_ptr<MidiInputHandler> midiInputHandler_;
    std::unique_ptr<MidiOutputHandler> midiOutputHandler_;
    std::unique_ptr<libremidi::observer> observer_;
    std::function<void(const std::string&)> sentMidiLogCallback_;
    std::function<void(const std::string&)> receivedMidiLogCallback_;

    libremidi::input_port selectedInputPort_{};
    libremidi::output_port selectedOutputPort_{};
    bool hasSelectedInputPort_ = false;
    bool hasSelectedOutputPort_ = false;
    bool initialized_ = false;
    bool callbacksWired_ = false;
    bool streaming_ = false;
};

#endif // APP_CONTROLLER_H