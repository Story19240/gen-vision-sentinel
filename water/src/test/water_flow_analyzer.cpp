#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <iomanip>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
#include <cstring>
#include <csignal>
#include <regex>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <memory>

#include "../feishu_client.h"
#include "../relay_client.h"
#include "../env_utils.h"

// 系统运行阶段状态机枚举
enum SystemMode {
    MODE_CALIBRATION = 0, // 开机标定阶段 (零初始框，安全等待鼠标画框，零误报)
    MODE_MONITORING = 1   // 正式监控阶段 (动能分析、抗抖纠偏、断流判定与硬件联动)
};

static std::atomic<SystemMode> g_system_mode(MODE_CALIBRATION);
static std::mutex g_frame_mutex;
static std::vector<uchar> g_jpeg_buffer;
static std::atomic<bool> g_running(true);
static std::atomic<bool> g_sim_cutoff_pipe2(false);
static bool g_is_live_stream = false;

void signal_handler(int sig) {
    std::cout << "\n[系统信号] 收到退出信号 (" << sig << ")，正在平稳停止..." << std::endl;
    g_running = false;
}

// 刚体锚点抗抖动跟踪器 (可选启用，默认纯静态 0 锚点模式)
struct AnchorTracker {
    bool enabled = false; // 默认零锚点，用户框选螺栓等构件后才激活
    cv::Rect base_anchor = cv::Rect(0, 0, 0, 0);
    cv::Mat template_gray;
    int cur_dx = 0;
    int cur_dy = 0;
    float match_score = 1.0f;
    bool initialized = false;

    void reset(const cv::Mat& gray, const cv::Rect& roi) {
        cv::Rect valid_r = roi & cv::Rect(0, 0, gray.cols, gray.rows);
        if (valid_r.width >= 20 && valid_r.height >= 20) {
            base_anchor = valid_r;
            template_gray = gray(base_anchor).clone();
            cur_dx = 0;
            cur_dy = 0;
            match_score = 1.0f;
            initialized = true;
            enabled = true;
            std::cout << "\n🎯 [抗抖锚点] 成功设置锚点区域: [" 
                      << base_anchor.x << "," << base_anchor.y << "," 
                      << (base_anchor.x + base_anchor.width) << "," 
                      << (base_anchor.y + base_anchor.height) << "] (尺寸: " 
                      << base_anchor.width << "x" << base_anchor.height << " px)" << std::endl;
        }
    }

    void set_pending_roi(const cv::Rect& roi) {
        base_anchor = roi;
        enabled = true;
        initialized = false;
        cur_dx = 0;
        cur_dy = 0;
    }

    void clear() {
        enabled = false;
        initialized = false;
        template_gray.release();
        cur_dx = 0;
        cur_dy = 0;
        base_anchor = cv::Rect(0, 0, 0, 0);
        std::cout << "\n🎯 [抗抖锚点] 已清空抗抖锚点，进入纯静态 0 锚点模式。" << std::endl;
    }

    void update(const cv::Mat& gray) {
        if (!enabled) {
            cur_dx = 0;
            cur_dy = 0;
            return;
        }

        if (!initialized) {
            cv::Rect valid_r = base_anchor & cv::Rect(0, 0, gray.cols, gray.rows);
            if (valid_r.width >= 20 && valid_r.height >= 20) {
                base_anchor = valid_r;
                template_gray = gray(base_anchor).clone();
                match_score = 1.0f;
                initialized = true;
                std::cout << "\n🎯 [抗抖锚点] 异步首帧自动锁定锚点区域: [" 
                          << base_anchor.x << "," << base_anchor.y << "," 
                          << (base_anchor.x + base_anchor.width) << "," 
                          << (base_anchor.y + base_anchor.height) << "] (尺寸: " 
                          << base_anchor.width << "x" << base_anchor.height << " px)" << std::endl;
            } else {
                return;
            }
        }

        if (template_gray.empty()) {
            cur_dx = 0;
            cur_dy = 0;
            return;
        }

        // 搜索窗口限制在预估位置周边 ±90 像素
        int margin = 90;
        int sx = std::max(0, base_anchor.x + cur_dx - margin);
        int sy = std::max(0, base_anchor.y + cur_dy - margin);
        int ex = std::min(gray.cols, base_anchor.x + cur_dx + base_anchor.width + margin);
        int ey = std::min(gray.rows, base_anchor.y + cur_dy + base_anchor.height + margin);

        cv::Rect search_win(sx, sy, ex - sx, ey - sy);
        if (search_win.width >= template_gray.cols && search_win.height >= template_gray.rows) {
            cv::Mat search_area = gray(search_win);
            cv::Mat match_res;
            cv::matchTemplate(search_area, template_gray, match_res, cv::TM_CCOEFF_NORMED);

            double min_val, max_val;
            cv::Point min_loc, max_loc;
            cv::minMaxLoc(match_res, &min_val, &max_val, &min_loc, &max_loc);

            match_score = static_cast<float>(max_val);
            if (match_score >= 0.65f) {
                int matched_x = sx + max_loc.x;
                int matched_y = sy + max_loc.y;
                int new_dx = matched_x - base_anchor.x;
                int new_dy = matched_y - base_anchor.y;
                // 物理安全限幅 ±35px
                if (std::abs(new_dx) <= 35 && std::abs(new_dy) <= 35) {
                    cur_dx = new_dx;
                    cur_dy = new_dy;
                }
            } else {
                cur_dx = 0;
                cur_dy = 0;
            }
        }
    }

    cv::Rect get_current_anchor_box(int max_w, int max_h) const {
        if (!enabled || base_anchor.width <= 0) return cv::Rect(0, 0, 0, 0);
        cv::Rect r(base_anchor.x + cur_dx, base_anchor.y + cur_dy, base_anchor.width, base_anchor.height);
        return r & cv::Rect(0, 0, max_w, max_h);
    }
};

// 管道分析单元结构
struct PipeUnit {
    int id = 1;
    std::string name;
    cv::Rect base_roi;    // 基准坐标 (未抖动时的初始位置)
    cv::Rect dynamic_roi; // 实时自适应微调后的动态坐标 (基准 + dx, dy)
    float threshold = 3.5f;
    float current_energy = 0.0f;
    float avg_energy = 0.0f;
    int state = 0; // 0=OK(绿), 1=WARN(橙), 2=ALARM(红)
    double low_start = 0.0;
    double recover_start = 0.0;
    double last_alarm_time = 0.0;
    bool alarmed = false;
    int relay_channel = 0; // 3~8 对应 Y3~Y8
};

// 截断分析器类
class WaterAnalyzer {
public:
    WaterAnalyzer() : frame_idx_(0) {
        // 开机默认不加载任何死板预设框，必须由用户通过鼠标画框来标定！
        pipes_.clear();

        locate_config_path();
        load_feishu_config();

        feishu_ = std::make_shared<FeishuClient>();
        feishu_->init(feishu_app_id_.empty() ? env_or("FEISHU_APP_ID") : feishu_app_id_,
                     feishu_app_secret_.empty() ? env_or("FEISHU_APP_SECRET") : feishu_app_secret_,
                     feishu_receive_id_.empty() ? env_or("FEISHU_RECEIVE_ID") : feishu_receive_id_,
                     feishu_receive_id_type_.empty() ? env_or("FEISHU_RECEIVE_ID_TYPE", "chat_id") : feishu_receive_id_type_,
                     feishu_web_monitor_url_.empty() ? env_or("FEISHU_WEB_MONITOR_URL", "http://192.168.55.1:8080") : feishu_web_monitor_url_);
        std::cout << "[飞书联动] 纯 C++ 异步飞书告警客户端已成功装载！" << std::endl;

        relay_ = std::make_shared<RelayClient>();
        relay_->init(env_or("RELAY_HOST", "192.168.0.7"),
                     std::stoi(env_or("RELAY_PORT", "8234")),
                     std::stoi(env_or("RELAY_ENABLED", "1")) != 0);
        std::cout << "[硬件联动] 纯 C++ 原生以太网继电器驱动客户端已成功装载！" << std::endl;
    }

    ~WaterAnalyzer() {
        if (relay_) {
            relay_->reset_all();
        }
    }

    void locate_config_path() {
        std::vector<std::string> paths = {"../../water_config.json", "../water_config.json", "water_config.json"};
        for (const auto& p : paths) {
            std::ifstream in(p);
            if (in.good()) {
                config_path_ = p;
                break;
            }
        }
        if (config_path_.empty()) config_path_ = "water_config.json";
    }

    std::string feishu_app_id_ = "";
    std::string feishu_app_secret_ = "";
    std::string feishu_receive_id_type_ = "chat_id";
    std::string feishu_receive_id_ = "";
    std::string feishu_web_monitor_url_ = "http://192.168.55.1:8080";

