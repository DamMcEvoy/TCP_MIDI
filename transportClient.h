#ifndef TRANSPORT_CLIENT_H
#define TRANSPORT_CLIENT_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>

#include <openssl/ssl.h>
#include <libremidi/libremidi.hpp>

#include "TimedMidiEvent.h"

class TransportClient {
public:
    using ReceiveCallback = std::function<void(const TimedMidiEvent&)>;

    TransportClient(const std::string& serverIp, int serverPort);
    ~TransportClient();

    bool connectToServer();
    void disconnect();
    bool isConnected() const;

    bool sendMidiMessage(const libremidi::midi_bytes& midiMessage);
    void setReceiveCallback(ReceiveCallback callback);

    bool getClientId(std::string& outClientId) const;

private:
    bool initSslContext();
    bool createSocketAndConnect();
    void setSocketRecvTimeout(int timeoutMs);
    bool receiveClientId();

    void startReceiveLoop();
    void stopReceiveLoop();
    void receiveLoop();

    bool readExact(uint8_t* dest, std::size_t bytesToRead);
    bool writeAll(const uint8_t* data, std::size_t bytesToWrite);

    uint16_t readBe16(const uint8_t* data) const;
    uint32_t readBe32(const uint8_t* data) const;
    uint64_t readBe64(const uint8_t* data) const;

private:
    std::string serverIp_;
    int serverPort_;

    int socketFd_;
    SSL_CTX* sslContext_;
    SSL* ssl_;

    std::condition_variable shutdownCv_;
    std::mutex shutdownMutex_;

    std::string clientId_;
    std::atomic<bool> connected_;
    std::atomic<bool> recvRunning_;

    mutable std::mutex sendMutex_;
    mutable std::mutex callbackMutex_;
    std::thread receiveThread_;
    ReceiveCallback receiveCallback_;
};

#endif // TRANSPORT_CLIENT_H
