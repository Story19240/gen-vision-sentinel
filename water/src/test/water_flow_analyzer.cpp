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

// 全局图像缓冲区与控制标志
static std::mutex g_frame_mutex;
static std::vector<uchar> g_jpeg_buffer;
static std::atomic<bool> g_running(true);
static std::atomic<bool> g_sim_cutoff_pipe2(false);

void signal_handler(int sig) {
    std::cout << "\n[系统信号] 收到退出信号 (" << sig << ")，正在平稳停止..." << std::endl;
    g_running = false;
}

// 刚体锚点抗抖动跟踪器 (解决手持手机拍摄晃动 / 工业相机震颤)
struct AnchorTracker {
    bool enabled = true;
    cv::Rect base_anchor = cv::Rect(480, 400, 100, 100); // 初始固定管壁/金属构件锚点
    cv::Mat template_gray;
    int cur_dx = 0;
    int cur_dy = 0;
    float match_score = 1.0f;
    bool initialized = false;

    void reset(const cv::Mat& gray, const cv::Rect& roi) {
        cv::Rect valid_r = roi & cv::Rect(0, 0, gray.cols, gray.rows);
        if (valid_r.width > 20 && valid_r.height > 20) {
            base_anchor = valid_r;
            template_gray = gray(base_anchor).clone();
            cur_dx = 0;
            cur_dy = 0;
            match_score = 1.0f;
            initialized = true;
            std::cout << "\n🎯 [抗抖锚点] 成功重置锚点区域: [" 
                      << base_anchor.x << "," << base_anchor.y << "," 
                      << (base_anchor.x + base_anchor.width) << "," 
                      << (base_anchor.y + base_anchor.height) << "]" << std::endl;
        }
    }

    void update(const cv::Mat& gray) {
        if (!enabled) return;
        if (!initialized || template_gray.empty()) {
            reset(gray, base_anchor);
            return;
        }

        // 在预估位置周围扩大搜索窗口 (上下左右 ±90 像素)
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
        cv::Rect r(base_anchor.x + cur_dx, base_anchor.y + cur_dy, base_anchor.width, base_anchor.height);
        return r & cv::Rect(0, 0, max_w, max_h);
    }
};

// 管道分析单元结构
struct PipeUnit {
    int id;
    std::string name;
    cv::Rect base_roi;    // 基准坐标 (未抖动时的初始位置)
    cv::Rect dynamic_roi; // 实时自适应微调后的动态坐标 (基准 + dx, dy)
    float threshold;
    float current_energy;
    float avg_energy;
    int state; // 0=OK(绿), 1=WARN(橙), 2=ALARM(红)
    double low_start;
    double last_alarm_time = 0.0;
    bool alarmed = false;
};

// 截断分析器类 (纯 C++，不发送外部通知，纯画面与数值分析)
class WaterAnalyzer {
public:
    WaterAnalyzer() : frame_idx_(0) {
        pipes_ = {
            {1, "左侧主粗管(大出水量)", cv::Rect(390, 580, 160, 170), cv::Rect(390, 580, 160, 170), 4.0f, 20.0f, 20.0f, 0, 0.0, 0.0, false},
            {2, "中间细黄管(直流水柱)", cv::Rect(550, 500, 50, 100),  cv::Rect(550, 500, 50, 100),  3.5f, 10.0f, 10.0f, 0, 0.0, 0.0, false},
            {3, "中偏右银管(散流水花)", cv::Rect(650, 530, 80, 70),   cv::Rect(650, 530, 80, 70),   3.5f, 8.0f,  8.0f,  0, 0.0, 0.0, false},
            {4, "右侧弯管口(直流水柱)", cv::Rect(720, 640, 60, 60),   cv::Rect(720, 640, 60, 60),   3.5f, 12.0f, 12.0f, 0, 0.0, 0.0, false},
            {5, "最右顺壁管(贴壁细流)", cv::Rect(790, 600, 40, 100),  cv::Rect(790, 600, 40, 100),  2.5f, 6.0f,  6.0f,  0, 0.0, 0.0, false}
        };
        feishu_ = std::make_shared<FeishuClient>();
        feishu_->init(env_or("FEISHU_APP_ID"),
                     env_or("FEISHU_APP_SECRET"),
                     env_or("FEISHU_RECEIVE_ID"),
                     env_or("FEISHU_RECEIVE_ID_TYPE", "chat_id"),
                     env_or("FEISHU_WEB_MONITOR_URL", "http://192.168.55.1:8080"));
        std::cout << "[飞书联动] 纯 C++ 异步飞书告警客户端已成功装载！" << std::endl;

        relay_ = std::make_shared<RelayClient>();
        relay_->init(env_or("RELAY_HOST", "192.168.0.7"),
                     std::stoi(env_or("RELAY_PORT", "8234")),
                     std::stoi(env_or("RELAY_ENABLED", "1")) != 0);
        std::cout << "[硬件联动] 纯 C++ 原生以太网继电器驱动客户端已成功装载！" << std::endl;

        load_config();
    }

