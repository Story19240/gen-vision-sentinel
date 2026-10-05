#include "feishu_client.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <thread>
#include <cstring>
#include <curl/curl.h>

// libcurl 写数据回调
static size_t feishu_curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total_size);
    return total_size;
}

FeishuClient::FeishuClient() : token_expire_time_(0) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

FeishuClient::~FeishuClient() {
    curl_global_cleanup();
}

void FeishuClient::init(const std::string& app_id, 
                        const std::string& app_secret, 
                        const std::string& receive_id, 
                        const std::string& receive_id_type,
                        const std::string& web_url) {
    app_id_ = app_id;
    app_secret_ = app_secret;
    receive_id_ = receive_id;
    receive_id_type_ = receive_id_type;
    web_monitor_url_ = web_url;

    // 启动时在独立后台线程预取 Token 并缓存，有效期 2 小时，告警时直接秒发
    std::thread([this]() {
        std::string token = this->get_token();
        if (!token.empty()) {
            std::cout << "✅ [FeishuClient-C++] 启动预取飞书 Token 成功，已常驻内存就绪！" << std::endl;
        } else {
            std::cerr << "⚠️ [FeishuClient-C++] 预取飞书 Token 未完成，告警触发时将自动重试。" << std::endl;
        }
    }).detach();
}

bool FeishuClient::http_post(const std::string& url, 
                             const std::string& json_body, 
                             const std::string& auth_header, 
                             std::string& response) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=utf-8");
    if (!auth_header.empty()) {
        std::string auth = "Authorization: " + auth_header;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, feishu_curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // 允许自签证书
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    const char* env_proxy = getenv("https_proxy");
    if (!env_proxy) env_proxy = getenv("http_proxy");
    if (env_proxy && strlen(env_proxy) > 0) {
        curl_easy_setopt(curl, CURLOPT_PROXY, env_proxy);
    } else {
        curl_easy_setopt(curl, CURLOPT_PROXY, "http://192.168.55.100:10811");
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (res != CURLE_OK) {
        std::cerr << "[FeishuClient-C++] curl 请求失败: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && http_code == 200);
}

std::string FeishuClient::get_token() {
    std::lock_guard<std::mutex> lock(token_mutex_);
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (!token_.empty() && now < token_expire_time_) {
        return token_;
    }

    if (app_id_.empty() || app_secret_.empty()) {
        std::cerr << "[FeishuClient-C++] 错误: app_id 或 app_secret 为空!" << std::endl;
        return "";
    }

    std::string url = "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal";
    std::string body = "{\"app_id\":\"" + app_id_ + "\",\"app_secret\":\"" + app_secret_ + "\"}";
    std::string resp;

    if (http_post(url, body, "", resp)) {
        // 简易提取 tenant_access_token
        size_t pos = resp.find("\"tenant_access_token\":\"");
        if (pos != std::string::npos) {
            size_t start = pos + 23;
            size_t end = resp.find("\"", start);
            token_ = resp.substr(start, end - start);
            token_expire_time_ = now + 7000;
            return token_;
        }
    }
    std::cerr << "[FeishuClient-C++] 获取 Token 失败，返回: " << resp << std::endl;
    return "";
}

bool FeishuClient::send_card_sync(const std::string& title, 
                                 const std::string& color, 
                                 const std::vector<FeishuField>& fields) {
    std::string token = get_token();
    if (token.empty()) {
        std::cerr << "[FeishuClient-C++] 无有效 Token，取消发送!" << std::endl;
        return false;
    }

    if (receive_id_.empty()) {
        std::cerr << "[FeishuClient-C++] receive_id 为空，无法指定接收群!" << std::endl;
        return false;
    }

    // 格式化当前绝对时间戳
    auto now_tp = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now_tp);
    std::stringstream time_ss;
    time_ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");

    // 组装两列排布的字段卡片
    std::stringstream card_ss;
    card_ss << "{\"config\":{\"wide_screen_mode\":true},";
    card_ss << "\"header\":{\"title\":{\"tag\":\"plain_text\",\"content\":\"" << title << "\"},";
    card_ss << "\"template\":\"" << color << "\"},";
    card_ss << "\"elements\":[{\"tag\":\"div\",\"fields\":[";

    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) card_ss << ",";
        card_ss << "{\"is_short\":true,\"text\":{\"tag\":\"lark_md\",\"content\":\"**" 
                << fields[i].label << "：**\\n" << fields[i].value << "\"}}";
    }

    card_ss << "]},{\"tag\":\"hr\"},";
    card_ss << "{\"tag\":\"note\",\"elements\":[{\"tag\":\"plain_text\",\"content\":\"⏱ 触发时间: " 
            << time_ss.str() << " | 设备视觉哨兵原生 C++ 核心驱动\"}]}";

    // 底部大屏跳转按钮
    if (!web_monitor_url_.empty()) {
        card_ss << ",{\"tag\":\"action\",\"actions\":[{\"tag\":\"button\",";
        card_ss << "\"text\":{\"tag\":\"plain_text\",\"content\":\"🔍 查看现场实时大屏\"},";
        card_ss << "\"type\":\"" << (color == "red" ? "primary" : "default") << "\",";
        card_ss << "\"url\":\"" << web_monitor_url_ << "\"}]}";
    }

    card_ss << "]}";

    // 构造发送到飞书 API 的外层请求体
    // 注意：content 字段在飞书协议中必须是以字符串转义的 JSON
    std::string card_content_raw = card_ss.str();
    std::string card_content_escaped = "";
    for (char c : card_content_raw) {
        if (c == '"') card_content_escaped += "\\\"";
        else if (c == '\\') card_content_escaped += "\\\\";
        else card_content_escaped += c;
    }

    std::string send_url = "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=" + receive_id_type_;
    std::string body = "{\"receive_id\":\"" + receive_id_ + "\",\"msg_type\":\"interactive\",\"content\":\"" + card_content_escaped + "\"}";
    std::string resp;

    bool ok = http_post(send_url, body, "Bearer " + token, resp);
    if (ok && resp.find("\"code\":0") != std::string::npos) {
        std::cout << "[FeishuClient-C++] 告警卡片发送成功: " << title << std::endl;
        return true;
    } else {
        std::cerr << "[FeishuClient-C++] 告警发送失败: " << resp << std::endl;
        return false;
    }
}