    void load_feishu_config() {
        std::ifstream in(config_path_);
        if (!in.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        std::smatch m;
        if (std::regex_search(content, m, std::regex("\"app_id\"\\s*:\\s*\"([^\"]*)\""))) feishu_app_id_ = m[1];
        if (std::regex_search(content, m, std::regex("\"app_secret\"\\s*:\\s*\"([^\"]*)\""))) feishu_app_secret_ = m[1];
        if (std::regex_search(content, m, std::regex("\"receive_id_type\"\\s*:\\s*\"([^\"]*)\""))) feishu_receive_id_type_ = m[1];
        if (std::regex_search(content, m, std::regex("\"receive_id\"\\s*:\\s*\"([^\"]*)\""))) feishu_receive_id_ = m[1];
        if (std::regex_search(content, m, std::regex("\"web_monitor_url\"\\s*:\\s*\"([^\"]*)\""))) feishu_web_monitor_url_ = m[1];
    }

    // 核心业务决策与图像视觉可控参数
    float alarm_duration_sec_ = 2.0f;
    float recovery_duration_sec_ = 2.0f;
    float cooldown_sec_ = 60.0f;
    float ema_alpha_ = 0.75f;
    int diff_threshold_ = 12;
    int gaussian_blur_size_ = 5;

    // 热更新单个管道阈值
    void update_pipe_threshold(int id, float th) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        for (auto& p : pipes_) {
            if (p.id == id) {
                p.threshold = th;
                std::cout << "\n⚡ [热更新] Pipe " << id << " 告警阈值已变更为: " << th << std::endl;
                break;
            }
        }
        save_config();
    }

    // 热更新核心业务与高级图像算法参数
    void update_algorithm_params(float alarm_dur, float rec_dur, float cooldown, float ema_alpha, int diff_th, int blur_sz) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (alarm_dur >= 0.1f) alarm_duration_sec_ = alarm_dur;
        if (rec_dur >= 0.1f) recovery_duration_sec_ = rec_dur;
        if (cooldown >= 1.0f) cooldown_sec_ = cooldown;
        if (ema_alpha >= 0.05f && ema_alpha <= 0.99f) ema_alpha_ = ema_alpha;
        if (diff_th >= 3 && diff_th <= 80) diff_threshold_ = diff_th;
        if (blur_sz >= 3 && blur_sz <= 15 && blur_sz % 2 == 1) gaussian_blur_size_ = blur_sz;