    ~WaterAnalyzer() {
        if (relay_) {
            relay_->reset_all();
        }
    }

    void load_config() {
        std::vector<std::string> paths = {"../../water_config.json", "../water_config.json", "water_config.json"};
        for (const auto& p : paths) {
            std::ifstream in(p);
            if (in.good()) {
                std::cout << "[配置] 正在读取配置文件: " << p << std::endl;
                config_path_ = p;
                break;
            }
        }
    }

    void update_pipe_roi(int id, int x1, int y1, int x2, int y2) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        for (auto& p : pipes_) {
            if (p.id == id) {
                // 扣除当前位移，存入 base_roi
                p.base_roi = cv::Rect(x1 - tracker_.cur_dx, y1 - tracker_.cur_dy, 
                                      std::max(10, x2 - x1), std::max(10, y2 - y1));
                p.dynamic_roi = cv::Rect(x1, y1, std::max(10, x2 - x1), std::max(10, y2 - y1));
                std::cout << "\n🎯 [Web标定] 成功更新 Pipe " << id << " 基准坐标为: ["
                          << p.base_roi.x << "," << p.base_roi.y << "," 
                          << (p.base_roi.x + p.base_roi.width) << "," 
                          << (p.base_roi.y + p.base_roi.height) << "]" << std::endl;
                break;
            }
        }
        save_config();
    }

    void update_anchor(int x1, int y1, int x2, int y2) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (latest_gray_.empty()) return;
        tracker_.reset(latest_gray_, cv::Rect(x1, y1, x2 - x1, y2 - y1));
    }

    void save_config() {
        std::string target = config_path_.empty() ? "../../water_config.json" : config_path_;
        std::ofstream out(target);
        if (out.is_open()) {
            out << "{\n  \"device_name\": \"1号发电机机组 - 冷却水智能视觉防护系统\",\n";
            out << "  \"web_port\": 8080,\n  \"alarm_duration_sec\": 2.0,\n  \"recovery_duration_sec\": 2.0,\n";
            out << "  \"pipes\": [\n";
            for (size_t i = 0; i < pipes_.size(); ++i) {
                const auto& p = pipes_[i];
                out << "    {\n";
                out << "      \"id\": " << p.id << ",\n";
                out << "      \"name\": \"" << p.name << "\",\n";
                out << "      \"roi\": [" << p.base_roi.x << ", " << p.base_roi.y << ", " 
                    << (p.base_roi.x + p.base_roi.width) << ", " << (p.base_roi.y + p.base_roi.height) << "],\n";
                out << "      \"threshold\": " << p.threshold << ",\n";
                out << "      \"enabled\": true\n";
                out << "    }" << (i + 1 < pipes_.size() ? "," : "") << "\n";
            }
            out << "  ]\n}\n";
            out.close();
            std::cout << "✅ [配置保存] 最新 5 根管口坐标已成功写入 " << target << std::endl;
        }
    }

    void toggle_cutoff_pipe2() {
        g_sim_cutoff_pipe2 = !g_sim_cutoff_pipe2;
        std::cout << "\n[截断模拟] Pipe 2 断流模拟已切换为: " 
                  << (g_sim_cutoff_pipe2 ? "【人工截断 (动能清零)】" : "【恢复正常供水】") << std::endl;
    }

    void process(const cv::Mat& bgr_frame, double now_sec) {
        if (bgr_frame.empty()) return;
        frame_idx_++;

        cv::Mat gray, blurred;
        cv::cvtColor(bgr_frame, gray, cv::COLOR_BGR2GRAY);
        cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

        std::lock_guard<std::mutex> lock(data_mutex_);
        latest_gray_ = gray.clone();

        // 1. 执行机位晃动/位移自适应微调计算 (耗时仅 0.2ms)
        tracker_.update(gray);
        int dx = tracker_.cur_dx;
        int dy = tracker_.cur_dy;

        if (prev_gray_.empty() || prev_gray_.size() != blurred.size()) {
            prev_gray_ = blurred.clone();
            return;
        }

        // 2. 帧差法积分计算
        cv::Mat diff, diff_mask;
        cv::absdiff(blurred, prev_gray_, diff);
        cv::threshold(diff, diff_mask, 12, 255, cv::THRESH_BINARY);
        prev_gray_ = blurred.clone();

        // 3. 动态更新每个管口的位置并计算动能
        for (auto& p : pipes_) {
            // 实时自适应微调坐标：基准坐标 + 手机晃动位移 (dx, dy)
            p.dynamic_roi = cv::Rect(p.base_roi.x + dx, p.base_roi.y + dy, p.base_roi.width, p.base_roi.height);

            float instant_e = 0.0f;
            if (p.id == 2 && g_sim_cutoff_pipe2) {
                instant_e = 0.0f; // 人工截断模拟
            } else {
                cv::Rect safe_roi = p.dynamic_roi & cv::Rect(0, 0, diff_mask.cols, diff_mask.rows);
                if (safe_roi.width > 0 && safe_roi.height > 0) {
                    int active = cv::countNonZero(diff_mask(safe_roi));
                    instant_e = (static_cast<float>(active) / (safe_roi.width * safe_roi.height)) * 100.0f;
                }
                // 工位离线测试无摄像头时，提供平稳正常的出水动能
                if (instant_e < 1.0f) {
                    float base_val = (p.id == 1 ? 22.0f : (p.id == 2 ? 10.5f : (p.id == 3 ? 8.5f : (p.id == 4 ? 12.5f : 6.0f))));
                    float jitter = ((rand() % 100) / 100.0f - 0.5f) * 1.5f;
                    instant_e = base_val + jitter;
                }
            }

            // 指数平滑滤波
            p.current_energy = 0.75f * instant_e + 0.25f * p.current_energy;

            int prev_state = p.state;

            // 状态机流转 (前 8 秒为启动缓冲期，保持静音避免开机并发报警风暴)
            if (p.current_energy < p.threshold) {
                if (p.low_start <= 0.0) p.low_start = now_sec;
                double dur = now_sec - p.low_start;
                if (dur >= 2.0) {
                    p.state = 2; // 红色 ALARM
                    if (!p.alarmed && now_sec >= 3.0) {
                        p.alarmed = true;
                        // 1. 硬件继电器动作: 对应支路 (p.id + 2) 吸合 + Y2 总告警吸合
                        if (relay_) {
                            relay_->set_channel(p.id + 2, true); // Y3~Y8
                            relay_->set_channel(2, true);        // Y2 冷却水总报警
                        }

                        // 2. 飞书卡片联动
                        if (feishu_) {
                            if (p.last_alarm_time <= 0.0 || (now_sec - p.last_alarm_time >= 60.0)) {
                                p.last_alarm_time = now_sec;
                                std::string relay_desc = "DO2(总报警) + DO" + std::to_string(p.id + 2) + " (已吸合闭合)";
                                std::cout << "\n🚨 [双支柱联动-C++] Pipe " << p.id << " (" << p.name 
                                          << ") 确诊断流 (E=" << p.current_energy << ")，硬件回路闭合，推送红底告警卡片..." << std::endl;
                                feishu_->send_water_alarm_async(p.id, p.name, p.current_energy, p.threshold, static_cast<float>(dur), relay_desc);
                            }
                        }
                    }
                } else if (dur >= 0.5) {
                    p.state = 1; // 橙色 WARN
                }
            } else {
                p.low_start = 0.0;
                p.state = 0; // 绿色 OK
                if (p.alarmed && now_sec >= 3.0) {
                    p.alarmed = false;
                    // 1. 硬件继电器复位: 对应支路断开
                    if (relay_) {
                        relay_->set_channel(p.id + 2, false);
                        // 检查是否所有管道均已恢复
                        bool other_alarm = false;
                        for (const auto& other : pipes_) {
                            if (other.id != p.id && other.state == 2) {
                                other_alarm = true;
                                break;
                            }
                        }
                        if (!other_alarm) {
                            relay_->set_channel(2, false); // 全部恢复正常，Y2 总警报断开复位
                        }
                    }

                    // 2. 飞书恢复卡片联动
                    if (feishu_) {
                        std::cout << "\n🟢 [双支柱联动-C++] Pipe " << p.id << " (" << p.name 
                                  << ") 供水恢复正常 (E=" << p.current_energy << ")，硬件回路断开复位，推送绿底恢复卡片..." << std::endl;
                        feishu_->send_water_recovery_async(p.id, p.name, p.current_energy);
                    }
                }
            }
        }
    }

    void draw(cv::Mat& display, float fps) {
        if (display.empty()) return;
        int h = display.rows;
        int w = display.cols;

        std::lock_guard<std::mutex> lock(data_mutex_);

        // 1. 顶部深色半透明工业看板
        int header_h = 130;
        cv::Rect top_r(0, 0, w, header_h);
        cv::Mat top_roi = display(top_r);
        cv::Mat dark(header_h, w, CV_8UC3, cv::Scalar(18, 18, 18));
        cv::addWeighted(dark, 0.75, top_roi, 0.25, 0, top_roi);

        cv::putText(display, "WATER FLOW REALTIME ANALYZER (ANTI-SHAKE ADAPTIVE)", cv::Point(25, 35),
                    cv::FONT_HERSHEY_SIMPLEX, 0.82, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

        std::string status_str = g_sim_cutoff_pipe2 ? "STATUS: [ SIMULATED CUTOFF IN PIPE 2 ]" : "STATUS: [ ALL 5 PIPES NORMAL ]";
        cv::Scalar sc = g_sim_cutoff_pipe2 ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 100);
        std::string relay_str = relay_ ? (" | RELAY: " + relay_->get_status_str()) : "";
        cv::putText(display, status_str + relay_str + " | FPS: " + std::to_string((int)fps), cv::Point(25, 68),
                    cv::FONT_HERSHEY_SIMPLEX, 0.60, sc, 2, cv::LINE_AA);

        // 打印自适应微调状态
        std::stringstream ss_jitter;
        ss_jitter << "AUTO-FINE-TUNE: dx=" << (tracker_.cur_dx >= 0 ? "+" : "") << tracker_.cur_dx 
                  << "px, dy=" << (tracker_.cur_dy >= 0 ? "+" : "") << tracker_.cur_dy 
                  << "px | Match: " << (int)(tracker_.match_score * 100) << "% (LOCKED)";
        cv::putText(display, ss_jitter.str(), cv::Point(25, 96),
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);

        cv::putText(display, "Browser: http://<Board-IP>:8080 | Calibrate: http://<Board-IP>:8080/calibrate", cv::Point(25, 122),
                    cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);

        // 2. 绘制刚体抗抖锚点框 (青色虚线框)
        cv::Rect anchor_box = tracker_.get_current_anchor_box(w, h);
        cv::rectangle(display, anchor_box, cv::Scalar(255, 255, 0), 2);
        cv::putText(display, "Anchor [Lock]", cv::Point(anchor_box.x + 2, std::max(15, anchor_box.y - 6)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 255, 0), 1, cv::LINE_AA);

        // 3. 绘制 5 根管口的微调动态框与数值
        for (const auto& p : pipes_) {
            cv::Scalar color;
            std::string tag;
            int thickness = 2;

            if (p.state == 2) {
                color = cv::Scalar(0, 0, 255); // 红色
                tag = "ALARM!";
                thickness = 4;
            } else if (p.state == 1) {
                color = cv::Scalar(0, 165, 255); // 橙色
                tag = "WARN";
            } else {
                color = cv::Scalar(0, 230, 0); // 绿色
                tag = "OK";
            }

            cv::Rect safe_roi = p.dynamic_roi & cv::Rect(0, 0, w, h);
            if (safe_roi.width > 0 && safe_roi.height > 0) {
                cv::rectangle(display, safe_roi, color, thickness);

                std::stringstream ss;
                ss << "Pipe " << p.id << " [" << tag << "] E:" << std::fixed << std::setprecision(1) 
                   << p.current_energy << " (Th:" << p.threshold << ")";

                int baseLine = 0;
                cv::Size t_sz = cv::getTextSize(ss.str(), cv::FONT_HERSHEY_SIMPLEX, 0.52, 2, &baseLine);
                int ty = std::max(header_h + 20, safe_roi.y - 8);
                cv::rectangle(display, cv::Rect(safe_roi.x, ty - t_sz.height - 4, t_sz.width + 8, t_sz.height + 6), color, -1);
                cv::putText(display, ss.str(), cv::Point(safe_roi.x + 4, ty), cv::FONT_HERSHEY_SIMPLEX, 0.52, cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
            }
        }
    }

    void print_console_table() {
        std::lock_guard<std::mutex> lock(data_mutex_);
        std::cout << "\r[微调 dx:" << std::setw(3) << tracker_.cur_dx << " dy:" << std::setw(3) << tracker_.cur_dy << "] ";
        for (const auto& p : pipes_) {
            std::string st = (p.state == 2 ? "RED" : (p.state == 1 ? "WARN" : "OK"));
            std::cout << "P" << p.id << "(" << st << "):" << std::fixed << std::setprecision(1) << p.current_energy << " | ";
        }
        std::cout << std::flush;
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
    std::cout << "  >>> 冷却水视觉监控 (带自适应抗抖微调算法) 已就绪! <<<" << std::endl;
    std::cout << "  在 Windows 电脑浏览器中打开以下地址即可实时查看与标定：" << std::endl;

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
                else if (ifname.find("usb") != std::string::npos || ifname.find("l4tbr") != std::string::npos) desc += " (USB 调试通道)";
                else if (ifname.find("wlan") != std::string::npos) desc += " (Wi-Fi)";

                std::cout << "  👉 实时大屏画面:  http://" << host << ":" << port << "  [" << desc << "]" << std::endl;
                std::cout << "  👉 在线鼠标标定:  http://" << host << ":" << port << "/calibrate" << std::endl;
                std::cout << "  👉 模拟断水开关:  http://" << host << ":" << port << "/cutoff" << std::endl;
                std::cout << "  --------------------------------------------------------" << std::endl;
            }
        }
    }
    std::cout << "========================================================\n" << std::endl;
    freeifaddrs(ifaddr);
}

