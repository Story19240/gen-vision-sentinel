#ifndef WATER_FLOW_DETECTOR_H
#define WATER_FLOW_DETECTOR_H

#include <string>
#include <vector>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include "feishu_client.h"

// 管道水流状态枚举
enum PipeFlowState {
    STATE_NORMAL = 0,   // 正常流水 (绿色框)
    STATE_WARNING = 1,  // 疑似气阻/短时波动 (橙色框, <2秒)
    STATE_ALARM = 2     // 确认断水 (红色高亮框, >=2秒, 触发跳闸与推送)
};

// 单根管道的物理配置与运行态数据结构
struct PipeMonitorUnit {
    int id;
    std::string name;
    cv::Rect roi;              // 管口监测窗口 (x, y, w, h)
    float threshold;           // 断流临界能量阈值
    int relay_channel;         // 对应继电器通道 (3~8)
    bool enabled;

    // 运行动态参数
    float current_energy;      // 当前瞬时动能 (0.0 ~ 100.0)
    PipeFlowState state;       // 当前状态
    double low_energy_start_time; // 动能跌破阈值的起始时间戳 (秒)
    double normal_start_time;     // 动能恢复正常的起始时间戳 (秒)
    double last_alarm_time;       // 上次推送报警的时间戳 (防重发冷却)

    // 模拟测试注入控制
    bool is_simulated_cutoff;  // 是否被人工注入模拟断水
};

class WaterFlowDetector {
public:
    WaterFlowDetector();
    ~WaterFlowDetector();

    // 加载配置文件 (water_config.json)
    bool init(const std::string& config_path);

    // 注入飞书客户端
    void set_feishu_client(std::shared_ptr<FeishuClient> feishu);

    // 处理单帧画面 (执行微秒级帧差动能积分与三级状态机流转)
    void process_frame(const cv::Mat& bgr_frame, double current_time_sec);

    // 在画面上绘制 5 根管口的彩色状态框与顶部工业 OSD 看板
    void draw_overlay(cv::Mat& display_frame, float fps = 0.0f);

    // 模拟注入：强制切断/恢复某根管道的水流 (用于离线完整验证断水报警)
    void simulate_pipe_cutoff(int pipe_id, bool cutoff);

    // 获取当前管道状态列表
    const std::vector<PipeMonitorUnit>& get_pipes() const { return pipes_; }

    // 获取是否有任意管道处于紧急报警状态
    bool has_any_alarm() const;

private:
    std::string device_name_;
    float alarm_duration_sec_;      // 断水持续确认时间 (默认 2.0s)
    float recovery_duration_sec_;   // 供水恢复确认时间 (默认 2.0s)
    float cooldown_sec_;            // 重复告警冷却周期 (默认 300s)

    std::vector<PipeMonitorUnit> pipes_;
    cv::Mat prev_gray_frame_;       // 前一帧灰度图
    std::shared_ptr<FeishuClient> feishu_;

    // 计算指定 ROI 内部的运动像素动能密度 (耗时 < 0.05ms)
    float calculate_roi_energy(const cv::Mat& diff_mask, const cv::Rect& roi);

    // 触发单管断水联动
    void trigger_pipe_alarm(PipeMonitorUnit& pipe, double current_time);

    // 触发单管恢复正常联动
    void trigger_pipe_recovery(PipeMonitorUnit& pipe, double current_time);
};

#endif // WATER_FLOW_DETECTOR_H
