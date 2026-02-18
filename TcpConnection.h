// TcpConnection.h
#ifndef TCPCONNECTION_H
#define TCPCONNECTION_H

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <functional>
#include <atomic>
#include <openssl/ssl.h>

class TcpConnection {
public:
    TcpConnection(const std::string& ip, int port);
    ~TcpConnection();

    bool connectToServer();
    void disconnect();

    bool isConnected() const;

    bool getClientID(std::string& outID);

    bool sendMidiMessage(const std::vector<uint8_t>& midiMessage);
    void registerMidiReceiverCallback(std::function<void(const std::vector<uint8_t>&)> callback);

    void startReceiveLoop();
    void stopReceiveLoop();

private:
    std::string serverIp;
    int serverPort;
    int sock;
    SSL_CTX* ctx;
    SSL* ssl;
    std::atomic<bool> connected;

    std::mutex sendMutex;

    std::thread recvThread;
    std::atomic<bool> recvRunning;

    std::function<void(const std::vector<uint8_t>&)> midiReceiveCallback;

    bool initSSLContext();
    bool createSocketAndConnect();

    void receiveLoop();
    void parseRtpPacket(const std::vector<uint8_t>& packet);

    void setSocketRecvTimeout(int timeout_ms);

    uint16_t rtpSequenceNumber;

    void buildRtpHeader(std::vector<uint8_t>& buffer, uint16_t sequenceNumber, uint32_t timestamp, uint32_t ssrc);

    std::string clientId;
};

#endif // TCPCONNECTION_H