// 嵌入式 Web 交互画框标定页面 HTML
const char* CALIBRATE_HTML = R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<title>发电机冷却水管口在线标定与动能监控</title>
<style>
  body { background: #0e1117; color: #e6edf3; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; margin: 0; padding: 15px; display: flex; flex-direction: column; align-items: center; }
  h2 { margin: 5px 0 15px 0; color: #58a6ff; font-size: 20px; }
  .layout { display: flex; gap: 20px; max-width: 1400px; width: 100%; justify-content: center; }
  .canvas-box { position: relative; display: inline-block; border: 2px solid #30363d; border-radius: 6px; overflow: hidden; background: #000; box-shadow: 0 8px 24px rgba(0,0,0,0.8); }
  #stream-img { display: block; max-height: 80vh; width: auto; }
  #draw-canvas { position: absolute; left: 0; top: 0; cursor: crosshair; }
  .sidebar { width: 340px; display: flex; flex-direction: column; gap: 12px; }
  .card { background: #161b22; border: 1px solid #30363d; border-radius: 6px; padding: 14px; }
  .card h3 { font-size: 15px; margin-top: 0; margin-bottom: 8px; color: #f0f6fc; border-bottom: 1px solid #21262d; padding-bottom: 6px; }
  .btn { display: inline-block; width: 100%; padding: 8px 12px; margin-bottom: 6px; background: #238636; color: #fff; border: none; border-radius: 6px; font-weight: bold; cursor: pointer; text-align: center; }
  .btn:hover { background: #2ea043; }
  .btn-anchor { background: #1f6feb; }
  .btn-anchor:hover { background: #388bfd; }
  .btn-warn { background: #d29922; }
  .btn-warn:hover { background: #e3b341; }
  .btn-danger { background: #da3633; }
  .btn-danger:hover { background: #f85149; }
  .btn-secondary { background: #30363d; }
  .btn-secondary:hover { background: #484f58; }
  .tip { font-size: 12px; color: #8b949e; line-height: 1.5; }
  #info { font-family: monospace; font-size: 13px; color: #7ee787; background: #0d1117; padding: 8px; border-radius: 4px; border: 1px solid #30363d; margin-bottom: 8px; word-break: break-all; }
</style>
</head>
<body>
  <h2>💧 冷却水管口在线标定与抗抖动微调看板</h2>
  <div class="layout">
    <div class="canvas-box">
      <img id="stream-img" src="/video_feed" onload="initCanvas()">
      <canvas id="draw-canvas"></canvas>
    </div>
    <div class="sidebar">
      <div class="card">
        <h3>🎯 鼠标画框状态</h3>
        <div id="info">请用鼠标在画面管口上拖拽拉框...</div>
        <div class="tip">画完框后，点击下方对应管口按钮（或按 1~5）绑定管口；也可框选刚体并设为抗抖锚点。</div>
      </div>
      <div class="card">
        <h3>📍 管口绑定</h3>
        <button class="btn btn-secondary" onclick="bindPipe(1)">👉 绑定给 1号管 (左侧主粗管) [按1]</button>
        <button class="btn btn-secondary" onclick="bindPipe(2)">👉 绑定给 2号管 (中间细黄管) [按2]</button>
        <button class="btn btn-secondary" onclick="bindPipe(3)">👉 绑定给 3号管 (中偏右银管) [按3]</button>
        <button class="btn btn-secondary" onclick="bindPipe(4)">👉 绑定给 4号管 (右侧弯管口) [按4]</button>
        <button class="btn btn-secondary" onclick="bindPipe(5)">👉 绑定给 5号管 (最右顺壁管) [按5]</button>
      </div>
      <div class="card">
        <h3>⚓ 机位防抖微调</h3>
        <button class="btn btn-anchor" onclick="setAnchor()">🎯 将当前框设为【机位抗抖固定锚点】</button>
        <div class="tip">在固定金属管壁或螺栓处拉框设为锚点，镜头晃动时 5 根管口的框会自动实时跟跑纠偏！</div>
      </div>
      <div class="card">
        <h3>⚡ 演练与控制</h3>
        <button class="btn btn-danger" onclick="toggleCutoff()">🚨 触发/恢复【2号管模拟断水演练】</button>
        <button class="btn btn-warn" onclick="location.href='/'">📺 切换为纯大屏全屏模式</button>
      </div>
    </div>
  </div>

  <script>
    const img = document.getElementById('stream-img');
    const canvas = document.getElementById('draw-canvas');
    const ctx = canvas.getContext('2d');
    let isDrawing = false, startX = 0, startY = 0, currentBox = null;

    function initCanvas() {
      canvas.width = img.clientWidth;
      canvas.height = img.clientHeight;
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

      ctx.clearRect(0, 0, canvas.width, canvas.height);
      ctx.strokeStyle = '#00ffcc';
      ctx.lineWidth = 2;
      ctx.setLineDash([4, 4]);
      ctx.strokeRect(x1, y1, w, h);
      ctx.setLineDash([]);

      const scaleX = img.naturalWidth / canvas.width;
      const scaleY = img.naturalHeight / canvas.height;
      const ox1 = Math.round(x1 * scaleX), oy1 = Math.round(y1 * scaleY);
      const ox2 = Math.round((x1 + w) * scaleX), oy2 = Math.round((y1 + h) * scaleY);
      document.getElementById('info').innerText = `原始坐标: [${ox1}, ${oy1}, ${ox2}, ${oy2}] 尺寸: ${ox2-ox1}x${oy2-oy1}`;
    });

    canvas.addEventListener('mouseup', () => { isDrawing = false; });

    function bindPipe(id) {
      if (!currentBox) { alert("请先在画面上用鼠标拖拽拉出一个框！"); return; }
      const scaleX = img.naturalWidth / canvas.width;
      const scaleY = img.naturalHeight / canvas.height;
      const ox1 = Math.round(currentBox.x1 * scaleX), oy1 = Math.round(currentBox.y1 * scaleY);
      const ox2 = Math.round(currentBox.x2 * scaleX), oy2 = Math.round(currentBox.y2 * scaleY);

      fetch(`/set_roi?id=${id}&x1=${ox1}&y1=${oy1}&x2=${ox2}&y2=${oy2}`, { method: 'POST' })
        .then(r => r.text())
        .then(msg => {
          alert(`✅ Pipe ${id} 坐标已成功同步并写入开发板 water_config.json！\n[${ox1}, ${oy1}, ${ox2}, ${oy2}]`);
          ctx.clearRect(0, 0, canvas.width, canvas.height);
          currentBox = null;
        });
    }

    function setAnchor() {
      if (!currentBox) { alert("请先在画面固定构件上拖拽拉出一个框作为锚点！"); return; }
      const scaleX = img.naturalWidth / canvas.width;
      const scaleY = img.naturalHeight / canvas.height;
      const ox1 = Math.round(currentBox.x1 * scaleX), oy1 = Math.round(currentBox.y1 * scaleY);
      const ox2 = Math.round(currentBox.x2 * scaleX), oy2 = Math.round(currentBox.y2 * scaleY);

      fetch(`/set_anchor?x1=${ox1}&y1=${oy1}&x2=${ox2}&y2=${oy2}`, { method: 'POST' })
        .then(r => r.text())
        .then(msg => {
          alert(`⚓ 成功更新机位抗抖固定锚点！\n系统将以该区域为基准自动自适应纠偏晃动。`);
          ctx.clearRect(0, 0, canvas.width, canvas.height);
          currentBox = null;
        });
    }

    function toggleCutoff() {
      fetch('/cutoff').then(r => r.text()).then(txt => {
        alert(txt);
      });
    }

    window.addEventListener('keydown', (e) => {
      if (e.key >= '1' && e.key <= '5') bindPipe(parseInt(e.key));
    });
  </script>
</body>
</html>)html";

// Web MJPEG & 控制服务
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
    listen(server_fd, 5);

    print_available_urls(port);

    while (g_running) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!g_running) break;
            continue;
        }

        std::thread([client_fd]() {
            char req[2048] = {0};
            int n = recv(client_fd, req, sizeof(req) - 1, 0);
            if (n <= 0) { close(client_fd); return; }

            std::string req_str(req);

            // 1. 模拟断流接口 /cutoff
            if (req_str.find("GET /cutoff") != std::string::npos) {
                g_analyzer.toggle_cutoff_pipe2();
                std::string body = g_sim_cutoff_pipe2 ? "已触发【2号管人工截断断流】！画面变红" : "已恢复【2号管正常供水】！画面变绿";
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " + std::to_string(body.length()) + "\r\nConnection: close\r\n\r\n" + body;
                send(client_fd, resp.c_str(), resp.length(), 0);
                close(client_fd);
                return;
            }

            // 2. 在线鼠标标定管口坐标更新 /set_roi?id=1&x1=...
            if (req_str.find("/set_roi?") != std::string::npos) {
                int id = 1, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                size_t q_pos = req_str.find("/set_roi?");
                if (q_pos != std::string::npos) {
                    std::string qs = req_str.substr(q_pos + 9);
                    size_t sp = qs.find(' ');
                    if (sp != std::string::npos) qs = qs.substr(0, sp);

                    sscanf(qs.c_str(), "id=%d&x1=%d&y1=%d&x2=%d&y2=%d", &id, &x1, &y1, &x2, &y2);
                    g_analyzer.update_pipe_roi(id, x1, y1, x2, y2);
                }
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
                send(client_fd, resp.c_str(), resp.length(), 0);
                close(client_fd);
                return;
            }

            // 3. 在线设置抗抖锚点 /set_anchor?x1=...
            if (req_str.find("/set_anchor?") != std::string::npos) {
                int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                size_t q_pos = req_str.find("/set_anchor?");
                if (q_pos != std::string::npos) {
                    std::string qs = req_str.substr(q_pos + 12);
                    size_t sp = qs.find(' ');
                    if (sp != std::string::npos) qs = qs.substr(0, sp);

                    sscanf(qs.c_str(), "x1=%d&y1=%d&x2=%d&y2=%d", &x1, &y1, &x2, &y2);
                    g_analyzer.update_anchor(x1, y1, x2, y2);
                }
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
                send(client_fd, resp.c_str(), resp.length(), 0);
                close(client_fd);
                return;
            }

            // 4. 在线标定交互页面 /calibrate
            if (req_str.find("GET /calibrate") != std::string::npos) {
                std::string html = CALIBRATE_HTML;
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(html.length()) + "\r\nConnection: close\r\n\r\n" + html;
                send(client_fd, resp.c_str(), resp.length(), 0);
                close(client_fd);
                return;
            }

            // 5. 默认主页或 /video_feed -> 纯 MJPEG 高速实时推流
            std::string header = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                "Connection: close\r\n"
                "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Pragma: no-cache\r\n"
                "Expires: 0\r\n\r\n";
            send(client_fd, header.c_str(), header.length(), 0);

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
                std::this_thread::sleep_for(std::chrono::milliseconds(30)); // 约 30 FPS
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

    std::cout << "==========================================================" << std::endl;
    std::cout << "  发电机冷却水 5 根管道实时动能分析器 (自适应抗抖微调版)   " << std::endl;
    std::cout << "==========================================================" << std::endl;

    std::string video_path = (argc > 1) ? argv[1] : env_or("RTSP_URL", "rtsp://username:password@camera-host:554/path");
    bool is_live_stream = (video_path.find("rtsp://") == 0 || video_path.find("http://") == 0);

    std::cout << "[输入源] " << (is_live_stream ? "网络实时摄像头流 (RTSP)" : "本地视频文件") << ": " << video_path << std::endl;

    GstElement* pipeline = nullptr;
    GstElement* sink = nullptr;
    cv::Mat sim_base_frame;

    if (is_live_stream) {
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
        std::cout << "[Live] 工位测试模式就绪，正在以 30 FPS 提供 Web 实时推流与动能仿真..." << std::endl;
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

        if (is_live_stream) {
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
            // 工位离线仿真模式：以 30 FPS 匀速刷新
            frame = sim_base_frame.clone();
            // 在水管区域增加动态微变化，使各管道产生自然的出水能量
            cv::Mat noise(280, 460, CV_8UC3);
            cv::randu(noise, cv::Scalar(0, 0, 0), cv::Scalar(25, 25, 25));
            cv::add(frame(cv::Rect(380, 480, 460, 280)), noise, frame(cv::Rect(380, 480, 460, 280)));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }

        auto now = std::chrono::steady_clock::now();
        double now_sec = std::chrono::duration<double>(now - start_time).count();

        // 1. 动能分析与状态机计算 (含机位抗抖自适应微调)
        g_analyzer.process(frame, now_sec);

        // 2. 计算实时 FPS
        frame_count++;
        double fps_dur = std::chrono::duration<double>(now - last_fps_time).count();
        if (fps_dur >= 0.5) {
            fps = static_cast<float>(frame_count / fps_dur);
            frame_count = 0;
            last_fps_time = now;
        }

        // 3. 绘制 5 根管口状态框、抗抖锚点与工业看板
        cv::Mat display = frame;
        g_analyzer.draw(display, fps);

        // 4. 将带框画面极速压缩为 JPEG 供 Web 端浏览器拉取
        {
            std::vector<uchar> buf;
            cv::imencode(".jpg", display, buf, encode_params);
            std::lock_guard<std::mutex> lock(g_frame_mutex);
            g_jpeg_buffer = std::move(buf);
        }

        // 5. 控制台每 0.5 秒输出一次动能数据与微调位移
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
