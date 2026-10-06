#include "relay_client.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <chrono>

RelayClient::RelayClient() {
}

RelayClient::~RelayClient() {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    // 析构时保证断开
    if (sock_fd_ >= 0) {
        close_socket();
    }
}

void RelayClient::init(const std::string& host, int port, bool enabled) {
    host_ = host;
    port_ = port;
    enabled_ = enabled;

    if (!enabled_) {
        std::cout << "[硬件继电器-C++] 模块已被配置为禁用。" << std::endl;
        return;
    }

    running_ = true;
    thread_ = std::thread(&RelayClient::worker_thread, this);
    std::cout << "[硬件继电器-C++] 驱动已初始化，目标地址: " << host_ << ":" << port_ << std::endl;
}

uint16_t RelayClient::calc_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

bool RelayClient::connect_socket() {
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // 设置发送/接收超时为 1.5 秒
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 500000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &serv_addr.sin_addr) <= 0) {
        ::close(fd);
        return false;
    }

    if (::connect(fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        ::close(fd);
        return false;
    }

    sock_fd_ = fd;
    connected_ = true;
    return true;
}

void RelayClient::close_socket() {
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
    connected_ = false;
}

bool RelayClient::send_command_raw(int channel, bool state) {
    if (sock_fd_ < 0) return false;

    if (channel == 0 && !state) {
        // 复位所有通道 1~8
        bool all_ok = true;
        for (int ch = 1; ch <= 8; ++ch) {
            if (!send_command_raw(ch, false)) all_ok = false;
            usleep(20000); // 20ms 间隔
        }
        return all_ok;
    }

    if (channel < 1 || channel > 8) return false;

    uint16_t reg = static_cast<uint16_t>(channel - 1);
    uint16_t val = state ? 0xFF00 : 0x0000;

    uint8_t pdu[6];
    pdu[0] = 0x01; // Slave ID
    pdu[1] = 0x05; // Force Single Coil
    pdu[2] = static_cast<uint8_t>((reg >> 8) & 0xFF);
    pdu[3] = static_cast<uint8_t>(reg & 0xFF);
    pdu[4] = static_cast<uint8_t>((val >> 8) & 0xFF);
    pdu[5] = static_cast<uint8_t>(val & 0xFF);

    uint16_t crc = calc_crc16(pdu, 6);
    uint8_t packet[8];
    std::memcpy(packet, pdu, 6);
    packet[6] = static_cast<uint8_t>(crc & 0xFF);
    packet[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);

    ssize_t sent = ::send(sock_fd_, packet, 8, 0);
    if (sent != 8) {
        close_socket();
        return false;
    }

    uint8_t resp[32];
    ssize_t recvd = ::recv(sock_fd_, resp, sizeof(resp), 0);
    if (recvd <= 0) {
        close_socket();
        return false;
    }

    return true;
}

void RelayClient::worker_thread() {
    while (running_) {
        // 1. 如果未连接，尝试重连
        if (!connected_) {
            if (connect_socket()) {
                std::cout << "⚡ [硬件继电器-C++] 成功连通继电器模块 " << host_ << ":" << port_ << "！" << std::endl;
                // 连接成功后先全部复位断开，保证现场初始安全
                send_command_raw(0, false);
            } else {
                // 连接失败，等待 2 秒重试
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
        }

        // 2. 取指令处理
        RelayCmd cmd;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this]() {
                return !running_ || !cmd_queue_.empty() || !connected_;
            });

            if (!running_) break;
            if (!connected_) continue;
            if (cmd_queue_.empty()) continue;

            cmd = cmd_queue_.front();
            cmd_queue_.pop();
        }

        // 3. 执行动作
        bool ok = send_command_raw(cmd.channel, cmd.state);
        if (ok) {
            if (cmd.channel == 0) {
                std::cout << "✅ [硬件继电器-C++] 所有继电器通道 (Y1~Y8) 安全复位已执行。" << std::endl;
            } else {
                std::cout << "⚡ [硬件继电器-C++] 通道 Y" << cmd.channel << " 动作成功: " 
                          << (cmd.state ? "【吸合闭合】" : "【断开释放】") << std::endl;
            }
        } else {
            std::cerr << "⚠️ [硬件继电器-C++] 发送指令失败，准备自动重连..." << std::endl;
            close_socket();
        }
    }

    // 线程退出前最后一次全关
    if (connected_) {
        send_command_raw(0, false);
        close_socket();
    }
}

void RelayClient::set_channel(int channel, bool state) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    cmd_queue_.push({channel, state});
    cv_.notify_one();
}

void RelayClient::trigger_pulse(int channel, float duration_sec) {
    if (!enabled_) return;
    set_channel(channel, true);
    std::thread([this, channel, duration_sec]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(duration_sec * 1000)));
        set_channel(channel, false);
    }).detach();
}

void RelayClient::reset_all() {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    cmd_queue_.push({0, false});
    cv_.notify_one();
}

std::string RelayClient::get_status_str() const {
    if (!enabled_) return "DISABLED";
    return connected_ ? "ONLINE (TCP 8234)" : "CONNECTING...";
}
