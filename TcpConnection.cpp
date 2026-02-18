// TcpConnection.cpp
#include "TcpConnection.h"
#include <iostream>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <signal.h>
#include <cstring>
#include <chrono>

TcpConnection::TcpConnection(const std::string& ip, int port)
: serverIp(ip), serverPort(port), sock(-1),
  ctx(nullptr), ssl(nullptr), connected(false),
  recvRunning(false), rtpSequenceNumber(0)
{
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Ignore SIGPIPE to prevent crashes on socket write errors
    signal(SIGPIPE, SIG_IGN);
}

TcpConnection::~TcpConnection() {
    stopReceiveLoop();
    disconnect();
    EVP_cleanup();
}

bool TcpConnection::initSSLContext() {
    const SSL_METHOD* method = TLS_client_method();
    ctx = SSL_CTX_new(method);
    if (!ctx) {
        std::cerr << "ERROR: Unable to create SSL context" << std::endl;
        return false;
    }
    return true;
}

bool TcpConnection::createSocketAndConnect() {
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "ERROR: Failed to create socket" << std::endl;
        return false;
    }

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    // Set receive timeout to 100ms to unblock SSL_read regularly
    setSocketRecvTimeout(100);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(serverPort);

    if (inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "ERROR: Invalid IP address" << std::endl;
        close(sock);
        sock = -1;
        return false;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "ERROR: Connect failed" << std::endl;
        close(sock);
        sock = -1;
        return false;
    }
    return true;
}

void TcpConnection::setSocketRecvTimeout(int timeout_ms) {
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

bool TcpConnection::connectToServer() {
    if (!initSSLContext()) return false;
    if (!createSocketAndConnect()) return false;

    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);

    if (SSL_connect(ssl) <= 0) {
        std::cerr << "ERROR: SSL connect failed" << std::endl;
        disconnect();
        return false;
    }
    connected = true;

    char buf[9] = {0};
    int bytes = SSL_read(ssl, buf, 8);
    if (bytes == 8) {
        clientId = std::string(buf, 8);
        std::cout << "Received Client ID: " << clientId << std::endl;
    } else {
        std::cerr << "Failed to receive Client ID" << std::endl;
        disconnect();
        return false;
    }

    startReceiveLoop();
    return true;
}

void TcpConnection::disconnect() {
    connected = false;

    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ssl = nullptr;
    }
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
    if (ctx) {
        SSL_CTX_free(ctx);
        ctx = nullptr;
    }
}

bool TcpConnection::isConnected() const {
    return connected.load();
}

bool TcpConnection::getClientID(std::string& outID) {
    if (clientId.empty()) {
        return false;
    }
    outID = clientId;
    return true;
}

bool TcpConnection::sendMidiMessage(const std::vector<uint8_t>& midiMessage) {
    std::lock_guard<std::mutex> lock(sendMutex);
    if (!connected) return false;

    std::vector<uint8_t> packet;

    auto now = std::chrono::steady_clock::now();
    uint32_t timestamp = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
    );
    const uint32_t ssrc = 0x12345678;

    buildRtpHeader(packet, rtpSequenceNumber++, timestamp, ssrc);

    packet.insert(packet.end(), midiMessage.begin(), midiMessage.end());

    size_t totalSent = 0;
    while (totalSent < packet.size()) {
        int sent = SSL_write(ssl, packet.data() + totalSent, packet.size() - totalSent);
        if (sent <= 0) {
            std::cerr << "Send failed or connection lost." << std::endl;
            return false;
        }
        totalSent += sent;
    }

    return true;
}

void TcpConnection::buildRtpHeader(std::vector<uint8_t>& buffer, uint16_t sequenceNumber,
                                  uint32_t timestamp, uint32_t ssrc) {
    const uint8_t RTP_VERSION = 2;
    const uint8_t RTP_PAYLOAD_TYPE = 96;

    buffer.push_back((RTP_VERSION << 6));
    buffer.push_back(RTP_PAYLOAD_TYPE);
    buffer.push_back(sequenceNumber >> 8);
    buffer.push_back(sequenceNumber & 0xFF);

    buffer.push_back((timestamp >> 24) & 0xFF);
    buffer.push_back((timestamp >> 16) & 0xFF);
    buffer.push_back((timestamp >> 8) & 0xFF);
    buffer.push_back(timestamp & 0xFF);

    buffer.push_back((ssrc >> 24) & 0xFF);
    buffer.push_back((ssrc >> 16) & 0xFF);
    buffer.push_back((ssrc >> 8) & 0xFF);
    buffer.push_back(ssrc & 0xFF);
}

void TcpConnection::startReceiveLoop() {
    if (recvRunning.load()) return;
    recvRunning = true;
    recvThread = std::thread(&TcpConnection::receiveLoop, this);
}

void TcpConnection::stopReceiveLoop() {
    if (!recvRunning.load()) return;
    recvRunning = false;

    // Closing the socket helps unblock SSL_read in receiveLoop
    if (sock >= 0) {
        shutdown(sock, SHUT_RD); // shutdown reading side to unblock read
    }

    if (recvThread.joinable()) {
        recvThread.join();
    }
}

void TcpConnection::receiveLoop() {
    constexpr size_t bufferSize = 4096;
    uint8_t buffer[bufferSize];

    while (recvRunning.load() && connected.load()) {
        int bytesRead = SSL_read(ssl, buffer, bufferSize);
        if (bytesRead <= 0) {
            int err = SSL_get_error(ssl, bytesRead);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                // Non-fatal; continue waiting
                continue;
            }
            std::cerr << "Connection closed or SSL_read error." << std::endl;
            recvRunning = false;
            connected = false;
            break;
        }
        std::vector<uint8_t> packet(buffer, buffer + bytesRead);
        parseRtpPacket(packet);
    }
}

void TcpConnection::parseRtpPacket(const std::vector<uint8_t>& packet) {
    if (packet.size() < 12) return;

    std::vector<unsigned char> midiData(packet.begin() + 12, packet.end());

    if (midiReceiveCallback) {
        midiReceiveCallback(midiData);
    }
}

void TcpConnection::registerMidiReceiverCallback(std::function<void(const std::vector<uint8_t>&)> callback) {
    midiReceiveCallback = std::move(callback);
}