        std::cout << "\n⚡ [热更新参数] 报警确认:" << alarm_duration_sec_ << "s, 复位延时:" 
                  << recovery_duration_sec_ << "s, 飞书冷却:" << cooldown_sec_ << "s, EMA_α:" 
                  << ema_alpha_ << ", 帧差阈值:" << diff_threshold_ << ", 高斯核:" 
                  << gaussian_blur_size_ << std::endl;
        save_config();
    }

    // 添加或更新管口标定
    void add_or_update_pipe(int id, const std::string& name, int x1, int y1, int x2, int y2, float threshold = 3.5f, int relay_channel = 0) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        int w = std::max(10, x2 - x1);
        int h = std::max(10, y2 - y1);
        int bx = x1 - tracker_.cur_dx;
        int by = y1 - tracker_.cur_dy;
        if (relay_channel <= 0) relay_channel = id + 2; // Y3~Y8

        bool found = false;
        for (auto& p : pipes_) {
            if (p.id == id) {
                p.name = name;
                p.base_roi = cv::Rect(bx, by, w, h);
                p.dynamic_roi = cv::Rect(x1, y1, w, h);
                p.threshold = threshold;
                p.relay_channel = relay_channel;
                p.current_energy = 15.0f;
                p.state = 0;
                p.low_start = 0.0;
                p.alarmed = false;
                found = true;
                break;
            }
        }

        if (!found) {
            PipeUnit p;
            p.id = id;
            p.name = name;
            p.base_roi = cv::Rect(bx, by, w, h);
            p.dynamic_roi = cv::Rect(x1, y1, w, h);
            p.threshold = threshold;
            p.relay_channel = relay_channel;
            p.current_energy = 15.0f;
            p.state = 0;
            p.low_start = 0.0;
            p.alarmed = false;
            pipes_.push_back(p);
        }

        // 按 ID 升序排序
        std::sort(pipes_.begin(), pipes_.end(), [](const PipeUnit& a, const PipeUnit& b) {
            return a.id < b.id;
        });

        std::cout << "\n🎯 [标定绑定] Pipe " << id << " (" << name << "): ["
                  << bx << "," << by << "," << (bx + w) << "," << (by + h) 
                  << "] 尺寸: " << w << "x" << h << " 阈值: " << threshold << std::endl;
    }

    // 删除单个管口
    void delete_pipe(int id) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        pipes_.erase(std::remove_if(pipes_.begin(), pipes_.end(), [id](const PipeUnit& p) {
            return p.id == id;
        }), pipes_.end());
        std::cout << "\n🗑️ [标定删除] 已移除 Pipe " << id << std::endl;
    }

    // 清空所有管口标定
    void clear_all_pipes() {
        std::lock_guard<std::mutex> lock(data_mutex_);
        pipes_.clear();
        std::cout << "\n🗑️ [标定清空] 已清空全部管口标定，等待重新画框！" << std::endl;
    }

    // 设置抗抖锚点
    void set_anchor(int x1, int y1, int x2, int y2) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (latest_gray_.empty()) return;
        tracker_.reset(latest_gray_, cv::Rect(x1, y1, x2 - x1, y2 - y1));
    }

    // 清空抗抖锚点 (回到纯静态 0 锚点模式)
    void clear_anchor() {
        std::lock_guard<std::mutex> lock(data_mutex_);
        tracker_.clear();
    }

    // 载入历史配置文件 (供用户一键恢复上次的标定作为参考)
    bool load_history_config() {
        std::ifstream in(config_path_);
        if (!in.is_open()) return false;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        std::lock_guard<std::mutex> lock(data_mutex_);
        pipes_.clear();

        try {
            // 解析管道
            std::regex pipe_regex(
                "\"id\"\\s*:\\s*(\\d+)[\\s\\S]*?\"name\"\\s*:\\s*\"([^\"]*)\"[\\s\\S]*?\"roi\"\\s*:\\s*\\[\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*\\][\\s\\S]*?\"threshold\"\\s*:\\s*([0-9.]+)[\\s\\S]*?\"relay_channel\"\\s*:\\s*(\\d+)[\\s\\S]*?\"enabled\"\\s*:\\s*(true|false)"
            );
            auto begin = std::sregex_iterator(content.begin(), content.end(), pipe_regex);
            auto end = std::sregex_iterator();

            for (std::sregex_iterator i = begin; i != end; ++i) {
                std::smatch match = *i;
                int id = std::stoi(match[1]);
                std::string name = match[2];
                int x1 = std::stoi(match[3]);
                int y1 = std::stoi(match[4]);
                int x2 = std::stoi(match[5]);
                int y2 = std::stoi(match[6]);
                float th = std::stof(match[7]);
                int rc = std::stoi(match[8]);
                bool enabled = (match[9] == "true");

                if (enabled && x2 > x1 && y2 > y1) {
                    PipeUnit p;
                    p.id = id;
                    p.name = name;
                    p.base_roi = cv::Rect(x1, y1, x2 - x1, y2 - y1);
                    p.dynamic_roi = p.base_roi;
                    p.threshold = th;
                    p.relay_channel = rc;
                    p.current_energy = 15.0f;
                    p.avg_energy = 15.0f;
                    p.state = 0;
                    p.low_start = 0.0;
                    p.alarmed = false;
                    pipes_.push_back(p);
                }
            }

            // 解析锚点
            std::regex anchor_regex("\"anchor\"\\s*:\\s*\\{[\\s\\S]*?\"enabled\"\\s*:\\s*(true|false)[\\s\\S]*?\"roi\"\\s*:\\s*\\[\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*,\\s*(\\d+)\\s*\\]");
            std::smatch am;
            if (std::regex_search(content, am, anchor_regex)) {
                bool a_enabled = (am[1] == "true");
                int ax1 = std::stoi(am[2]);
                int ay1 = std::stoi(am[3]);
                int ax2 = std::stoi(am[4]);
                int ay2 = std::stoi(am[5]);
                if (a_enabled && ax2 > ax1 && ay2 > ay1) {
                    if (!latest_gray_.empty()) {
                        tracker_.reset(latest_gray_, cv::Rect(ax1, ay1, ax2 - ax1, ay2 - ay1));
                    } else {
                        tracker_.set_pending_roi(cv::Rect(ax1, ay1, ax2 - ax1, ay2 - ay1));
                    }
                }
            }

            // 解析全局业务与图像算法参数
            std::smatch m;
            if (std::regex_search(content, m, std::regex("\"alarm_duration_sec\"\\s*:\\s*([0-9.]+)"))) alarm_duration_sec_ = std::stof(m[1]);
            if (std::regex_search(content, m, std::regex("\"recovery_duration_sec\"\\s*:\\s*([0-9.]+)"))) recovery_duration_sec_ = std::stof(m[1]);
            if (std::regex_search(content, m, std::regex("\"cooldown_sec\"\\s*:\\s*([0-9.]+)"))) cooldown_sec_ = std::stof(m[1]);
            if (std::regex_search(content, m, std::regex("\"ema_alpha\"\\s*:\\s*([0-9.]+)"))) ema_alpha_ = std::stof(m[1]);
            if (std::regex_search(content, m, std::regex("\"diff_threshold\"\\s*:\\s*(\\d+)"))) diff_threshold_ = std::stoi(m[1]);
            if (std::regex_search(content, m, std::regex("\"gaussian_blur_size\"\\s*:\\s*(\\d+)"))) gaussian_blur_size_ = std::stoi(m[1]);
        } catch (const std::exception& e) {
            std::cerr << "解析配置异常: " << e.what() << std::endl;
        }

        std::sort(pipes_.begin(), pipes_.end(), [](const PipeUnit& a, const PipeUnit& b) {
            return a.id < b.id;
        });

        std::cout << "📂 [配置载入] 成功从 " << config_path_ << " 恢复了 " << pipes_.size() << " 根历史管口标定。" << std::endl;
        return !pipes_.empty();
    }

    // 保存标定至持久化 JSON 配置文件
    void save_config() {
        std::ofstream out(config_path_);
        if (out.is_open()) {
            out << "{\n";
            out << "  \"device_name\": \"1号发电机机组 - 冷却水智能视觉防护系统\",\n";
            out << "  \"web_port\": 8080,\n";
            out << "  \"alarm_duration_sec\": " << alarm_duration_sec_ << ",\n";
            out << "  \"recovery_duration_sec\": " << recovery_duration_sec_ << ",\n";
            out << "  \"cooldown_sec\": " << cooldown_sec_ << ",\n";
            out << "  \"ema_alpha\": " << ema_alpha_ << ",\n";
            out << "  \"diff_threshold\": " << diff_threshold_ << ",\n";
            out << "  \"gaussian_blur_size\": " << gaussian_blur_size_ << ",\n";
            out << "  \"feishu\": {\n";
            out << "    \"enabled\": true,\n";
            out << "    \"app_id\": \"" << (feishu_app_id_.empty() ? env_or("FEISHU_APP_ID") : feishu_app_id_) << "\",\n";
            out << "    \"app_secret\": \"" << (feishu_app_secret_.empty() ? env_or("FEISHU_APP_SECRET") : feishu_app_secret_) << "\",\n";
            out << "    \"receive_id_type\": \"" << (feishu_receive_id_type_.empty() ? env_or("FEISHU_RECEIVE_ID_TYPE", "chat_id") : feishu_receive_id_type_) << "\",\n";
            out << "    \"receive_id\": \"" << (feishu_receive_id_.empty() ? env_or("FEISHU_RECEIVE_ID") : feishu_receive_id_) << "\",\n";
            out << "    \"web_monitor_url\": \"" << (feishu_web_monitor_url_.empty() ? env_or("FEISHU_WEB_MONITOR_URL", "http://192.168.55.1:8080") : feishu_web_monitor_url_) << "\"\n";
            out << "  },\n";
            out << "  \"anchor\": {\n";
            out << "    \"enabled\": " << (tracker_.enabled ? "true" : "false") << ",\n";
            out << "    \"roi\": [" << tracker_.base_anchor.x << ", " << tracker_.base_anchor.y << ", " 
                << (tracker_.base_anchor.x + tracker_.base_anchor.width) << ", " 
                << (tracker_.base_anchor.y + tracker_.base_anchor.height) << "]\n";
            out << "  },\n";
            out << "  \"pipes\": [\n";
            for (size_t i = 0; i < pipes_.size(); ++i) {
                const auto& p = pipes_[i];
                out << "    {\n";
                out << "      \"id\": " << p.id << ",\n";
                out << "      \"name\": \"" << p.name << "\",\n";
                out << "      \"roi\": [" << p.base_roi.x << ", " << p.base_roi.y << ", " 
                    << (p.base_roi.x + p.base_roi.width) << ", " << (p.base_roi.y + p.base_roi.height) << "],\n";
                out << "      \"threshold\": " << p.threshold << ",\n";
                out << "      \"relay_channel\": " << p.relay_channel << ",\n";
                out << "      \"enabled\": true\n";
                out << "    }" << (i + 1 < pipes_.size() ? "," : "") << "\n";
            }
            out << "  ]\n";
            out << "}\n";
            out.close();
            std::cout << "✅ [配置持久化] 最新 " << pipes_.size() << " 根管口与锚点已保存至 " << config_path_ << std::endl;
        }
    }

    // 切换至标定模式
    void enter_calibration_mode() {
        g_system_mode = MODE_CALIBRATION;
        if (relay_) relay_->reset_all();
        std::lock_guard<std::mutex> lock(data_mutex_);
        for (auto& p : pipes_) {
            p.state = 0;
            p.low_start = 0.0;
            p.alarmed = false;
        }
        std::cout << "\n🟡 [模式切换] 进入【管口标定阶段】！告警已挂起，安全等待用户画框。" << std::endl;
    }

    // 确认标定并启动正式监控
    bool start_monitoring_mode() {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (pipes_.empty()) {
            std::cerr << "⚠️ [启动失败] 当前尚未标定任何管道，无法启动监控！" << std::endl;
            return false;
        }

        save_config();
        // 重置动能状态
        for (auto& p : pipes_) {
            p.state = 0;
            p.low_start = 0.0;
            p.alarmed = false;
            p.current_energy = 15.0f;
            p.avg_energy = 15.0f;
        }
        g_system_mode = MODE_MONITORING;
        std::cout << "\n🟢 [模式切换] 标定完成！正式启动【冷却水监控预警系统】（监控管口数: " 
                  << pipes_.size() << " 根）" << std::endl;
        return true;
    }

    void toggle_cutoff_pipe2() {
        g_sim_cutoff_pipe2 = !g_sim_cutoff_pipe2;
        std::cout << "\n[截断模拟] 2号管模拟断水已切换为: " 
                  << (g_sim_cutoff_pipe2 ? "【人工截断 (动能清零)】" : "【恢复正常供水】") << std::endl;
    }

    // 核心动能分析循环 (仅在 MONITORING 模式下计算动能并触发联动)
    void process(const cv::Mat& bgr_frame, double now_sec) {
        if (bgr_frame.empty()) return;
        frame_idx_++;

        cv::Mat gray, blurred;
        cv::cvtColor(bgr_frame, gray, cv::COLOR_BGR2GRAY);
        int blur_k = (gaussian_blur_size_ % 2 == 1 && gaussian_blur_size_ >= 3) ? gaussian_blur_size_ : 5;
        cv::GaussianBlur(gray, blurred, cv::Size(blur_k, blur_k), 0);

        std::lock_guard<std::mutex> lock(data_mutex_);
        latest_gray_ = gray.clone();

        // 标定模式下：仅更新底图与锚点跟踪，绝对不进行断水判定、不触发任何告警
        if (g_system_mode == MODE_CALIBRATION) {
            tracker_.update(gray);
            prev_gray_ = blurred.clone();
            return;
        }

        // 监控模式下：启动抗抖纠偏与动能帧差
        tracker_.update(gray);
        int dx = tracker_.cur_dx;
        int dy = tracker_.cur_dy;

        if (prev_gray_.empty() || prev_gray_.size() != blurred.size()) {
            prev_gray_ = blurred.clone();
            return;
        }

        // 帧差积分
        cv::Mat diff, diff_mask;
        cv::absdiff(blurred, prev_gray_, diff);
        cv::threshold(diff, diff_mask, diff_threshold_, 255, cv::THRESH_BINARY);
        prev_gray_ = blurred.clone();

        for (auto& p : pipes_) {
            p.dynamic_roi = cv::Rect(p.base_roi.x + dx, p.base_roi.y + dy, p.base_roi.width, p.base_roi.height);

            float instant_e = 0.0f;
            if (p.id == 2 && g_sim_cutoff_pipe2) {
                instant_e = 0.0f; // 人工断水模拟
            } else {
                cv::Rect safe_roi = p.dynamic_roi & cv::Rect(0, 0, diff_mask.cols, diff_mask.rows);
                if (safe_roi.width > 0 && safe_roi.height > 0) {
                    int active = cv::countNonZero(diff_mask(safe_roi));
                    instant_e = (static_cast<float>(active) / (safe_roi.width * safe_roi.height)) * 100.0f;
                }
                // 仅在离线工位无相机仿真时提供底图微扰动
                if (!g_is_live_stream && instant_e < 1.0f) {
                    float base_val = (p.id == 1 ? 22.0f : (p.id == 2 ? 10.5f : (p.id == 3 ? 8.5f : (p.id == 4 ? 12.5f : 6.0f))));
                    float jitter = ((rand() % 100) / 100.0f - 0.5f) * 1.5f;
                    instant_e = base_val + jitter;
                }
            }

            p.current_energy = ema_alpha_ * instant_e + (1.0f - ema_alpha_) * p.current_energy;

            // 状态机流转 (断流确认延时 + 复位确认防抖延时)
            if (p.current_energy < p.threshold) {
                p.recover_start = 0.0;
                if (p.low_start <= 0.0) p.low_start = now_sec;
                double dur = now_sec - p.low_start;
                if (dur >= alarm_duration_sec_) {
                    p.state = 2; // 红色 ALARM
                    if (!p.alarmed && now_sec >= 3.0) {
                        p.alarmed = true;
                        int relay_ch = (p.relay_channel > 0) ? p.relay_channel : (p.id + 2);
                        if (relay_) {
                            relay_->set_channel(relay_ch, true);
                            relay_->set_channel(2, true); // Y2 总告警
                        }

                        if (feishu_) {
                            if (p.last_alarm_time <= 0.0 || (now_sec - p.last_alarm_time >= cooldown_sec_)) {
                                p.last_alarm_time = now_sec;
                                std::string relay_desc = "DO2(总报警) + DO" + std::to_string(relay_ch) + " (已吸合闭合)";
                                std::cout << "\n🚨 [双支柱联动] Pipe " << p.id << " (" << p.name 
                                          << ") 确诊断流 (E=" << p.current_energy << ")，闭合继电器并推飞书..." << std::endl;
                                feishu_->send_water_alarm_async(p.id, p.name, p.current_energy, p.threshold, static_cast<float>(dur), relay_desc);
                            }
                        }
                    }
                } else if (dur >= 0.5) {
                    p.state = 1; // 橙色 WARN
                }
            } else {
                p.low_start = 0.0;
                if (p.alarmed) {
                    if (p.recover_start <= 0.0) p.recover_start = now_sec;
                    double rec_dur = now_sec - p.recover_start;
                    if (rec_dur >= recovery_duration_sec_) {
                        p.state = 0; // 绿色 OK
                        p.alarmed = false;
                        p.recover_start = 0.0;
                        int relay_ch = (p.relay_channel > 0) ? p.relay_channel : (p.id + 2);
                        if (relay_) {
                            relay_->set_channel(relay_ch, false);
                            bool other_alarm = false;
                            for (const auto& other : pipes_) {
                                if (other.id != p.id && other.state == 2) {
                                    other_alarm = true;
                                    break;
                                }
                            }
                            if (!other_alarm) relay_->set_channel(2, false);
                        }

                        if (feishu_) {
                            std::cout << "\n🟢 [双支柱联动] Pipe " << p.id << " (" << p.name 
                                      << ") 供水恢复正常 (E=" << p.current_energy << ")，继电器复位并推飞书..." << std::endl;
                            feishu_->send_water_recovery_async(p.id, p.name, p.current_energy);
                        }
                    } else {
                        p.state = 1; // 恢复确认延时观察中 (保持 WARN)
                    }
                } else {
                    p.recover_start = 0.0;
                    p.state = 0; // 绿色 OK
                }
            }
        }
    }

    // 画面绘制与工业看板
    void draw(cv::Mat& display, float fps) {
        // 前后端职责完全解耦：
        // 视频帧流保持 100% 原始高清纯净，绝不在底层像素上烙印边框或文字！
        // 所有锚点框、管道动能框、中文标签与告警指示全部交由 Web 前端 HTML5 Canvas 统一渲染！
        // 彻底根除画面“双框重影”、文字模糊与 UTF-8 中文乱码问题！
        return;
    }

    void print_console_table() {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (g_system_mode == MODE_CALIBRATION) {
            std::cout << "\r🟡 [开机标定阶段] 等待用户画框... (已绑定管口: " << pipes_.size() 
                      << " 根 | 锚点: " << (tracker_.enabled ? "已启用" : "纯静态0锚点") << ") " << std::flush;
        } else {
            std::cout << "\r[微调 dx:" << std::setw(3) << tracker_.cur_dx << " dy:" << std::setw(3) << tracker_.cur_dy << "] ";
            for (const auto& p : pipes_) {
                std::string st = (p.state == 2 ? "RED" : (p.state == 1 ? "WARN" : "OK"));
                std::cout << "P" << p.id << "(" << st << "):" << std::fixed << std::setprecision(1) << p.current_energy << " | ";
            }
            std::cout << std::flush;
        }
    }

    // 生成前端 JSON 数据
    std::string get_status_json() {
        std::lock_guard<std::mutex> lock(data_mutex_);
        std::stringstream ss;
        ss << "{\n";
        ss << "  \"mode\": \"" << (g_system_mode == MODE_CALIBRATION ? "CALIBRATING" : "MONITORING") << "\",\n";
        ss << "  \"pipe_count\": " << pipes_.size() << ",\n";
        ss << "  \"cutoff_sim\": " << (g_sim_cutoff_pipe2 ? "true" : "false") << ",\n";
        ss << "  \"anchor_enabled\": " << (tracker_.enabled ? "true" : "false") << ",\n";

        // 锚点坐标：监控模式下叠加抗抖偏移量
        int ax1 = tracker_.base_anchor.x + ((g_system_mode == MODE_MONITORING && tracker_.enabled) ? tracker_.cur_dx : 0);
        int ay1 = tracker_.base_anchor.y + ((g_system_mode == MODE_MONITORING && tracker_.enabled) ? tracker_.cur_dy : 0);
        int ax2 = ax1 + tracker_.base_anchor.width;
        int ay2 = ay1 + tracker_.base_anchor.height;
        ss << "  \"anchor_roi\": [" << ax1 << "," << ay1 << "," << ax2 << "," << ay2 << "],\n";
        ss << "  \"anchor_dx\": " << tracker_.cur_dx << ",\n";
        ss << "  \"anchor_dy\": " << tracker_.cur_dy << ",\n";

        ss << "  \"alarm_duration_sec\": " << alarm_duration_sec_ << ",\n";
        ss << "  \"recovery_duration_sec\": " << recovery_duration_sec_ << ",\n";
        ss << "  \"cooldown_sec\": " << cooldown_sec_ << ",\n";
        ss << "  \"ema_alpha\": " << ema_alpha_ << ",\n";
        ss << "  \"diff_threshold\": " << diff_threshold_ << ",\n";
        ss << "  \"gaussian_blur_size\": " << gaussian_blur_size_ << ",\n";
        ss << "  \"pipes\": [\n";
        for (size_t i = 0; i < pipes_.size(); ++i) {
            const auto& p = pipes_[i];
            // 监控模式下输出动态跟踪纠偏后的实时坐标，标定模式下输出基准坐标
            int px1 = (g_system_mode == MODE_MONITORING) ? p.dynamic_roi.x : p.base_roi.x;
            int py1 = (g_system_mode == MODE_MONITORING) ? p.dynamic_roi.y : p.base_roi.y;
            int pw = p.base_roi.width;
            int ph = p.base_roi.height;
            ss << "    {\"id\": " << p.id << ", \"name\": \"" << p.name << "\", "
               << "\"x1\": " << px1 << ", \"y1\": " << py1 << ", "
               << "\"x2\": " << (px1 + pw) << ", \"y2\": " << (py1 + ph) << ", "
               << "\"w\": " << pw << ", \"h\": " << ph << ", "
               << "\"threshold\": " << p.threshold << ", \"energy\": " << std::fixed << std::setprecision(1) << p.current_energy << ", "
               << "\"state\": " << p.state << "}" << (i + 1 < pipes_.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}";
        return ss.str();
    }

private:
    std::mutex data_mutex_;
    std::vector<PipeUnit> pipes_;
    cv::Mat prev_gray_;
    cv::Mat latest_gray_;
    AnchorTracker tracker_;
    int frame_idx_;
    std::string config_path_;
    std::shared_ptr<FeishuClient> feishu_;
    std::shared_ptr<RelayClient> relay_;
};

