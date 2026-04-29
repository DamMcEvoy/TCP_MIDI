#include <openssl/ssl.h>
#include <openssl/err.h>

#include <signal.h>
#include <sys/types.h>
#include <memory>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define PORT 443
#define CERT_FILE "server.crt"
#define KEY_FILE "server.key"
#define BUFFER_SIZE 256
#define SERVER_ID_BYTES 8

namespace {
constexpr uint32_t MIDI_FRAME_MAGIC = 0x4D494449;
constexpr uint16_t MIDI_FRAME_VERSION = 1;
constexpr uint16_t MIDI_FRAME_FLAGS_RAW_MIDI = 0x0001;
constexpr uint16_t MIDI_FRAME_FLAGS_JR_CLOCK = 0x0002;
constexpr uint16_t MIDI_FRAME_HEADER_SIZE = 24;
constexpr uint32_t MIDI_FRAME_TOTAL_MAX = MIDI_FRAME_HEADER_SIZE + BUFFER_SIZE;
constexpr auto JR_CLOCK_INTERVAL = std::chrono::milliseconds(250);

struct ClientSession {
    SSL* ssl = nullptr;
    std::mutex writeMutex;
};

std::atomic<int> client_id_counter{0};
std::atomic<uint32_t> event_seq_counter{0};
std::atomic<bool> shutdown_requested{false};
std::mutex client_map_mutex;
std::map<int, std::shared_ptr<ClientSession>> clients;

uint64_t monotonic_time_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

int32_t estimate_drift_ppm() {
    return 0;
}

void append_u16_be(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u32_be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u64_be(std::vector<uint8_t>& out, uint64_t value) {
    out.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void print_hex(const uint8_t* buffer, size_t len) {
    std::cout << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        std::cout << std::setw(2) << static_cast<unsigned>(buffer[i]) << ' ';
    }
    std::cout << std::dec << std::endl;
}

const char* ssl_error_name(int err) {
    switch (err) {
        case SSL_ERROR_NONE: return "SSL_ERROR_NONE";
        case SSL_ERROR_SSL: return "SSL_ERROR_SSL";
        case SSL_ERROR_WANT_READ: return "SSL_ERROR_WANT_READ";
        case SSL_ERROR_WANT_WRITE: return "SSL_ERROR_WANT_WRITE";
        case SSL_ERROR_WANT_X509_LOOKUP: return "SSL_ERROR_WANT_X509_LOOKUP";
        case SSL_ERROR_SYSCALL: return "SSL_ERROR_SYSCALL";
        case SSL_ERROR_ZERO_RETURN: return "SSL_ERROR_ZERO_RETURN";
        case SSL_ERROR_WANT_CONNECT: return "SSL_ERROR_WANT_CONNECT";
        case SSL_ERROR_WANT_ACCEPT: return "SSL_ERROR_WANT_ACCEPT";
#ifdef SSL_ERROR_WANT_ASYNC
        case SSL_ERROR_WANT_ASYNC: return "SSL_ERROR_WANT_ASYNC";
#endif
#ifdef SSL_ERROR_WANT_ASYNC_JOB
        case SSL_ERROR_WANT_ASYNC_JOB: return "SSL_ERROR_WANT_ASYNC_JOB";
#endif
#ifdef SSL_ERROR_WANT_CLIENT_HELLO_CB
        case SSL_ERROR_WANT_CLIENT_HELLO_CB: return "SSL_ERROR_WANT_CLIENT_HELLO_CB";
#endif
        default: return "SSL_ERROR_UNKNOWN";
    }
}

std::vector<uint8_t> build_framed_packet(const uint8_t* payload,
                                         uint16_t payload_len,
                                         uint16_t flags,
                                         uint32_t& out_sequence,
                                         uint64_t& out_timestamp_ns) {
    std::vector<uint8_t> packet;
    packet.reserve(4 + MIDI_FRAME_HEADER_SIZE + payload_len);

    out_sequence = ++event_seq_counter;
    out_timestamp_ns = monotonic_time_ns();
    const uint32_t total_length = MIDI_FRAME_HEADER_SIZE + payload_len;

    append_u32_be(packet, total_length);
    append_u32_be(packet, MIDI_FRAME_MAGIC);
    append_u16_be(packet, MIDI_FRAME_VERSION);
    append_u16_be(packet, MIDI_FRAME_HEADER_SIZE);
    append_u32_be(packet, out_sequence);
    append_u64_be(packet, out_timestamp_ns);
    append_u16_be(packet, payload_len);
    append_u16_be(packet, flags);
    packet.insert(packet.end(), payload, payload + payload_len);

    return packet;
}

std::vector<uint8_t> build_timestamped_packet(const uint8_t* midi_bytes,
                                              uint16_t midi_len,
                                              uint32_t& out_sequence,
                                              uint64_t& out_timestamp_ns) {
    return build_framed_packet(midi_bytes,
                               midi_len,
                               MIDI_FRAME_FLAGS_RAW_MIDI,
                               out_sequence,
                               out_timestamp_ns);
}

std::vector<uint8_t> build_jr_clock_packet(uint32_t& out_sequence,
                                           uint64_t& out_timestamp_ns) {
    std::vector<uint8_t> payload;
    payload.reserve(2 + 4 + 8);
    append_u16_be(payload, 0x0002);
    append_u32_be(payload, static_cast<uint32_t>(estimate_drift_ppm()));
    append_u64_be(payload, monotonic_time_ns());
    return build_framed_packet(payload.data(),
                               static_cast<uint16_t>(payload.size()),
                               MIDI_FRAME_FLAGS_JR_CLOCK,
                               out_sequence,
                               out_timestamp_ns);
}

bool ssl_write_all(SSL* ssl, const uint8_t* data, size_t len, int peer_id = -1) {
    size_t total_sent = 0;
    while (total_sent < len && !shutdown_requested.load()) {
        const int sent = SSL_write(ssl, data + total_sent, static_cast<int>(len - total_sent));
        if (sent > 0) {
            total_sent += static_cast<size_t>(sent);
            continue;
        }

        const int err = SSL_get_error(ssl, sent);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::cerr << "[Server] SSL_write failed"
                  << (peer_id >= 0 ? " for Client " + std::to_string(peer_id) : std::string())
                  << ": error=" << err << " (" << ssl_error_name(err) << ")" << std::endl;
        if (err == SSL_ERROR_SSL) {
            ERR_print_errors_fp(stderr);
        }
        return false;
    }
    return total_sent == len;
}

void signal_handler(int) {
    shutdown_requested = true;
}

void remove_client(int client_id) {
    std::lock_guard<std::mutex> lock(client_map_mutex);
    clients.erase(client_id);
}

void broadcast_to_other_clients(int sender_client_id, const std::vector<uint8_t>& packet) {
    std::vector<std::pair<int, std::shared_ptr<ClientSession>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(client_map_mutex);
        for (const auto& [other_id, other_session] : clients) {
            if (other_id != sender_client_id) {
                snapshot.emplace_back(other_id, other_session);
            }
        }
    }

    for (const auto& [other_id, other_session] : snapshot) {
        std::lock_guard<std::mutex> writeLock(other_session->writeMutex);
        if (!ssl_write_all(other_session->ssl, packet.data(), packet.size(), other_id)) {
            std::cerr << "[Server] Error sending framed packet to Client " << other_id << std::endl;
        }
    }
}

void broadcast_to_all_clients(const std::vector<uint8_t>& packet) {
    std::vector<std::pair<int, std::shared_ptr<ClientSession>>> snapshot;

    {
        std::lock_guard<std::mutex> lock(client_map_mutex);
        for (const auto& [other_id, other_session] : clients) {
            snapshot.emplace_back(other_id, other_session);
        }
    }

    for (const auto& [other_id, other_session] : snapshot) {
        std::lock_guard<std::mutex> writeLock(other_session->writeMutex);
        if (!ssl_write_all(other_session->ssl, packet.data(), packet.size(), other_id)) {
            std::cerr << "[Server] Error sending broadcast packet to Client " << other_id << std::endl;
        }
    }
}

void handle_client(int client_id, SSL* ssl, sockaddr_in addr) {
    const int fd = SSL_get_fd(ssl);
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    char client_ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &(addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    const int client_port = ntohs(addr.sin_port);

    std::cout << "Client connected: ID=" << client_id
              << ", IP=" << client_ip
              << ", Port=" << client_port << std::endl;

    std::ostringstream oss;
    oss << std::setw(SERVER_ID_BYTES) << std::setfill('0') << client_id;
    const std::string client_id_bytes = oss.str();
    if (!ssl_write_all(ssl,
                       reinterpret_cast<const uint8_t*>(client_id_bytes.data()),
                       SERVER_ID_BYTES,
                       client_id)) {
        std::cerr << "[Server] Failed to send client ID to Client " << client_id << std::endl;
        remove_client(client_id);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        return;
    }

    uint8_t buffer[BUFFER_SIZE];
    auto last_clock = std::chrono::steady_clock::now();

    while (!shutdown_requested.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_clock >= JR_CLOCK_INTERVAL) {
            uint32_t clock_sequence = 0;
            uint64_t clock_timestamp_ns = 0;
            auto clock_packet = build_jr_clock_packet(clock_sequence, clock_timestamp_ns);
            //std::cout << "[ServerClock] seq=" << clock_sequence
            //          << " ts=" << clock_timestamp_ns
            //          << " payloadBytes=" << (clock_packet.size() - 4 - MIDI_FRAME_HEADER_SIZE)
            //          << " totalFrameBytes=" << clock_packet.size() << std::endl;
            broadcast_to_all_clients(clock_packet);
            last_clock = now;
        }

        const int ret = SSL_read(ssl, buffer, BUFFER_SIZE);
        if (ret > 0) {
            std::cout << "Received raw MIDI from Client " << client_id
                      << " (" << ret << " bytes): ";
            print_hex(buffer, static_cast<size_t>(ret));

            uint32_t sequence = 0;
            uint64_t timestamp_ns = 0;
            auto packet = build_timestamped_packet(buffer,
                                                   static_cast<uint16_t>(ret),
                                                   sequence,
                                                   timestamp_ns);

            std::cout << "[ServerFrame] seq=" << sequence
                      << " ts=" << timestamp_ns
                      << " payloadBytes=" << ret
                      << " totalFrameBytes=" << packet.size() << std::endl;

            if (packet.size() >= 24) {
                std::cout << "[ServerFrame] header bytes: ";
                print_hex(packet.data(), 24);
            }

            broadcast_to_other_clients(client_id, packet);
            continue;
        }


        const int err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (err == SSL_ERROR_ZERO_RETURN) {
            std::cout << "[Server] Client " << client_id << " closed TLS session cleanly." << std::endl;
            break;
        }

        if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::cerr << "[Server] SSL_read failed for Client " << client_id
                  << ": ret=" << ret
                  << " error=" << err << " (" << ssl_error_name(err) << ")"
                  << " errno=" << errno << std::endl;
        if (err == SSL_ERROR_SSL) {
            ERR_print_errors_fp(stderr);
        }
        break;
    }

    remove_client(client_id);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);

