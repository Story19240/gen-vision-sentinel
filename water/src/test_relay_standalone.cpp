#include "relay_client.h"
#include <iostream>
#include <unistd.h>

int main() {
    std::cout << "=== 启动纯 C++ 原生继电器驱动测试 (ZS-DO-R-10A-8) ===" << std::endl;
    RelayClient relay;
    relay.init("192.168.0.7", 8234, true);

    // 等待连接建立
    sleep(1);

    if (!relay.is_connected()) {
        std::cerr << "连接未成功，状态: " << relay.get_status_str() << std::endl;
        return 1;
    }

    std::cout << "测试 1: 触发 Y2 (冷却水总报警) 闭合 1.5 秒..." << std::endl;
    relay.set_channel(2, true);
    sleep(2);

    std::cout << "测试 2: 触发 Y2 复位断开..." << std::endl;
    relay.set_channel(2, false);
    sleep(1);

    std::cout << "测试 3: 触发 Y4 (Pipe 2 细黄管) 脉冲点动 1 秒..." << std::endl;
    relay.trigger_pulse(4, 1.0f);
    sleep(2);

    std::cout << "测试 4: 全部复位断开..." << std::endl;
    relay.reset_all();
    sleep(1);

    std::cout << "=== 纯 C++ 继电器驱动测试圆满完成！ ===" << std::endl;
    return 0;
}