static WaterAnalyzer g_analyzer;

// 动态打印开发板真实网络 IP
void print_available_urls(int port) {
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        std::cout << "  * 浏览器访问地址: http://0.0.0.0:" << port << std::endl;
        return;
    }

    std::cout << "\n========================================================" << std::endl;
    std::cout << "  ⚡ 冷却水视觉监控与在线标定看板服务已启动! ⚡" << std::endl;
    std::cout << "  👉 请在 Windows 电脑浏览器中打开以下地址开展鼠标画框：" << std::endl;

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            char host[NI_MAXHOST];
            sockaddr_in* pAddr = (sockaddr_in*)ifa->ifa_addr;
            inet_ntop(AF_INET, &(pAddr->sin_addr), host, NI_MAXHOST);
            std::string ifname = ifa->ifa_name;
            if (ifname != "lo") {
                std::string desc = ifname;
                if (ifname.find("eth") != std::string::npos) desc += " (工业有线网口)";
                else if (ifname.find("usb") != std::string::npos || ifname.find("l4tbr") != std::string::npos) desc += " (USB 调试通道 - 推荐)";
                else if (ifname.find("wlan") != std::string::npos) desc += " (Wi-Fi)";

                std::cout << "  👉 在线交互看板:  http://" << host << ":" << port << "  [" << desc << "]" << std::endl;
            }
        }
    }
    std::cout << "========================================================\n" << std::endl;
    freeifaddrs(ifaddr);
}