void FeishuClient::send_card_async(const std::string& title, 
                                  const std::string& color, 
                                  const std::vector<FeishuField>& fields) {
    // 启动完全隔离的独立线程异步发送，零微秒阻塞视频主循环！
    std::thread([this, title, color, fields]() {
        this->send_card_sync(title, color, fields);
    }).detach();
}

void FeishuClient::send_water_alarm_async(int pipe_id, 
                                         const std::string& pipe_name, 
                                         float current_energy, 
                                         float threshold, 
                                         float duration_sec, 
                                         const std::string& relay_info) {
    std::string title = "🔴【紧急告警】发电机冷却水断流 - Pipe " + std::to_string(pipe_id) + " 异常！";
    std::string relay_text = relay_info.empty() ? ("DO" + std::to_string(pipe_id + 2) + " (回路已闭合)") : relay_info;

    std::stringstream e_ss, th_ss, dur_ss;
    e_ss << std::fixed << std::setprecision(2) << current_energy;
    th_ss << std::fixed << std::setprecision(2) << threshold;
    dur_ss << std::fixed << std::setprecision(1) << duration_sec;

    std::vector<FeishuField> fields = {
        {"监控设备", "1号发电机机组 - 冷却水系统"},
        {"告警等级", "🚨 一级紧急 (CRITICAL)"},
        {"故障管道", "Pipe " + std::to_string(pipe_id) + " (" + pipe_name + ")"},
        {"异常状态", "水流完全中断 / 流量趋近于零"},
        {"实时水流动能", "`" + e_ss.str() + "` (安全阈值: " + th_ss.str() + ")"},
        {"持续异常时间", "**" + dur_ss.str() + " 秒** (超设定上限)"},
        {"硬件联锁动作", "`" + relay_text + "`"},
        {"处置建议", "请值班人员立即核实备用水泵或联系机组降负荷！"}
    };

    send_card_async(title, "red", fields);
}

void FeishuClient::send_water_warning_async(int pipe_id, 
                                           const std::string& pipe_name, 
                                           float current_energy, 
                                           float threshold, 
                                           float duration_sec) {
    std::string title = "⚠️【运行预警】冷却水流量偏低 - Pipe " + std::to_string(pipe_id) + " 疑似气阻";
    std::stringstream e_ss, th_ss, dur_ss;
    e_ss << std::fixed << std::setprecision(2) << current_energy;
    th_ss << std::fixed << std::setprecision(2) << threshold;
    dur_ss << std::fixed << std::setprecision(1) << duration_sec;

    std::vector<FeishuField> fields = {
        {"监控设备", "1号发电机机组 - 冷却水系统"},
        {"预警等级", "⚠️ 二级注意 (WARNING)"},
        {"预警管道", "Pipe " + std::to_string(pipe_id) + " (" + pipe_name + ")"},
        {"实时动能参数", "`" + e_ss.str() + "` (安全阈值: " + th_ss.str() + ")"},
        {"波动持续时间", dur_ss.str() + " 秒"},
        {"状态说明", "水流短时跌落，系统密切跟踪，若超2s将升格停机报警"}
    };

    send_card_async(title, "orange", fields);
}

void FeishuClient::send_water_recovery_async(int pipe_id, 
                                            const std::string& pipe_name, 
                                            float current_energy) {
    std::string title = "🟢【状态恢复】发电机冷却水正常 - Pipe " + std::to_string(pipe_id) + " 水流恢复";
    std::stringstream e_ss;
    e_ss << std::fixed << std::setprecision(2) << current_energy;

    std::vector<FeishuField> fields = {
        {"监控设备", "1号发电机机组 - 冷却水系统"},
        {"通知等级", "✅ 恢复正常 (NORMAL)"},
        {"恢复管道", "Pipe " + std::to_string(pipe_id) + " (" + pipe_name + ")"},
        {"当前动能参数", "`" + e_ss.str() + "` (已回归健康区间)"},
        {"继电器状态", "对应报警回路已自动释放复位"}
    };

    send_card_async(title, "green", fields);
}
