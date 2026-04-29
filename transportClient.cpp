#include "transportClient.h"
#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
constexpr uint32_t kMidiFrameMagic = 0x4D494449;
constexpr uint16_t kMidiFrameVersion = 1;
constexpr uint16_t kMidiFrameHeaderSize = 24;
constexpr uint32_t kMaxFrameSize = 4096;
}

TransportClient::TransportClient(const std::string& serverIp, int serverPort)
    : serverIp_(serverIp),
      serverPort_(serverPort),
      socketFd_(-1),
      sslContext_(nullptr),
      ssl_(nullptr),
      connected_(false),
      recvRunning_(false) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    signal(SIGPIPE, SIG_IGN);
}

TransportClient::~TransportClient() {
    disconnect();

    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        receiveCallback_ = nullptr;
    }

    EVP_cleanup();
}

bool TransportClient::initSslContext() {
    const SSL_METHOD* method = TLS_client_method();
    sslContext_ = SSL_CTX_new(method);
    if (!sslContext_) {
        std::cerr << "[TransportClient] Failed to create SSL context." << std::endl;
        return false;
    }
    return true;
}

bool TransportClient::createSocketAndConnect() {
    socketFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd_ < 0) {
        std::cerr << "[TransportClient] Failed to create socket." << std::endl;
        return false;
    }

    int flag = 1;
    setsockopt(socketFd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&flag), sizeof(flag));

    sockaddr_in addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(serverPort_);

    if (inet_pton(AF_INET, serverIp_.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[TransportClient] Invalid IP address." << std::endl;
        close(socketFd_);
        socketFd_ = -1;
        return false;
    }

    if (connect(socketFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[TransportClient] Connect failed: server unavailable or refused connection." << std::endl;
        close(socketFd_);
        socketFd_ = -1;
        return false;
    }

    setSocketRecvTimeout(100);
    return true;
}

void TransportClient::setSocketRecvTimeout(int timeoutMs) {
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(socketFd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

bool TransportClient::receiveClientId() {
    char idBuffer[9] = {0};
    std::size_t totalRead = 0;

    while (totalRead < 8) {
        const int bytesRead = SSL_read(ssl_, idBuffer + totalRead, static_cast<int>(8 - totalRead));
        if (bytesRead > 0) {
            totalRead += static_cast<std::size_t>(bytesRead);
            continue;
        }

        const int err = SSL_get_error(ssl_, bytesRead);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            continue;
        }

        std::cerr << "[TransportClient] Failed to receive client ID. SSL error=" << err << std::endl;
        return false;
    }

    clientId_ = std::string(idBuffer, 8);
    std::cerr << "[TransportClient] Received client ID=" << clientId_ << std::endl;
    return true;
}

bool TransportClient::connectToServer() {
    if (!initSslContext()) {
        return false;
    }

    if (!createSocketAndConnect()) {
        disconnect();
        return false;
    }

    ssl_ = SSL_new(sslContext_);
    if (!ssl_) {
        std::cerr << "[TransportClient] Failed to create SSL session." << std::endl;
        disconnect();
        return false;
    }

    SSL_set_fd(ssl_, socketFd_);

    if (SSL_connect(ssl_) <= 0) {
        std::cerr << "[TransportClient] SSL connect failed." << std::endl;
        ERR_print_errors_fp(stderr);
        disconnect();
        return false;
    }

    if (!receiveClientId()) {
        disconnect();
        return false;
    }

    connected_ = true;
    startReceiveLoop();
    return true;
}

void TransportClient::disconnect() {
    bool wasConnected = connected_.exchange(false);
    recvRunning_ = false;

    if (socketFd_ >= 0) {
        shutdown(socketFd_, SHUT_RDWR);
    }

    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }

    std::lock_guard<std::mutex> sendLock(sendMutex_);

    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }

    if (socketFd_ >= 0) {
        close(socketFd_);
        socketFd_ = -1;
    }

    if (sslContext_) {
        SSL_CTX_free(sslContext_);
        sslContext_ = nullptr;
    }

    if (wasConnected) {
        std::cerr << "[TransportClient] Disconnected cleanly." << std::endl;
    }
}

bool TransportClient::isConnected() const {
    return connected_.load();
}