    std::cout << "Client disconnected: ID=" << client_id
              << ", IP=" << client_ip
              << ", Port=" << client_port << std::endl;
}

void cleanup_clients() {
    std::lock_guard<std::mutex> lock(client_map_mutex);
    for (auto& [id, session] : clients) {
        if (session && session->ssl) {
            const int fd = SSL_get_fd(session->ssl);
            SSL_shutdown(session->ssl);
            SSL_free(session->ssl);
            if (fd >= 0) {
                close(fd);
            }
        }
    }
    clients.clear();
}

} // namespace

int main() {
    std::set<std::string> allowedIPs = {
        "185.167.198.34",
        "149.157.147.199",
        "149.157.144.132",
        "149.157.151.193",
        "51.199.18.159",
        "149.157.151.153",
        "149.157.146.244",
        "149.157.150.191",
        "80.233.46.249",
        "149.157.145.150",
        "149.157.147.142",
        "80.233.60.175"
    };

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD* method = TLS_server_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        return 1;
    }

    if (SSL_CTX_use_certificate_file(ctx, CERT_FILE, SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, KEY_FILE, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        SSL_CTX_free(ctx);
        return 1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("Bind failed");
        SSL_CTX_free(ctx);
        close(sockfd);
        return 1;
    }

    if (listen(sockfd, 5) < 0) {
        perror("Listen failed");
        SSL_CTX_free(ctx);
        close(sockfd);
        return 1;
    }

    std::cout << "Server listening on port " << PORT << std::endl;

    struct sigaction sa = {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    while (!shutdown_requested.load()) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(sockfd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (shutdown_requested.load()) {
                std::cout << "\nShutting down server...\n" << std::endl;
                break;
            }
            perror("Accept failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        if (allowedIPs.find(std::string(client_ip)) == allowedIPs.end()) {
            std::cerr << "Connection rejected from IP: " << client_ip << std::endl;
            close(client_fd);
            continue;
        }

        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            std::cerr << "Failed to create SSL object for client " << client_ip << std::endl;
            close(client_fd);
            continue;
        }
                SSL_set_fd(ssl, client_fd);
        if (SSL_accept(ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            close(client_fd);
            continue;
        }

        const int client_id = ++client_id_counter;
        auto session = std::make_shared<ClientSession>();
        session->ssl = ssl;

        {
            std::lock_guard<std::mutex> lock(client_map_mutex);
            clients[client_id] = session;
        }

        std::thread(handle_client, client_id, ssl, client_addr).detach();
    }

    cleanup_clients();
    close(sockfd);
    SSL_CTX_free(ctx);
    return 0;
}