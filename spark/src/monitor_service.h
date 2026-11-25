#pragma once

#include "json.hpp"
#include "spark_detector.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

class FeishuClient;
class RelayClient;

namespace spark {

struct MonitorOptions {
    std::string config_path = "spark/spark_config.json";
    std::string recordings_dir = "spark/recordings";
    int web_port = 8081;
};

class MonitorService {
public:
    explicit MonitorService(MonitorOptions options);
    ~MonitorService();

    void initialize();
    void shutdown();

    DetectionResult process_video_frame(
        const cv::Mat& frame,
        const std::vector<uchar>& jpeg,
        std::int64_t timestamp_ms,
        double detector_fps,
        double measured_fps);
    void finish_source();

    std::string handle_api(const std::string& path, const std::string& query);
    std::string status_json() const;
    std::string recordings_dir() const;
    int web_port() const;

private:
    struct JpegFrame {
        std::int64_t timestamp_ms = 0;
        std::vector<uchar> bytes;
    };

    struct RecordingJob {
        std::string base_name;
        double fps = 25.0;
        std::int64_t post_end_ms = 0;
        std::vector<JpegFrame> frames;
    };

    struct EventInfo {
        std::string base_name;
        std::string clip_name;
        std::string time_text;
        std::string level;
        std::string color_type;
        std::int64_t timestamp_ms = 0;
        bool clip_ready = false;
    };

    MonitorOptions options_;
    mutable std::mutex mutex_;
    JsonValue config_;
    std::string config_path_;
    std::unique_ptr<SparkDetector> detector_;
    std::uint64_t detector_revision_ = 0;
    bool monitoring_ = false;
    bool source_online_ = false;
    bool resolution_dirty_ = true;
    int source_width_ = 0;
    int source_height_ = 0;
    double measured_fps_ = 0.0;
    DetectionResult latest_result_;
    std::deque<JpegFrame> recent_frames_;
    std::unique_ptr<RecordingJob> active_recording_;
    std::vector<std::thread> recording_threads_;
    std::vector<EventInfo> events_;
    std::string last_recording_;
    std::string recording_error_;
    std::int64_t last_notification_ms_ = -1;
    bool feishu_enabled_ = false;
    std::string feishu_web_url_;
    std::shared_ptr<FeishuClient> feishu_;
    bool relay_trip_enabled_ = false;
    int relay_channel_ = 1;
    bool relay_energized_ = false;
    std::shared_ptr<RelayClient> relay_;

    void load_initial_config();
    void load_config_file(const std::string& path);
    void save_config_locked() const;
    void configure_feishu_locked();
    void configure_relay_locked();
    void set_relay_energized_locked(bool energized);
    void ensure_source_shape_locked(int width, int height);
    void rebuild_detector_locked(double fps);
    void clear_rois_locked(int width, int height);
    void update_recent_locked(const JpegFrame& frame);
    void handle_event_locked(
        const DetectionResult& result,
        const cv::Mat& frame,
        double fps);
    void append_post_frame_locked(const JpegFrame& frame);
    void finalize_recording_locked();
    void recording_worker(RecordingJob job);
    void send_spark_notification_locked(const EventInfo& event, const DetectionResult& result);

    static std::string api_ok(const std::string& message = "ok");
    static std::string api_error(const std::string& message);
};

}  // namespace spark
