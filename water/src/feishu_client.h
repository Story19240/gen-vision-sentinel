#ifndef FEISHU_CLIENT_H
#define FEISHU_CLIENT_H

#include <string>
#include <vector>
#include <mutex>

// 单个告警键值对
struct FeishuField {
    std::string label;
    std::string value;
};

class FeishuClient {
public:
    FeishuClient();
    ~FeishuClient();

    // 初始化凭证
    void init(const std::string& app_id, 
              const std::string& app_secret, 
              const std::string& receive_id, 
              const std::string& receive_id_type = "chat_id",
              const std::string& web_url = "http://192.168.55.1:8080");

    // 异步发送水流断水紧急卡片 (🔴 红底卡片，新线程执行，0ms 不卡顿主循环)
    void send_water_alarm_async(int pipe_id, 
                                const std::string& pipe_name, 
                                float current_energy, 
                                float threshold, 
                                float duration_sec, 
                                const std::string& relay_info = "");

    // 异步发送水流偏低预警卡片 (⚠️ 橙底卡片)
    void send_water_warning_async(int pipe_id, 
                                  const std::string& pipe_name, 
                                  float current_energy, 
                                  float threshold, 
                                  float duration_sec);

    // 异步发送水流恢复正常卡片 (🟢 绿底卡片)
    void send_water_recovery_async(int pipe_id, 
                                   const std::string& pipe_name, 
                                   float current_energy);

    // 异步发送自定义卡片
    void send_card_async(const std::string& title, 
                         const std::string& color, 
                         const std::vector<FeishuField>& fields);

    // 同步发送方法 (供测试使用)
    bool send_card_sync(const std::string& title, 
                        const std::string& color, 
                        const std::vector<FeishuField>& fields);

private:
    std::string app_id_;
    std::string app_secret_;
    std::string receive_id_;
    std::string receive_id_type_;
    std::string web_monitor_url_;

    std::string token_;
    int64_t token_expire_time_;
    std::mutex token_mutex_;

    // 获取内部 tenant_access_token
    std::string get_token();

    // libcurl HTTP POST 请求封装
    static bool http_post(const std::string& url, 
                          const std::string& json_body, 
                          const std::string& auth_header, 
                          std::string& response);
};

#endif // FEISHU_CLIENT_H
