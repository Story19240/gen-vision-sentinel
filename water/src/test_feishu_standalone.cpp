#include "feishu_client.h"
#include "env_utils.h"
#include <iostream>

int main() {
    FeishuClient client;
    client.init(env_or("FEISHU_APP_ID"),
                env_or("FEISHU_APP_SECRET"),
                env_or("FEISHU_RECEIVE_ID"),
                env_or("FEISHU_RECEIVE_ID_TYPE", "chat_id"),
                env_or("FEISHU_WEB_MONITOR_URL", "http://192.168.55.1:8080"));

    std::cout << "[纯 C++ 飞书测试] 正在向飞书推送告警卡片..." << std::endl;
    bool ok = client.send_card_sync("🔴【纯 C++ 原生测试】发电机冷却水断流告警", "red", {
        {"告警来源", "Jetson AGX Xavier 原生纯 C++ 客户端"},
        {"异常管道", "Pipe 2 中间细黄管"},
        {"当前动能", "0.00 (健康阈值: 3.50)"},
        {"持续时长", "2.0s 断流确诊"}
    });

    if (ok) {
        std::cout << "✅ [纯 C++ 飞书测试成功] 飞书群已接收到原生 C++ 推送的卡片！" << std::endl;
        return 0;
    } else {
        std::cerr << "❌ [测试失败]" << std::endl;
        return 1;
    }
}
