#ifndef RELAY_CLIENT_H
#define RELAY_CLIENT_H

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <cstdint>

/**
 * 中盛科技 8路工业以太网继电器 (ZS-DO-R-10A-8) 纯 C++ 原生驱动客户端
 * 通信协议: Modbus-RTU over TCP (默认 192.168.0.7:8234)
 * 通道定义:
 *   Y1 (通道1): 碳刷打火告警
 *   Y2 (通道2): 冷却水系统总告警 (任意一管断水触发)
 *   Y3 (通道3): Pipe 1 异常 (主粗管)
 *   Y4 (通道4): Pipe 2 异常 (细黄管)
 *   Y5 (通道5): Pipe 3 异常 (中银管)
 *   Y6 (通道6): Pipe 4 异常 (弯管口)
 *   Y7 (通道7): Pipe 5 异常 (顺壁流)
 *   Y8 (通道8): Pipe 6 异常 (预留管)
 */
class RelayClient {
public:
    RelayClient();
    ~RelayClient();

    // 初始化参数
    void init(const std::string& host = "192.168.0.7", int port = 8234, bool enabled = true);

    // 控制单个继电器动作 (channel 1~8 对应 Y1~Y8)
    void set_channel(int channel, bool state);

    // 点动脉冲动作 (闭合 duration_sec 秒后自动复位断开)
    void trigger_pulse(int channel, float duration_sec = 1.0f);

    // 全部通道安全复位断开
    void reset_all();

    // 状态查询
    bool is_connected() const { return connected_.load(); }
    bool is_enabled() const { return enabled_.load(); }
    std::string get_status_str() const;

private:
    struct RelayCmd {
        int channel; // 1~8, 0表示所有通道
        bool state;
    };

    void worker_thread();
    bool connect_socket();
    void close_socket();
    bool send_command_raw(int channel, bool state);
    static uint16_t calc_crc16(const uint8_t* data, size_t len);

    std::string host_{"192.168.0.7"};
    int port_{8234};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> connected_{false};
    std::atomic<bool> running_{false};

    int sock_fd_{-1};
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::queue<RelayCmd> cmd_queue_;
    std::thread thread_;
};

#endif // RELAY_CLIENT_H
