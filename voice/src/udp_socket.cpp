#include "../include/udp_socket.h"
#include <sodium.h>
#include <cstring>
#include <sstream>

namespace dv {

UDPSocket::UDPSocket()
#ifdef _WIN32
    : m_socket(INVALID_SOCKET)
#else
    : m_socket(-1)
#endif
{
    std::memset(&m_server, 0, sizeof(m_server));
}

UDPSocket::~UDPSocket() {
    Stop();
}

void UDPSocket::Connect(const std::string &ip, uint16_t port) {
    std::memset(&m_server, 0, sizeof(m_server));
    m_server.sin_family = AF_INET;
    m_server.sin_port = htons(port);

#ifdef _WIN32
    m_server.sin_addr.S_un.S_addr = inet_addr(ip.c_str());
#else
    m_server.sin_addr.s_addr = inet_addr(ip.c_str());
#endif

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

#ifdef _WIN32
    if (m_socket == INVALID_SOCKET) {
#else
    if (m_socket < 0) {
#endif
        Log(LOG_ERROR, "Failed to create UDP socket");
        return;
    }

    // Bind to any local address
    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = 0;
#ifdef _WIN32
    local_addr.sin_addr.S_un.S_addr = INADDR_ANY;
#else
    local_addr.sin_addr.s_addr = INADDR_ANY;
#endif
    bind(m_socket, reinterpret_cast<sockaddr *>(&local_addr), sizeof(local_addr));

    Log(LOG_INFO, "UDP socket connected to " + ip + ":" + std::to_string(port));
}

void UDPSocket::Run() {
    m_running = true;
    m_thread = std::thread(&UDPSocket::ReadThread, this);
}

void UDPSocket::Stop() {
    m_running = false;
#ifdef _WIN32
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
#else
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
#endif
    if (m_thread.joinable()) m_thread.join();
}

void UDPSocket::SetSecretKey(const std::array<uint8_t, 32> &key) {
    m_secret_key = key;
}

void UDPSocket::SetSSRC(uint32_t ssrc) {
    m_ssrc = ssrc;
}

void UDPSocket::SendEncrypted(const uint8_t *data, size_t len, uint32_t timestamp) {
    m_sequence++;
    m_nonce++;

    // Build RTP header (12 bytes) + encrypted payload + auth tag + 4-byte nonce
    const size_t encrypted_size = len + crypto_aead_xchacha20poly1305_ietf_ABYTES;
    std::vector<uint8_t> rtp(12 + encrypted_size + sizeof(uint32_t), 0);

    // RTP header
    rtp[0] = 0x80; // Version 2
    rtp[1] = 0x78; // Payload type 120 (Opus)
    rtp[2] = (m_sequence >> 8) & 0xFF;
    rtp[3] = (m_sequence >> 0) & 0xFF;
    rtp[4] = (timestamp >> 24) & 0xFF;
    rtp[5] = (timestamp >> 16) & 0xFF;
    rtp[6] = (timestamp >> 8) & 0xFF;
    rtp[7] = (timestamp >> 0) & 0xFF;
    rtp[8] = (m_ssrc >> 24) & 0xFF;
    rtp[9] = (m_ssrc >> 16) & 0xFF;
    rtp[10] = (m_ssrc >> 8) & 0xFF;
    rtp[11] = (m_ssrc >> 0) & 0xFF;

    // Nonce: 4-byte counter in first 4 bytes, rest zeroed
    std::array<uint8_t, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES> nonce_bytes{};
    std::memcpy(nonce_bytes.data(), &m_nonce, sizeof(uint32_t));

    // Encrypt with AEAD XChaCha20-Poly1305
    // AAD = RTP header (12 bytes)
    unsigned long long ciphertext_len;
    crypto_aead_xchacha20poly1305_ietf_encrypt(
        rtp.data() + 12, &ciphertext_len,
        data, len,
        rtp.data(), 12,  // AAD = RTP header
        nullptr,
        nonce_bytes.data(),
        m_secret_key.data());

    // Append 4-byte nonce counter at end
    rtp.resize(12 + static_cast<size_t>(ciphertext_len) + sizeof(uint32_t));
    std::memcpy(rtp.data() + rtp.size() - sizeof(uint32_t), &m_nonce, sizeof(uint32_t));

    Send(rtp.data(), rtp.size());
}

void UDPSocket::Send(const uint8_t *data, size_t len) {
    sendto(m_socket, reinterpret_cast<const char *>(data), static_cast<int>(len), 0,
           reinterpret_cast<sockaddr *>(&m_server), sizeof(m_server));
}

std::vector<uint8_t> UDPSocket::Receive() {
    while (true) {
        sockaddr_in from;
#ifdef _WIN32
        int fromlen = sizeof(from);
#else
        socklen_t fromlen = sizeof(from);
#endif
        uint8_t buf[4096];
        int n = recvfrom(m_socket, reinterpret_cast<char *>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr *>(&from), &fromlen);
        if (n < 0) {
            return {};
        }
#ifdef _WIN32
        if (from.sin_addr.S_un.S_addr == m_server.sin_addr.S_un.S_addr &&
#else
        if (from.sin_addr.s_addr == m_server.sin_addr.s_addr &&
#endif
            from.sin_port == m_server.sin_port) {
            return {buf, buf + n};
        }
    }
}

void UDPSocket::SetDataCallback(DataCallback cb) {
    m_data_callback = std::move(cb);
}

void UDPSocket::SetLogCallback(LogCallback cb) {
    m_log_callback = std::move(cb);
}

void UDPSocket::ReadThread() {
    while (m_running) {
        uint8_t buf[4096];
        sockaddr_in from;
#ifdef _WIN32
        int addrlen = sizeof(from);
#else
        socklen_t addrlen = sizeof(from);
#endif

        // Use select with 1-second timeout so we can check m_running
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(m_socket, &read_fds);

        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(static_cast<int>(m_socket + 1), &read_fds, nullptr, nullptr, &tv);
        if (sel > 0) {
            int n = recvfrom(m_socket, reinterpret_cast<char *>(buf), sizeof(buf), 0,
                             reinterpret_cast<sockaddr *>(&from), &addrlen);
            if (n > 0) {
#ifdef _WIN32
                bool from_server = (from.sin_addr.S_un.S_addr == m_server.sin_addr.S_un.S_addr &&
                                    from.sin_port == m_server.sin_port);
#else
                bool from_server = (from.sin_addr.s_addr == m_server.sin_addr.s_addr &&
                                    from.sin_port == m_server.sin_port);
#endif
                if (from_server && m_data_callback) {
                    m_data_callback({buf, buf + n});
                }
            }
        }
    }
}

void UDPSocket::Log(int level, const std::string &msg) {
    if (m_log_callback) m_log_callback(level, "[UDP] " + msg);
}

} // namespace dv