bool TransportClient::sendMidiMessage(const std::vector<unsigned char>& midiMessage) {
    if (!connected_.load() || midiMessage.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(sendMutex_);
    return writeAll(midiMessage.data(), midiMessage.size());
}

void TransportClient::setReceiveCallback(ReceiveCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    receiveCallback_ = std::move(callback);
}

bool TransportClient::getClientId(std::string& outClientId) const {
    if (clientId_.empty()) {
        return false;
    }
    outClientId = clientId_;
    return true;
}

void TransportClient::startReceiveLoop() {
    if (recvRunning_.load()) {
        return;
    }

    recvRunning_ = true;
    receiveThread_ = std::thread(&TransportClient::receiveLoop, this);
}

void TransportClient::stopReceiveLoop() {
    recvRunning_ = false;
    if (socketFd_ >= 0) {
        shutdown(socketFd_, SHUT_RD);
    }
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
}

void TransportClient::receiveLoop() {
    uint32_t lastSequence = 0;
    bool haveSequence = false;

    while (recvRunning_.load() && connected_.load()) {
        // 1. Read 4-byte BE length prefix
        uint8_t lengthBytes[4];
        if (!readExact(lengthBytes, sizeof(lengthBytes))) break;
        const uint32_t totalLength = readBe32(lengthBytes);
        if (totalLength < kMidiFrameHeaderSize || totalLength > kMaxFrameSize) {
            std::cerr << "[TransportClient] Invalid frame length: " << totalLength << std::endl;
            connected_ = false; recvRunning_ = false; break;
        }

        // 2. Read full frame
        std::vector<uint8_t> frame(totalLength);
        if (!readExact(frame.data(), frame.size())) break;

        // 3. Parse header
        const uint32_t magic = readBe32(frame.data());
        const uint16_t version = readBe16(frame.data() + 4);
        const uint16_t headerSize = readBe16(frame.data() + 6);
        const uint32_t sequence = readBe32(frame.data() + 8);
        const uint64_t serverTimestampNs = readBe64(frame.data() + 12);
        const uint16_t payloadSize = readBe16(frame.data() + 20);
        const uint16_t flags = readBe16(frame.data() + 22);

        // 4. Validate
        if (magic != kMidiFrameMagic || version != kMidiFrameVersion || headerSize != kMidiFrameHeaderSize) {
            std::cerr << "[TransportClient] Invalid frame header. magic=0x" << std::hex << magic << std::dec
                      << " version=" << version << " headerSize=" << headerSize << std::endl;
            connected_ = false; recvRunning_ = false; break;
        }
        if (headerSize + payloadSize != totalLength) {
            std::cerr << "[TransportClient] Frame size mismatch. header=" << headerSize
                      << " payload=" << payloadSize << " total=" << totalLength << std::endl;
            connected_ = false; recvRunning_ = false; break;
        }

        // 5. Sequence check
        if (haveSequence && sequence != lastSequence + 1) {
            std::cerr << "[TransportClient] Sequence gap. prev=" << lastSequence << " current=" << sequence << std::endl;
        }
        lastSequence = sequence;
        haveSequence = true;

        // 6. **NEW: Construct TimedMidiEvent**
        TimedMidiEvent event;
        event.sequence = sequence;
        event.serverTimestampNs = serverTimestampNs;
        event.flags = flags;
        event.midiMessage.assign(frame.begin() + headerSize, frame.end());

        //std::cerr << "[TransportClient] Received frame seq=" << event.sequence
        //          << " ts=" << event.serverTimestampNs << " bytes=" << event.midiMessage.size() << std::endl;

        
        ReceiveCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex_);
            callback = receiveCallback_;
        }
        if (callback) {
            callback(event);
        }
    }

    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
        shutdownCv_.notify_all();
    }
}

bool TransportClient::readExact(uint8_t* dest, std::size_t bytesToRead) {
    std::size_t totalRead = 0;

    while (recvRunning_.load() && connected_.load() && totalRead < bytesToRead) {
        const int bytesRead = SSL_read(ssl_, dest + totalRead, static_cast<int>(bytesToRead - totalRead));
        if (bytesRead > 0) {
            totalRead += static_cast<std::size_t>(bytesRead);
            continue;
        }

        const int err = SSL_get_error(ssl_, bytesRead);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            continue;
        }

        std::cerr << "[TransportClient] SSL_read failed, error=" << err << std::endl;
        connected_ = false;
        recvRunning_ = false;
        return false;
    }

    return totalRead == bytesToRead;
}

bool TransportClient::writeAll(const uint8_t* data, std::size_t bytesToWrite) {
    std::size_t totalWritten = 0;

    while (connected_.load() && totalWritten < bytesToWrite) {
        const int bytesWritten = SSL_write(ssl_, data + totalWritten, static_cast<int>(bytesToWrite - totalWritten));
        if (bytesWritten > 0) {
            totalWritten += static_cast<std::size_t>(bytesWritten);
            continue;
        }

        const int err = SSL_get_error(ssl_, bytesWritten);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            continue;
        }

        std::cerr << "[TransportClient] SSL_write failed, error=" << err << std::endl;
        connected_ = false;
        return false;
    }

    return totalWritten == bytesToWrite;
}

uint16_t TransportClient::readBe16(const uint8_t* data) const {
    return (static_cast<uint16_t>(data[0]) << 8) |
           static_cast<uint16_t>(data[1]);
}

uint32_t TransportClient::readBe32(const uint8_t* data) const {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint64_t TransportClient::readBe64(const uint8_t* data) const {
    return (static_cast<uint64_t>(data[0]) << 56) |
           (static_cast<uint64_t>(data[1]) << 48) |
           (static_cast<uint64_t>(data[2]) << 40) |
           (static_cast<uint64_t>(data[3]) << 32) |
           (static_cast<uint64_t>(data[4]) << 24) |
           (static_cast<uint64_t>(data[5]) << 16) |
           (static_cast<uint64_t>(data[6]) << 8) |
           static_cast<uint64_t>(data[7]);
}
