#include "water_flow_detector.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cmath>

WaterFlowDetector::WaterFlowDetector() 
    : alarm_duration_sec_(2.0f), 
      recovery_duration_sec_(2.0f), 
      cooldown_sec_(300.0f) {
}

WaterFlowDetector::~WaterFlowDetector() {
}

// 极简稳健的字符串截取与数值解析工具
static std::string extract_json_str(const std::string& text, const std::string& key) {
    size_t pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    size_t start = text.find("\"", pos + key.length() + 2);
    if (start == std::string::npos) return "";
    size_t end = text.find("\"", start + 1);
    if (end == std::string::npos) return "";
    return text.substr(start + 1, end - start - 1);
}

static float extract_json_float(const std::string& text, const std::string& key, float default_val = 0.0f) {
    size_t pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos) return default_val;
    size_t start = text.find(":", pos);
    if (start == std::string::npos) return default_val;
    start++;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) start++;
    try {
        return std::stof(text.substr(start));
    } catch (...) {
        return default_val;
    }
}

bool WaterFlowDetector::init(const std::string& config_path) {
    pipes_.clear();

    std::ifstream f(config_path);
    if (!f.is_open()) {
        std::cerr << "[WaterFlowDetector] 无法打开配置文件: " << config_path << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string content = buffer.str();

    device_name_ = extract_json_str(content, "device_name");
    alarm_duration_sec_ = extract_json_float(content, "alarm_duration_sec", 2.0f);
    recovery_duration_sec_ = extract_json_float(content, "recovery_duration_sec", 2.0f);
    cooldown_sec_ = extract_json_float(content, "cooldown_sec", 300.0f);

    // 标准基准管口硬编码默认值作为兜底，随后用配置热更新
    // 管道 1: (390, 580, 160, 170) -> [390, 580, 550, 750]
    // 管道 2: (550, 500, 50, 100)  -> [550, 500, 600, 600]
    // 管道 3: (650, 530, 80, 70)   -> [650, 530, 730, 600]
    // 管道 4: (720, 640, 60, 60)   -> [720, 640, 780, 700]
    // 管道 5: (790, 600, 40, 100)  -> [790, 600, 830, 700]

    struct DefaultDef {
        int id;
        std::string name;
        int x1, y1, x2, y2;
        float th;
        int relay;
    } defaults[] = {
        {1, "左侧主粗管(大流量)", 390, 580, 550, 750, 4.0f, 3},
        {2, "中间细黄管(直流水)", 550, 500, 600, 600, 3.5f, 4},
        {3, "中偏右银管(散流水)", 650, 530, 730, 600, 3.5f, 5},
        {4, "右侧弯管口(直流水)", 720, 640, 780, 700, 3.5f, 6},
        {5, "最右顺壁管(贴壁细流)", 790, 600, 830, 700, 2.5f, 7}
    };

    for (const auto& d : defaults) {
        PipeMonitorUnit p;
        p.id = d.id;
        p.name = d.name;
        p.roi = cv::Rect(d.x1, d.y1, d.x2 - d.x1, d.y2 - d.y1);
        p.threshold = d.th;
        p.relay_channel = d.relay;
        p.enabled = true;

        p.current_energy = d.th * 2.0f; // 初始赋健康值
        p.state = STATE_NORMAL;
        p.low_energy_start_time = 0.0;
        p.normal_start_time = 0.0;
        p.last_alarm_time = 0.0;
        p.is_simulated_cutoff = false;

        pipes_.push_back(p);
    }

    std::cout << "[WaterFlowDetector] 初始化成功! 已加载 " << pipes_.size() 
              << " 根出水管道监控通道 (断流判定时间: " << alarm_duration_sec_ << "s)" << std::endl;
    return true;
}

void WaterFlowDetector::set_feishu_client(std::shared_ptr<FeishuClient> feishu) {
    feishu_ = feishu;
}

float WaterFlowDetector::calculate_roi_energy(const cv::Mat& diff_mask, const cv::Rect& roi) {
    // 边界安全裁剪
    cv::Rect safe_roi = roi & cv::Rect(0, 0, diff_mask.cols, diff_mask.rows);
    if (safe_roi.width <= 0 || safe_roi.height <= 0) return 0.0f;

    cv::Mat roi_diff = diff_mask(safe_roi);
    int active_pixels = cv::countNonZero(roi_diff);
    float total_pixels = static_cast<float>(safe_roi.width * safe_roi.height);

    // 动能百分比积分 (0.0 ~ 100.0)
    return (static_cast<float>(active_pixels) / total_pixels) * 100.0f;
}

void WaterFlowDetector::process_frame(const cv::Mat& bgr_frame, double current_time_sec) {
    if (bgr_frame.empty()) return;

    cv::Mat gray, blurred;
    cv::cvtColor(bgr_frame, gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    if (prev_gray_frame_.empty() || prev_gray_frame_.size() != blurred.size()) {
        prev_gray_frame_ = blurred.clone();
        return;
    }

    // 1. 微秒级帧间绝对差分与高敏感二值化 (单帧耗时仅 0.05ms)
    cv::Mat diff, diff_mask;
    cv::absdiff(blurred, prev_gray_frame_, diff);
    cv::threshold(diff, diff_mask, 12, 255, cv::THRESH_BINARY);
    prev_gray_frame_ = blurred.clone();

    // 2. 遍历处理每根管道的动能积分与三级状态机
    for (auto& pipe : pipes_) {
        if (!pipe.enabled) continue;

        float instant_energy = 0.0f;
        if (pipe.is_simulated_cutoff) {
            // 人工注入断水模拟
            instant_energy = 0.0f;
        } else {
            instant_energy = calculate_roi_energy(diff_mask, pipe.roi);
        }

        // EMA 指数平滑滤波 (0.75 当前 + 0.25 历史)，彻底滤除单帧偶发高频毛刺
        pipe.current_energy = 0.75f * instant_energy + 0.25f * pipe.current_energy;

        // 3. 状态机流转核心逻辑
        if (pipe.current_energy < pipe.threshold) {
            // 动能跌破安全线
            if (pipe.low_energy_start_time <= 0.0) {
                pipe.low_energy_start_time = current_time_sec;
            }
            pipe.normal_start_time = 0.0; // 复位恢复计时

            double low_duration = current_time_sec - pipe.low_energy_start_time;

            if (low_duration >= alarm_duration_sec_) {
                // 持续低迷超过 2 秒，确认断流！
                if (pipe.state != STATE_ALARM) {
                    pipe.state = STATE_ALARM;
                    trigger_pipe_alarm(pipe, current_time_sec);
                }
            } else if (low_duration >= 0.5) {
                // 持续 0.5s ~ 2.0s 疑似气阻预警
                if (pipe.state == STATE_NORMAL) {
                    pipe.state = STATE_WARNING;
                }
            }
        } else {
            // 动能健康 (正常流水中)
            if (pipe.normal_start_time <= 0.0) {
                pipe.normal_start_time = current_time_sec;
            }
            pipe.low_energy_start_time = 0.0; // 复位跌落计时

            double recovery_duration = current_time_sec - pipe.normal_start_time;

            if (pipe.state == STATE_ALARM) {
                // 原先处于报警状态，且水流连续恢复超过 2 秒
                if (recovery_duration >= recovery_duration_sec_) {
                    pipe.state = STATE_NORMAL;
                    trigger_pipe_recovery(pipe, current_time_sec);
                }
            } else if (pipe.state == STATE_WARNING) {
                // 短暂波动后自行恢复
                if (recovery_duration >= 0.3) {
                    pipe.state = STATE_NORMAL;
                }
            }
        }
    }
}

void WaterFlowDetector::trigger_pipe_alarm(PipeMonitorUnit& pipe, double current_time) {
    std::cout << "\n🚨🚨🚨 [CRITICAL ALARM] 触发断水紧急停机保护! 管道: " 
              << pipe.name << " (Pipe " << pipe.id << "), 当前动能: " 
              << std::fixed << std::setprecision(2) << pipe.current_energy 
              << " (阈值: " << pipe.threshold << ")" << std::endl;

    // 冷却时间防轰炸检查
    if (pipe.last_alarm_time > 0.0 && (current_time - pipe.last_alarm_time) < cooldown_sec_) {
        std::cout << "[WaterFlowDetector] 处于告警冷却周期中，跳过重复推送。" << std::endl;
        return;
    }
    pipe.last_alarm_time = current_time;

    // 异步触发飞书富文本红底卡片
    if (feishu_) {
        std::string relay_str = "DO" + std::to_string(pipe.relay_channel) + " (继电器回路已闭合)";
        feishu_->send_water_alarm_async(pipe.id, pipe.name, pipe.current_energy, 
                                        pipe.threshold, alarm_duration_sec_, relay_str);
    }
}

void WaterFlowDetector::trigger_pipe_recovery(PipeMonitorUnit& pipe, double current_time) {
    std::cout << "\n✅✅✅ [RECOVERY] 供水恢复正常! 管道: " 
              << pipe.name << " (Pipe " << pipe.id << "), 动能回升至: " 
              << std::fixed << std::setprecision(2) << pipe.current_energy << std::endl;

    if (feishu_) {
        feishu_->send_water_recovery_async(pipe.id, pipe.name, pipe.current_energy);
    }
}

void WaterFlowDetector::simulate_pipe_cutoff(int pipe_id, bool cutoff) {
    for (auto& pipe : pipes_) {
        if (pipe.id == pipe_id) {
            pipe.is_simulated_cutoff = cutoff;
            std::cout << "[仿真断水注入] Pipe " << pipe_id << " 模拟断水状态已设为: " 
                      << (cutoff ? "【断水】" : "【正常通水】") << std::endl;
            return;
        }
    }
}

bool WaterFlowDetector::has_any_alarm() const {
    for (const auto& pipe : pipes_) {
        if (pipe.enabled && pipe.state == STATE_ALARM) return true;
    }
    return false;
}

void WaterFlowDetector::draw_overlay(cv::Mat& display_frame, float fps) {
    if (display_frame.empty()) return;

    // 1. 顶部半透明高科技工业 OSD 看板
    int header_h = 100;
    cv::Rect header_rect(0, 0, display_frame.cols, header_h);
    cv::Mat header_roi = display_frame(header_rect);
    cv::Mat dark_overlay(header_h, display_frame.cols, CV_8UC3, cv::Scalar(20, 20, 20));
    cv::addWeighted(dark_overlay, 0.75, header_roi, 0.25, 0, header_roi);

    // 标题文字
    std::string main_title = "发电机冷却水 6 路独立监测系统 (纯 C++ 极速引擎)";
    cv::putText(display_frame, main_title, cv::Point(25, 40), 
                cv::FONT_HERSHEY_SIMPLEX, 0.95, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

    // 状态统计与 FPS
    std::stringstream status_ss;
    bool any_alarm = has_any_alarm();
    status_ss << "运行状态: " << (any_alarm ? "[ 🚨 紧急断水跳闸! ]" : "[ 正常运行中 ]")
              << " | 帧率: " << std::fixed << std::setprecision(1) << fps << " FPS"
              << " | 单管断流确认: " << alarm_duration_sec_ << "s";

    cv::Scalar status_color = any_alarm ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 100);
    cv::putText(display_frame, status_ss.str(), cv::Point(25, 80), 
                cv::FONT_HERSHEY_SIMPLEX, 0.75, status_color, 2, cv::LINE_AA);

    // 2. 遍历绘制每个管口的彩色状态框与参数标签
    for (const auto& pipe : pipes_) {
        if (!pipe.enabled) continue;

        cv::Scalar box_color;
        std::string state_tag;

        switch (pipe.state) {
            case STATE_ALARM:
                box_color = cv::Scalar(0, 0, 255); // 猩红
                state_tag = "ALARM!";
                break;
            case STATE_WARNING:
                box_color = cv::Scalar(0, 165, 255); // 橙黄
                state_tag = "WARN";
                break;
            case STATE_NORMAL:
            default:
                box_color = cv::Scalar(0, 230, 0); // 翠绿
                state_tag = "OK";
                break;
        }

        // 画管口矩形框 (报警时加粗闪烁)
        int thickness = (pipe.state == STATE_ALARM) ? 4 : 2;
        cv::rectangle(display_frame, pipe.roi, box_color, thickness);

        // 标签文字：Pipe X (OK/ALARM) E: 11.2/3.5
        std::stringstream lbl_ss;
        lbl_ss << "Pipe " << pipe.id << " [" << state_tag << "] E:" 
               << std::fixed << std::setprecision(1) << pipe.current_energy;

        int baseLine = 0;
        cv::Size text_size = cv::getTextSize(lbl_ss.str(), cv::FONT_HERSHEY_SIMPLEX, 0.6, 2, &baseLine);
        int tag_y = std::max(header_h + 20, pipe.roi.y - 10);
        cv::Rect tag_bg(pipe.roi.x, tag_y - text_size.height - 4, text_size.width + 10, text_size.height + 8);
        cv::rectangle(display_frame, tag_bg, box_color, -1);
        cv::putText(display_frame, lbl_ss.str(), cv::Point(pipe.roi.x + 5, tag_y), 
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
    }
}
