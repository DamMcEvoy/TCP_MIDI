#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <cstring>
#include <sstream>
#include <atomic>
#include <mutex>
#include <map>
#include <set>
#include <netinet/tcp.h> 
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <signal.h>
#include <csignal>
#include <atomic>

#define PORT 443
#define CERT_FILE "server.crt"
#define KEY_FILE "server.key"
#define BUFFER_SIZE 128

std::atomic<int> client_id_counter(0);
std::mutex client_map_mutex;
std::atomic<bool> shutdown_requested{false};
std::map<int, SSL*> clients; // client_id -> SSL*

void signal_handler(int sig) {
    shutdown_requested = true;
    // Optional: close main sockfd here to unblock accept()
}

// Print a hex representation of the received buffer
void printHex(const char* buffer, int len) {
    std::cout << std::hex << std::setfill('0');
    for (int i = 0; i < len; ++i) {
        std::cout << std::setw(2) << (static_cast<unsigned int>(static_cast<unsigned char>(buffer[i]))) << " ";
    }
    std::cout << std::dec << std::endl;
}

void handle_client(int client_id, SSL* ssl, sockaddr_in addr) {
    // Disable Nagle's algorithm for low latency
    int fd = SSL_get_fd(ssl);
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    int client_port = ntohs(addr.sin_port);

    std::cout << "Client connected: ID=" << client_id << ", IP=" << client_ip << ", Port=" << client_port << std::endl;

    // Send 8-byte padded client ID
    std::ostringstream oss;
    oss << std::setw(8) << std::setfill('0') << client_id;
    std::string client_id_bytes = oss.str();
    SSL_write(ssl, client_id_bytes.c_str(), 8);

    char buffer[BUFFER_SIZE];

    while (!shutdown_requested) {
        int ret = SSL_read(ssl, buffer, BUFFER_SIZE);
        if (ret <= 0) break; // error or disconnect

        // Print as hex only (no binary string)
        std::cout << "Received message from Client " << client_id << " (" << ret << " bytes): ";
        printHex(buffer, ret);

        // Relay message to all other clients
        std::lock_guard<std::mutex> lock(client_map_mutex);
        for (const auto& [other_id, other_ssl] : clients) {
            if (other_id != client_id) {
                int send_ret = SSL_write(other_ssl, buffer, ret);
                if (send_ret <= 0) {
                    std::cerr << "Error sending to Client " << other_id << std::endl;
                }
            }
        }
    }
    // Cleanup on client disconnect
    {
        std::lock_guard<std::mutex> lock(client_map_mutex);
        clients.erase(client_id);
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    std::cout << "Client disconnected: ID=" << client_id << ", IP=" << client_ip << ", Port=" << client_port << std::endl;
}

void cleanup_clients() {  
    std::lock_guard<std::mutex> lock(client_map_mutex);  
    for (auto& pair : clients) {  
        SSL* ssl = pair.second;  
        if (ssl) {  
            SSL_shutdown(ssl);  
            SSL_free(ssl);  
        }  
    }  
    clients.clear();  
}  

int main() {
    // Whitelist of allowed IPs (edit as needed)
    std::set<std::string> allowedIPs = {
        "185.167.198.34",
        "149.157.147.199",
        "149.157.144.132",
        "149.157.151.193"
    };

    SSL_library_init();
    SSL_load_error_strings();
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
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
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
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    while (!shutdown_requested) {
        sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(sockfd, (sockaddr*)&client_addr, &len);
        if (client_fd < 0) {
            if (shutdown_requested.load()) {
                std::cout << "\nShutting down server...\n" << std::endl;
                break;
            }
            perror("\nAccept failed");
            continue;
        }

        // Extract client IP and check whitelist
        char client_ip[INET_ADDRSTRLEN];
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

        int client_id = ++client_id_counter;
        {
            std::lock_guard<std::mutex> lock(client_map_mutex);
            clients[client_id] = ssl;
        }

        // Launch new thread for client connection handling
        std::thread(handle_client, client_id, ssl, client_addr).detach();
    }

    cleanup_clients();
    close(sockfd);
    SSL_CTX_free(ctx);
    return 0;
}
