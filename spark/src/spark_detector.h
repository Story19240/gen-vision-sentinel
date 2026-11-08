#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace spark {

class JsonValue;

struct DetectionResult {
    std::int64_t timestamp_ms = 0;
    double rel_saturation = 0.0;
    double contact_sat_ratio = 0.0;
    double ref_sat_ratio = 0.0;
    double rel_flicker = 0.0;
    double contact_flicker = 0.0;
    double ref_flicker = 0.0;
    double color_match_ratio = 0.0;
    bool color_hit = false;
    std::string color_type = "none";
    int falling_bright_pixels = 0;
    int falling_components = 0;
    bool candidate = false;
    bool event_triggered = false;
    std::string level = "none";
    double contact_v_mean = 0.0;
    double reference_v_mean = 0.0;
    std::int64_t candidate_duration_ms = 0;
};

struct Roi {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;

    int width() const noexcept { return x2 - x1; }
    int height() const noexcept { return y2 - y1; }
    std::size_t area() const noexcept {
        return x2 > x1 && y2 > y1 ? static_cast<std::size_t>((x2 - x1) * (y2 - y1)) : 0;
    }
};

class SparkError : public std::runtime_error {
public:
    explicit SparkError(const std::string& message) : std::runtime_error(message) {}
};

struct ResolvedRoi {
    bool valid = false;
    Roi roi;
};

ResolvedRoi select_roi_profile(
    const JsonValue* roi_config,
    int frame_width,
    int frame_height);
ResolvedRoi clip_roi(
    const ResolvedRoi& roi,
    int frame_width,
    int frame_height);
ResolvedRoi resolve_roi(
    const JsonValue* roi_config,
    int frame_width,
    int frame_height);

class SparkDetector {
public:
    SparkDetector(const JsonValue& config, double fps);

    void reset();
    DetectionResult process_frame(
        const std::uint8_t* bgr,
        int width,
        int height,
        int channels,
        std::int64_t timestamp_ms);

    double fps() const noexcept { return fps_; }

private:
    std::shared_ptr<JsonValue> config_;
    double fps_;
    int v_sat_threshold_;
    bool use_relative_;
    double rel_saturation_threshold_;
    double flicker_threshold_;
    double color_match_threshold_;
    int heavy_bright_pixels_;
    int heavy_components_;
    std::int64_t confirm_duration_ms_;
    std::size_t window_frames_;
    std::deque<double> contact_v_history_;
    std::deque<double> reference_v_history_;
    bool candidate_start_valid_ = false;
    std::int64_t candidate_start_ms_ = 0;
    bool candidate_latched_ = false;
    bool last_timestamp_valid_ = false;
    std::int64_t last_timestamp_ms_ = 0;
    std::int64_t frame_index_ = 0;
};

}  // namespace spark
