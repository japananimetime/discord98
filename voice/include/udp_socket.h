#pragma once
#ifndef DISCORD_VOICE_UDP_SOCKET_H
#define DISCORD_VOICE_UDP_SOCKET_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "voice_types.h"

namespace dv {

class UDPSocket {
public:
    UDPSocket();
    ~UDPSocket();

    void Connect(const std::string &ip, uint16_t port);
    void Run();
    void Stop();

    void SetSecretKey(const std::array<uint8_t, 32> &key);
    void SetSSRC(uint32_t ssrc);

    void SendEncrypted(const uint8_t *data, size_t len, uint32_t timestamp);
    void Send(const uint8_t *data, size_t len);
    std::vector<uint8_t> Receive();

    // Callback for received UDP data
    using DataCallback = std::function<void(const std::vector<uint8_t> &data)>;
    void SetDataCallback(DataCallback cb);

    void SetLogCallback(LogCallback cb);

private:
    void ReadThread();
    void Log(int level, const std::string &msg);

#ifdef _WIN32
    SOCKET m_socket;
#else
    int m_socket;
#endif
    sockaddr_in m_server;

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    std::array<uint8_t, 32> m_secret_key{};
    uint32_t m_ssrc = 0;
    uint16_t m_sequence = 0;
    uint32_t m_nonce = 0;

    DataCallback m_data_callback;
    LogCallback m_log_callback;
};

} // namespace dv

#endif // DISCORD_VOICE_UDP_SOCKET_H