// 嵌入式 Web 交互看板 HTML (集成标定与监控状态机)
const char* CALIBRATE_HTML = R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>发电机冷却水管口在线鼠标标定与智能预警</title>
<style>
  * { box-sizing: border-box; }
  body { background: #0d1117; color: #e6edf3; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; margin: 0; padding: 16px; display: flex; flex-direction: column; align-items: center; }
  .header { display: flex; align-items: center; justify-content: space-between; width: 100%; max-width: 1440px; margin-bottom: 12px; border-bottom: 1px solid #21262d; padding-bottom: 10px; }
  .header-title { display: flex; align-items: center; gap: 12px; }
  .header-title h2 { margin: 0; font-size: 20px; color: #58a6ff; }
  .mode-badge { padding: 4px 10px; border-radius: 20px; font-size: 13px; font-weight: bold; }
  .mode-calib { background: #d2992226; color: #e3b341; border: 1px solid #d29922; }
  .mode-monitor { background: #23863626; color: #3fb950; border: 1px solid #238636; }
  .layout { display: flex; gap: 18px; max-width: 1440px; width: 100%; justify-content: center; }
  .canvas-box { position: relative; flex: 1; border: 2px solid #30363d; border-radius: 8px; overflow: hidden; background: #000; box-shadow: 0 8px 24px rgba(0,0,0,0.8); display: flex; align-items: center; justify-content: center; }
  #stream-img { display: block; width: 100%; height: auto; max-height: 82vh; object-fit: contain; }
  #draw-canvas { position: absolute; left: 0; top: 0; cursor: crosshair; }
  .sidebar { width: 360px; display: flex; flex-direction: column; gap: 12px; }
  .card { background: #161b22; border: 1px solid #30363d; border-radius: 8px; padding: 14px; }
  .card h3 { font-size: 14px; margin-top: 0; margin-bottom: 8px; color: #f0f6fc; display: flex; justify-content: space-between; align-items: center; }
  .btn { display: block; width: 100%; padding: 8px 12px; margin-bottom: 6px; background: #238636; color: #fff; border: none; border-radius: 6px; font-weight: bold; cursor: pointer; text-align: center; font-size: 13px; transition: 0.15s; }
  .btn:hover { background: #2ea043; }
  .btn-primary { background: #1f6feb; }
  .btn-primary:hover { background: #388bfd; }
  .btn-anchor { background: #8957e5; }
  .btn-anchor:hover { background: #a371f7; }
  .btn-danger { background: #da3633; }
  .btn-danger:hover { background: #f85149; }
  .btn-warn { background: #d29922; color: #000; }
  .btn-warn:hover { background: #e3b341; }
  .btn-secondary { background: #21262d; border: 1px solid #30363d; color: #c9d1d9; }
  .btn-secondary:hover { background: #30363d; color: #fff; }
  .btn-lg { padding: 12px; font-size: 15px; border-radius: 8px; box-shadow: 0 4px 12px rgba(35,134,54,0.4); }
  .tip { font-size: 12px; color: #8b949e; line-height: 1.4; margin-top: 4px; }
  #info { font-family: monospace; font-size: 12px; color: #7ee787; background: #0d1117; padding: 8px; border-radius: 4px; border: 1px solid #30363d; margin-bottom: 8px; word-break: break-all; min-height: 34px; display: flex; align-items: center; }
  .pipe-list { display: flex; flex-direction: column; gap: 6px; max-height: 200px; overflow-y: auto; }
  .pipe-item { display: flex; justify-content: space-between; align-items: center; background: #0d1117; padding: 6px 10px; border-radius: 4px; border: 1px solid #30363d; font-size: 12px; }
  .pipe-item .del-btn { background: none; border: none; color: #f85149; cursor: pointer; font-size: 14px; padding: 0 4px; }
  .pipe-item .del-btn:hover { color: #ff7b72; }
  .pipe-pill { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 6px; }
  .anchor-card { background: #0d1117; padding: 8px; border-radius: 4px; border: 1px solid #30363d; font-size: 12px; display: flex; justify-content: space-between; align-items: center; margin-top: 6px; }
</style>
</head>
<body>
  <div class="header">
    <div class="header-title">
      <h2>💧 发电机冷却水智能视觉防护系统</h2>
      <span id="mode-badge" class="mode-badge mode-calib">🟡 开机标定阶段 (请先鼠标画框)</span>
    </div>
    <div style="display: flex; gap: 8px;">
      <button id="recalib-btn" class="btn btn-warn" style="width: auto; margin: 0; display: none;" onclick="enterCalibration()">⚙️ 重新标定管口</button>
      <button class="btn btn-secondary" style="width: auto; margin: 0;" onclick="toggleCutoff()">🚨 模拟断流演练</button>
    </div>
  </div>

  <div class="layout">
    <div class="canvas-box">
      <img id="stream-img" src="/video_feed" onload="initCanvas()">
      <canvas id="draw-canvas"></canvas>
    </div>
    <div class="sidebar">
      <div class="card">
        <h3>🎯 鼠标选框状态 <span id="box-dim" style="font-size:11px;color:#8b949e;"></span></h3>
        <div id="info">请按住鼠标左键在画面水管出水口处拖拽拉框...</div>
        <div class="tip">画好框后，点击下方对应管口按钮（或按键盘数字键 1~6）即可绑定！</div>
      </div>

      <div class="card" id="bind-card">
        <h3>📍 管口绑定快捷键</h3>
        <button class="btn btn-secondary" onclick="bindPipe(1, '左侧主粗管(大流量)', 4.0)">👉 绑定给 1号管 (左侧主粗管) [按1]</button>
        <button class="btn btn-secondary" onclick="bindPipe(2, '中间细黄管(直流水)', 3.5)">👉 绑定给 2号管 (中间细黄管) [按2]</button>
        <button class="btn btn-secondary" onclick="bindPipe(3, '中偏右银管(散流水)', 3.5)">👉 绑定给 3号管 (中偏右银管) [按3]</button>
        <button class="btn btn-secondary" onclick="bindPipe(4, '右侧弯管口(直流水)', 3.5)">👉 绑定给 4号管 (右侧弯管口) [按4]</button>
        <button class="btn btn-secondary" onclick="bindPipe(5, '最右顺壁管(贴壁细流)', 2.5)">👉 绑定给 5号管 (最右顺壁管) [按5]</button>
        <button class="btn btn-secondary" onclick="bindPipe(6, '第6路预留管道', 3.0)">👉 绑定给 6号管 (备用预留管) [按6]</button>
      </div>

      <div class="card">
        <h3>⚓ 机位抗抖锚点 (可选)</h3>
        <button class="btn btn-anchor" onclick="setAnchor()">🎯 将当前框设为【机位抗抖固定锚点】[按A]</button>
        <div id="anchor-status" class="anchor-card">
          <span>未启用 (纯静态 0 锚点模式)</span>
        </div>
        <div class="tip">建议尺寸约 60~100 像素，框住固定螺母或角铁。固定机位建议保持纯静态。</div>
      </div>

      <div class="card">
        <h3>📋 已标定管口列表 (<span id="pipe-count">0</span> 根)</h3>
        <div id="pipe-list" class="pipe-list">
          <div style="color:#8b949e;text-align:center;padding:12px 0;">暂无管口，请在画面上框选...</div>
        </div>
      </div>

      <div class="card">
        <h3>🚀 系统流程控制</h3>
        <button id="start-btn" class="btn btn-lg" onclick="startMonitoring()">🚀 确认标定完成，开始正式监控</button>
        <button class="btn btn-secondary" onclick="clearAllPipes()">🗑️ 清空所有框重新画</button>
        <button class="btn btn-secondary" onclick="loadHistory()">📂 载入上次保存的历史配置</button>
      </div>
    </div>
  </div>

  <script>
    /* __SERVER_BOOTSTRAP_PLACEHOLDER__ */
    const img = document.getElementById('stream-img');
    const canvas = document.getElementById('draw-canvas');
    const ctx = canvas.getContext('2d');
    let isDrawing = false, startX = 0, startY = 0, currentBox = null;
    let serverPipes = [], serverAnchor = null, systemMode = "CALIBRATING";

    const PIPE_COLORS = ['#ffcc00', '#00ff80', '#00c8ff', '#ff66ff', '#64b4ff', '#ff9933'];

    function initCanvas() {
      canvas.width = img.clientWidth;
      canvas.height = img.clientHeight;
      redrawOverlay();
    }
    window.onresize = initCanvas;

    canvas.addEventListener('mousedown', (e) => {
      const r = canvas.getBoundingClientRect();
      startX = e.clientX - r.left;
      startY = e.clientY - r.top;
      isDrawing = true;
      currentBox = null;
    });

    canvas.addEventListener('mousemove', (e) => {
      if (!isDrawing) return;
      const r = canvas.getBoundingClientRect();
      const currX = e.clientX - r.left;
      const currY = e.clientY - r.top;
      const x1 = Math.min(startX, currX);
      const y1 = Math.min(startY, currY);
      const w = Math.abs(currX - startX);
      const h = Math.abs(currY - startY);
      currentBox = { x1, y1, x2: x1 + w, y2: y1 + h };

      redrawOverlay();

      ctx.strokeStyle = '#00ffcc';
      ctx.lineWidth = 2;
      ctx.setLineDash([4, 4]);
      ctx.strokeRect(x1, y1, w, h);
      ctx.setLineDash([]);

      const scaleX = (img.naturalWidth || 1920) / canvas.width;
      const scaleY = (img.naturalHeight || 1080) / canvas.height;
      const ox1 = Math.round(x1 * scaleX), oy1 = Math.round(y1 * scaleY);
      const ox2 = Math.round((x1 + w) * scaleX), oy2 = Math.round((y1 + h) * scaleY);
      const ow = ox2 - ox1, oh = oy2 - oy1;
      document.getElementById('info').innerText = `选框: [${ox1}, ${oy1}, ${ox2}, ${oy2}]  尺寸: ${ow}x${oh} px`;
      document.getElementById('box-dim').innerText = `${ow}x${oh} px`;
    });

    canvas.addEventListener('mouseup', () => { isDrawing = false; });

    function redrawOverlay() {
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      const scaleX = canvas.width / (img.naturalWidth || 1920);
      const scaleY = canvas.height / (img.naturalHeight || 1080);

      // 绘制已保存的锚点框
      if (serverAnchor && serverAnchor.enabled) {
        const ax = serverAnchor.roi[0] * scaleX;
        const ay = serverAnchor.roi[1] * scaleY;
        const aw = (serverAnchor.roi[2] - serverAnchor.roi[0]) * scaleX;
        const ah = (serverAnchor.roi[3] - serverAnchor.roi[1]) * scaleY;
        ctx.strokeStyle = '#a371f7';
        ctx.lineWidth = 2;
        ctx.strokeRect(ax, ay, aw, ah);
        ctx.fillStyle = '#a371f7';
        ctx.font = 'bold 11px sans-serif';
        ctx.fillText('⚓ 锚点 [已锁定]', ax, Math.max(12, ay - 4));
      }

      // 绘制各已绑定管道框
      serverPipes.forEach((p, idx) => {
        const px = p.x1 * scaleX;
        const py = p.y1 * scaleY;
        const pw = p.w * scaleX;
        const ph = p.h * scaleY;
        const col = PIPE_COLORS[(p.id - 1) % PIPE_COLORS.length];

        ctx.strokeStyle = col;
        ctx.lineWidth = 2;
        ctx.strokeRect(px, py, pw, ph);

        ctx.fillStyle = col;
        ctx.font = 'bold 11px sans-serif';
        ctx.fillText(`P${p.id}: ${p.name}`, px, Math.max(12, py - 4));
      });
    }

    function getSelectedCoords() {
      if (!currentBox || (currentBox.x2 - currentBox.x1 < 10)) {
        alert("请先用鼠标在画面上拖拽拉出一个框！");
        return null;
      }
      const scaleX = (img.naturalWidth || 1920) / canvas.width;
      const scaleY = (img.naturalHeight || 1080) / canvas.height;
      return {
        x1: Math.round(currentBox.x1 * scaleX),
        y1: Math.round(currentBox.y1 * scaleY),
        x2: Math.round(currentBox.x2 * scaleX),
        y2: Math.round(currentBox.y2 * scaleY)
      };
    }

    function bindPipe(id, defaultName, defaultTh) {
      const c = getSelectedCoords();
      if (!c) return;
      const name = encodeURIComponent(defaultName);
      fetch(`/set_pipe?id=${id}&name=${name}&x1=${c.x1}&y1=${c.y1}&x2=${c.x2}&y2=${c.y2}&th=${defaultTh}&rc=${id+2}`)
        .then(r => r.json())
        .then(() => {
          document.getElementById('info').innerText = `✅ Pipe ${id} 已成功绑定坐标！`;
          currentBox = null;
          fetchStatus();
        });
    }

    function setAnchor() {
      const c = getSelectedCoords();
      if (!c) return;
      fetch(`/set_anchor?x1=${c.x1}&y1=${c.y1}&x2=${c.x2}&y2=${c.y2}`)
        .then(r => r.json())
        .then(() => {
          document.getElementById('info').innerText = `⚓ 抗抖锚点设置成功！`;
          currentBox = null;
          fetchStatus();
        });
    }

    function clearAnchor() {
      fetch('/clear_anchor').then(r => r.json()).then(() => fetchStatus());
    }

    function deletePipe(id) {
      if (!confirm(`确认移除 Pipe ${id} 吗？`)) return;
      fetch(`/delete_pipe?id=${id}`).then(r => r.json()).then(() => fetchStatus());
    }

    function clearAllPipes() {
      if (!confirm("确认清空所有已标定管口吗？")) return;
      fetch('/clear_pipes').then(r => r.json()).then(() => fetchStatus());
    }

    function loadHistory() {
      fetch('/load_history').then(r => r.json()).then(() => {
        alert("已载入上次保存的历史配置！");
        fetchStatus();
      });
    }

    function startMonitoring() {
      if (serverPipes.length === 0) {
        alert("⚠️ 尚未标定任何管口！请先至少框选 1 根水管出水口。");
        return;
      }
      fetch('/start_monitoring').then(r => r.json()).then(res => {
        if (res.status === 'ok') {
          alert("🚀 标定成功！系统已进入正式监控阶段。");
          fetchStatus();
        } else {
          alert("启动失败: " + res.msg);
        }
      });
    }

    function enterCalibration() {
      if (!confirm("确认切回标定模式吗？监控和报警将暂停。")) return;
      fetch('/enter_calibrate').then(r => r.json()).then(() => {
        fetchStatus();
      });
    }

    function toggleCutoff() {
      fetch('/cutoff').then(r => r.json()).then(() => {
        alert("已切换 2号管断水演练状态！");
        fetchStatus();
      });
    }

    function fetchStatus() {
      fetch('/api/status')
        .then(r => r.json())
        .then(data => {
          serverPipes = data.pipes || [];
          serverAnchor = { enabled: data.anchor_enabled, roi: data.anchor_roi };
          systemMode = data.mode;

          // 更新状态徽章
          const badge = document.getElementById('mode-badge');
          const recalibBtn = document.getElementById('recalib-btn');
          const bindCard = document.getElementById('bind-card');
          if (systemMode === 'MONITORING') {
            badge.innerText = '🟢 正式监控阶段 (实时防护中)';
            badge.className = 'mode-badge mode-monitor';
            recalibBtn.style.display = 'block';
            bindCard.style.opacity = '0.6';
          } else {
            badge.innerText = '🟡 开机标定阶段 (请先鼠标画框)';
            badge.className = 'mode-badge mode-calib';
            recalibBtn.style.display = 'none';
            bindCard.style.opacity = '1.0';
          }

          // 更新管口列表
          document.getElementById('pipe-count').innerText = serverPipes.length;
          const listEl = document.getElementById('pipe-list');
          if (serverPipes.length === 0) {
            listEl.innerHTML = '<div style="color:#8b949e;text-align:center;padding:12px 0;">暂无管口，请在画面上框选...</div>';
          } else {
            listEl.innerHTML = serverPipes.map((p, idx) => `
              <div class="pipe-item">
                <span>
                  <span class="pipe-pill" style="background:${PIPE_COLORS[(p.id-1)%PIPE_COLORS.length]}"></span>
                  <strong>P${p.id}</strong>: ${p.name} <small style="color:#8b949e;">[${p.w}x${p.h}]</small>
                </span>
                <button class="del-btn" onclick="deletePipe(${p.id})" title="删除此管">✕</button>
              </div>
            `).join('');
          }

          // 更新锚点状态
          const aEl = document.getElementById('anchor-status');
          if (data.anchor_enabled) {
            const r = data.anchor_roi;
            aEl.innerHTML = `<span>⚓ 已锁定: [${r[0]},${r[1]}] (${r[2]-r[0]}x${r[3]-r[1]} px)</span>
                             <button class="del-btn" onclick="clearAnchor()" title="清空锚点">✕</button>`;
          } else {
            aEl.innerHTML = `<span>未启用 (纯静态 0 锚点模式)</span>`;
          }

          redrawOverlay();
        })
        .catch(() => {});
    }

    // 快捷键支持
    window.addEventListener('keydown', (e) => {
      if (e.key >= '1' && e.key <= '6') {
        const id = parseInt(e.key);
        const names = ['', '左侧主粗管(大流量)', '中间细黄管(直流水)', '中偏右银管(散流水)', '右侧弯管口(直流水)', '最右顺壁管(贴壁细流)', '第6路预留管道'];
        const ths = [0, 4.0, 3.5, 3.5, 3.5, 2.5, 3.0];
        bindPipe(id, names[id], ths[id]);
      } else if (e.key === 'a' || e.key === 'A') {
        setAnchor();
      } else if (e.key === 'Enter') {
        if (systemMode === 'CALIBRATING') startMonitoring();
      }
    });

    setInterval(fetchStatus, 1500);
    fetchStatus();
  </script>
</body>
</html>)html";

// Web HTTP Server Worker
void http_server_worker(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        return;
    }
    listen(server_fd, 64);

    print_available_urls(port);

    while (g_running) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }

        std::thread([client_fd]() {
            struct timeval tv{};
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

            char req[4096] = {0};
            int n = recv(client_fd, req, sizeof(req) - 1, 0);
            if (n <= 0) { close(client_fd); return; }

            std::string req_str(req);

            auto send_json = [client_fd](const std::string& body) {
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " 
                                 + std::to_string(body.length()) + "\r\nConnection: close\r\n\r\n" + body;
                send(client_fd, resp.c_str(), resp.length(), MSG_NOSIGNAL);
                close(client_fd);
            };

            // 1. 获取系统实时状态与管口清单 /api/status
            if (req_str.find("/api/status") != std::string::npos) {
                send_json(g_analyzer.get_status_json());
                return;
            }

            // 2. 绑定/更新管口 /set_pipe?id=1&name=...&x1=...&y1=...&x2=...&y2=...&th=3.5&rc=3
            if (req_str.find("/set_pipe?") != std::string::npos) {
                int id = 1, x1 = 0, y1 = 0, x2 = 0, y2 = 0, rc = 0;
                float th = 3.5f;
                char raw_name[128] = {0};

                size_t q_pos = req_str.find("/set_pipe?");
                if (q_pos != std::string::npos) {
                    std::string qs = req_str.substr(q_pos + 10);
                    size_t sp = qs.find(' ');
                    if (sp != std::string::npos) qs = qs.substr(0, sp);

                    sscanf(qs.c_str(), "id=%d&name=%127[^&]&x1=%d&y1=%d&x2=%d&y2=%d&th=%f&rc=%d",
                           &id, raw_name, &x1, &y1, &x2, &y2, &th, &rc);

                    // 简单 URL 解码
                    std::string name = raw_name;
                    std::string decoded_name;
                    for (size_t i = 0; i < name.length(); ++i) {
                        if (name[i] == '%' && i + 2 < name.length()) {
                            int hex_val = 0;
                            sscanf(name.substr(i + 1, 2).c_str(), "%x", &hex_val);
                            decoded_name += static_cast<char>(hex_val);
                            i += 2;
                        } else if (name[i] == '+') {
                            decoded_name += ' ';
                        } else {
                            decoded_name += name[i];
                        }
                    }
                    if (decoded_name.empty()) decoded_name = "Pipe " + std::to_string(id);

                    g_analyzer.add_or_update_pipe(id, decoded_name, x1, y1, x2, y2, th, rc);
                }
                send_json("{\"status\":\"ok\"}");
                return;
            }

            // 3. 删除指定管口 /delete_pipe?id=1
            if (req_str.find("/delete_pipe?") != std::string::npos) {
                int id = 1;
                size_t q_pos = req_str.find("/delete_pipe?");
                if (q_pos != std::string::npos) {
                    sscanf(req_str.substr(q_pos + 13).c_str(), "id=%d", &id);
                    g_analyzer.delete_pipe(id);
                }
                send_json("{\"status\":\"ok\"}");
                return;
            }

            // 4. 清空所有管口 /clear_pipes
            if (req_str.find("/clear_pipes") != std::string::npos) {
                g_analyzer.clear_all_pipes();
                send_json("{\"status\":\"ok\"}");
                return;
            }

            // 5. 设置抗抖锚点 /set_anchor?x1=...&y1=...&x2=...&y2=...
            if (req_str.find("/set_anchor?") != std::string::npos) {
                int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                size_t q_pos = req_str.find("/set_anchor?");
                if (q_pos != std::string::npos) {
                    sscanf(req_str.substr(q_pos + 12).c_str(), "x1=%d&y1=%d&x2=%d&y2=%d", &x1, &y1, &x2, &y2);
                    g_analyzer.set_anchor(x1, y1, x2, y2);
                }
                send_json("{\"status\":\"ok\"}");
                return;
            }

            // 6. 清除抗抖锚点 /clear_anchor
            if (req_str.find("/clear_anchor") != std::string::npos) {
                g_analyzer.clear_anchor();
                send_json("{\"status\":\"ok\"}");
                return;
            }

            // 7. 载入历史配置 /load_history
            if (req_str.find("/load_history") != std::string::npos) {
                bool ok = g_analyzer.load_history_config();
                send_json(ok ? "{\"status\":\"ok\"}" : "{\"status\":\"error\",\"msg\":\"config_not_found\"}");
                return;
            }

            // 8. 确认标定，开始正式监控 /start_monitoring
            if (req_str.find("/start_monitoring") != std::string::npos) {
                bool ok = g_analyzer.start_monitoring_mode();
                send_json(ok ? "{\"status\":\"ok\",\"msg\":\"started\"}" : "{\"status\":\"error\",\"msg\":\"no_pipes\"}");
                return;
            }

            // 9. 重新进入标定模式 /enter_calibrate
            if (req_str.find("/enter_calibrate") != std::string::npos) {
                g_analyzer.enter_calibration_mode();
                send_json("{\"status\":\"ok\",\"msg\":\"calibrating\"}");
                return;
            }

            // 10. 模拟断流接口 /cutoff
            if (req_str.find("/cutoff") != std::string::npos) {
                g_analyzer.toggle_cutoff_pipe2();
                send_json("{\"status\":\"ok\"}");
                return;
            }

            auto get_param_float = [](const std::string& qs, const std::string& key, float def) -> float {
                size_t pos = qs.find(key + "=");
                if (pos == std::string::npos) return def;
                try {
                    size_t start = pos + key.length() + 1;
                    size_t end = qs.find('&', start);
                    std::string val_str = (end == std::string::npos) ? qs.substr(start) : qs.substr(start, end - start);
                    return std::stof(val_str);
                } catch (...) {
                    return def;
                }
            };

            auto get_param_int = [](const std::string& qs, const std::string& key, int def) -> int {
                size_t pos = qs.find(key + "=");
                if (pos == std::string::npos) return def;
                try {
                    size_t start = pos + key.length() + 1;
                    size_t end = qs.find('&', start);
                    std::string val_str = (end == std::string::npos) ? qs.substr(start) : qs.substr(start, end - start);
                    return std::stoi(val_str);
                } catch (...) {
                    return def;
                }
            };

            // 11. 更新单个管道阈值 /api/set_threshold?id=1&th=4.0
            if (req_str.find("/api/set_threshold?") != std::string::npos) {
                size_t q_pos = req_str.find("/api/set_threshold?");
                if (q_pos != std::string::npos) {
                    std::string qs = req_str.substr(q_pos + 19);
                    size_t sp = qs.find(' ');
                    if (sp != std::string::npos) qs = qs.substr(0, sp);

                    int id = get_param_int(qs, "id", 1);
                    float th = get_param_float(qs, "th", 3.5f);
                    g_analyzer.update_pipe_threshold(id, th);
                }
                send_json("{\"status\":\"ok\"}");
                return;
            }

            // 12. 更新业务决策与图像算法高级参数 /api/update_params?alarm_dur=...&rec_dur=...&cooldown=...&ema_alpha=...&diff_th=...&blur_sz=...
            if (req_str.find("/api/update_params?") != std::string::npos) {
                size_t q_pos = req_str.find("/api/update_params?");
                if (q_pos != std::string::npos) {
                    std::string qs = req_str.substr(q_pos + 19);
                    size_t sp = qs.find(' ');
                    if (sp != std::string::npos) qs = qs.substr(0, sp);

                    float alarm_dur = get_param_float(qs, "alarm_dur", -1.0f);
                    float rec_dur = get_param_float(qs, "rec_dur", -1.0f);
                    float cooldown = get_param_float(qs, "cooldown", -1.0f);
                    float ema_alpha = get_param_float(qs, "ema_alpha", -1.0f);
                    int diff_th = get_param_int(qs, "diff_th", -1);
                    int blur_sz = get_param_int(qs, "blur_sz", -1);

                    g_analyzer.update_algorithm_params(alarm_dur, rec_dur, cooldown, ema_alpha, diff_th, blur_sz);
                }
                send_json("{\"status\":\"ok\"}");
                return;
            }

            // 13. 首页或标定页访问 -> 返回全功能交互式 HTML (动态注入当前系统最新状态与锚点/管口坐标)
            bool is_page_req = false;
            if (req_str.find("/api/") == std::string::npos &&
                req_str.find("/video_feed") == std::string::npos &&
                req_str.find("/set_pipe") == std::string::npos &&
                req_str.find("/set_anchor") == std::string::npos &&
                req_str.find("/clear_anchor") == std::string::npos &&
                req_str.find("/delete_pipe") == std::string::npos &&
                req_str.find("/clear_pipes") == std::string::npos &&
                req_str.find("/load_history") == std::string::npos &&
                req_str.find("/start_monitoring") == std::string::npos &&
                req_str.find("/enter_calibrate") == std::string::npos &&
                req_str.find("/cutoff") == std::string::npos) {
                if (req_str.find("GET / ") != std::string::npos ||
                    req_str.find("GET /?") != std::string::npos ||
                    req_str.find("GET /index.html") != std::string::npos ||
                    req_str.find("GET /calibrate") != std::string::npos) {
                    is_page_req = true;
                }
            }

            if (is_page_req) {
                std::string html;
                std::vector<std::string> html_paths = {
                    "calibrate_studio.html",
                    "../calibrate_studio.html",
                    "/data/workspace/water/src/test/calibrate_studio.html"
                };
                for (const auto& hp : html_paths) {
                    std::ifstream f(hp);
                    if (f.good()) {
                        std::stringstream buffer;
                        buffer << f.rdbuf();
                        html = buffer.str();
                        break;
                    }
                }
                if (html.empty()) html = CALIBRATE_HTML;

                // 核心修复：服务端动态注入当前实时状态与锚点/管口坐标
                // 确保新打开的浏览器标签页（如飞书告警/恢复卡片点击跳转）第一帧即可 100% 同步并绘制出全部管口和抗抖锚点！
                std::string status_json = g_analyzer.get_status_json();
                std::string placeholder = "/* __SERVER_BOOTSTRAP_PLACEHOLDER__ */";
                size_t p_pos = html.find(placeholder);
                if (p_pos != std::string::npos) {
                    html.replace(p_pos, placeholder.length(), "window.__INITIAL_SERVER_STATE__ = " + status_json + ";");
                }

                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " 
                                 + std::to_string(html.length()) + "\r\nConnection: close\r\n\r\n" + html;
                send(client_fd, resp.c_str(), resp.length(), MSG_NOSIGNAL);
                close(client_fd);
                return;
            }

            // 14. /video_feed -> 纯 MJPEG 高速实时推流
            struct timeval tv_snd{};
            tv_snd.tv_sec = 2;
            tv_snd.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_snd, sizeof(tv_snd));

            std::string header = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                "Connection: close\r\n"
                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Pragma: no-cache\r\n"
                "Expires: 0\r\n\r\n";
            send(client_fd, header.c_str(), header.length(), MSG_NOSIGNAL);

            while (g_running) {
                std::vector<uchar> buf;
                {
                    std::lock_guard<std::mutex> lock(g_frame_mutex);
                    buf = g_jpeg_buffer;
                }

                if (!buf.empty()) {
                    std::string part = 
                        "--frame\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: " + std::to_string(buf.size()) + "\r\n\r\n";
                    
                    if (send(client_fd, part.c_str(), part.length(), MSG_NOSIGNAL) <= 0) break;
                    if (send(client_fd, (char*)buf.data(), buf.size(), MSG_NOSIGNAL) <= 0) break;
                    if (send(client_fd, "\r\n", 2, MSG_NOSIGNAL) <= 0) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(33)); // 约 30 FPS
            }
            close(client_fd);
        }).detach();
    }
    close(server_fd);
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    gst_init(&argc, &argv);

    std::cout << "\n==========================================================" << std::endl;
    std::cout << "  ⚡ 发电机冷却水智能视觉预警系统 (开机鼠标标定版) ⚡   " << std::endl;
    std::cout << "==========================================================" << std::endl;

    std::string video_path = (argc > 1) ? argv[1] : env_or("RTSP_URL", "rtsp://username:password@camera-host:554/path");
    g_is_live_stream = (video_path.find("rtsp://") == 0 || video_path.find("http://") == 0);

    std::cout << "[输入源] " << (g_is_live_stream ? "网络实时摄像头流 (RTSP)" : "工位离线测试 (实拍底图)") << ": " << video_path << std::endl;

    GstElement* pipeline = nullptr;
    GstElement* sink = nullptr;
    cv::Mat sim_base_frame;

    if (g_is_live_stream) {
        std::string pipe_str = "rtspsrc location=" + video_path + 
                   " protocols=tcp latency=100 ! rtph265depay ! nvv4l2decoder ! nvvidconv ! "
                   "video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR ! appsink name=sink max-buffers=1 drop=true";
        GError* error = nullptr;
        pipeline = gst_parse_launch(pipe_str.c_str(), &error);
        if (!pipeline || error) {
            std::cerr << "❌ GStreamer 流水线启动失败: " << (error ? error->message : "未知错误") << std::endl;
            return -1;
        }
        sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        std::cout << "[Live] 硬件解码就绪，正在接收海康 RTSP 摄像头实时流..." << std::endl;
    } else {
        std::vector<std::string> img_paths = {
            "/data/workspace/water/images/water_5_pipes_snapshot.jpg",
            "/data/workspace/water/images/water_flow_sample.jpg",
            "../../images/water_5_pipes_snapshot.jpg",
            "../images/water_5_pipes_snapshot.jpg"
        };
        for (const auto& p : img_paths) {
            sim_base_frame = cv::imread(p);
            if (!sim_base_frame.empty()) {
                std::cout << "[工位仿真] 成功载入实拍基准底图: " << p << " (" 
                          << sim_base_frame.cols << "x" << sim_base_frame.rows << ")" << std::endl;
                break;
            }
        }
        if (sim_base_frame.empty()) {
            sim_base_frame = cv::Mat(1080, 1920, CV_8UC3, cv::Scalar(40, 40, 40));
        }
        std::cout << "[Live] 工位测试模式就绪，正在以 30 FPS 提供 Web 实时推流与画框标定..." << std::endl;
    }

    // 启动后台 Web 服务 (8080 端口)
    int web_port = 8080;
    std::thread web_thread(http_server_worker, web_port);
    web_thread.detach();

    auto start_time = std::chrono::steady_clock::now();
    auto last_fps_time = start_time;
    int frame_count = 0;
    float fps = 30.0f;
    auto last_print_time = start_time;

    // JPEG 压缩参数
    std::vector<int> encode_params = {cv::IMWRITE_JPEG_QUALITY, 80};

    while (g_running) {
        cv::Mat frame;

        if (g_is_live_stream) {
            GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 50 * GST_MSECOND);
            if (!sample) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            GstCaps* caps = gst_sample_get_caps(sample);
            GstStructure* s = gst_caps_get_structure(caps, 0);
            int width = 0, height = 0;
            gst_structure_get_int(s, "width", &width);
            gst_structure_get_int(s, "height", &height);

            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            gst_buffer_map(buffer, &map, GST_MAP_READ);

            cv::Mat raw_frame(height, width, CV_8UC3, (char*)map.data);
            frame = raw_frame.clone();

            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);
        } else {
            frame = sim_base_frame.clone();
            cv::Mat noise(280, 460, CV_8UC3);
            cv::randu(noise, cv::Scalar(0, 0, 0), cv::Scalar(25, 25, 25));
            cv::add(frame(cv::Rect(380, 480, 460, 280)), noise, frame(cv::Rect(380, 480, 460, 280)));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }

        auto now = std::chrono::steady_clock::now();
        double now_sec = std::chrono::duration<double>(now - start_time).count();

        // 1. 动能分析与状态机流转 (标定阶段仅更新图像，不触发告警)
        g_analyzer.process(frame, now_sec);

        // 2. 计算实时 FPS
        frame_count++;
        double fps_dur = std::chrono::duration<double>(now - last_fps_time).count();
        if (fps_dur >= 0.5) {
            fps = static_cast<float>(frame_count / fps_dur);
            frame_count = 0;
            last_fps_time = now;
        }

        // 3. 绘制画面状态与工业看板
        cv::Mat display = frame;
        g_analyzer.draw(display, fps);

        // 4. 将画面压缩为 JPEG 供 Web 实时拉取
        {
            std::vector<uchar> buf;
            cv::imencode(".jpg", display, buf, encode_params);
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_jpeg_buffer = std::move(buf);
        }

        // 5. 控制台每 0.5 秒输出当前状态
        if (std::chrono::duration<double>(now - last_print_time).count() >= 0.5) {
            g_analyzer.print_console_table();
            last_print_time = now;
        }
    }

    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
    }
    std::cout << "\n[退出] 程序已安全结束。" << std::endl;
    return 0;
}
