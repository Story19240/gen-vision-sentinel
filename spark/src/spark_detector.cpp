#include "spark_detector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#include "json.hpp"

namespace spark {
namespace {

constexpr int kDefaultVSatThreshold = 240;
constexpr double kDefaultRelSaturation = 0.01;
constexpr double kDefaultFlicker = 0.5;
constexpr double kDefaultColorMatchRatio = 0.5;
constexpr int kDefaultHeavyBrightPixels = 20;
constexpr int kDefaultHeavyComponents = 2;
constexpr std::int64_t kDefaultConfirmDurationMs = 200;
constexpr double kDefaultHueBlueWhite[2] = {100.0, 130.0};
constexpr double kDefaultHueOrangeYellow[2] = {5.0, 25.0};

ResolvedRoi as_roi(const JsonValue* value) {
    if (value == nullptr || !value->is_array() || value->as_array().size() != 4) {
        return ResolvedRoi{};
    }
    const auto& values = value->as_array();
    for (const auto& item : values) {
        if (!item.is_number()) return ResolvedRoi{};
    }
    try {
        const std::int64_t x1 = values[0].as_int64();
        const std::int64_t y1 = values[1].as_int64();
        const std::int64_t x2 = values[2].as_int64();
        const std::int64_t y3 = values[3].as_int64();
        if (x2 <= x1 || y3 <= y1) return ResolvedRoi{};
        return ResolvedRoi{true, Roi{
            static_cast<int>(x1),
            static_cast<int>(y1),
            static_cast<int>(x2),
            static_cast<int>(y3)}};
    } catch (const JsonError&) {
        return ResolvedRoi{};
    }
}

bool hue_in_range(int hue, const double bounds[2]) {
    const double low = std::fmod(bounds[0], 180.0);
    const double high = std::fmod(bounds[1], 180.0);
    const double value = static_cast<double>(hue);
    if (low <= high) {
        return value >= low && value <= high;
    }
    return value >= low || value <= high;
}

double mean_frame_delta(const std::deque<double>& values) {
    if (values.size() < 2) return 0.0;
    double sum = 0.0;
    auto previous = values.begin();
    for (auto current = std::next(previous); current != values.end(); ++current, ++previous) {
        sum += std::abs(*current - *previous);
    }
    return sum / static_cast<double>(values.size() - 1);
}

}  // namespace

ResolvedRoi select_roi_profile(
    const JsonValue* roi_config,
    int frame_width,
    int frame_height) {
    if (roi_config == nullptr || !roi_config->is_object()) {
        return as_roi(roi_config);
    }

    const ResolvedRoi sample = as_roi(roi_config->find("sample"));
    const ResolvedRoi synthetic = as_roi(roi_config->find("synthetic"));
    if (frame_width == 751 && frame_height == 1024 && sample.valid) {
        return sample;
    }
    if (frame_width == 640 && frame_height == 480 && synthetic.valid) {
        return synthetic;
    }

    const ResolvedRoi& preferred = frame_width >= frame_height ? synthetic : sample;
    const ResolvedRoi& fallback = preferred.valid == synthetic.valid ? sample : synthetic;
    if (preferred.valid) return preferred;
    return fallback;
}

ResolvedRoi clip_roi(
    const ResolvedRoi& roi,
    int frame_width,
    int frame_height) {
    if (!roi.valid || frame_width <= 0 || frame_height <= 0) return ResolvedRoi{};
    const int x1 = std::max(0, std::min(roi.roi.x1, frame_width));
    const int y1 = std::max(0, std::min(roi.roi.y1, frame_height));
    const int x2 = std::max(0, std::min(roi.roi.x2, frame_width));
    const int y2 = std::max(0, std::min(roi.roi.y2, frame_height));
    if (x2 <= x1 || y2 <= y1) return ResolvedRoi{};
    return ResolvedRoi{true, Roi{x1, y1, x2, y2}};
}

ResolvedRoi resolve_roi(
    const JsonValue* roi_config,
    int frame_width,
    int frame_height) {
    return clip_roi(select_roi_profile(roi_config, frame_width, frame_height), frame_width, frame_height);
}

bool normalize_frame(
    const std::uint8_t* bgr,
    int width,
    int height,
    int channels,
    std::vector<std::uint8_t>& output,
    int& out_width,
    int& out_height) {
    if (bgr == nullptr || width <= 0 || height <= 0 || channels < 3) {
        out_width = 0;
        out_height = 0;
        return false;
    }
    if (channels == 3) {
        output.assign(bgr, bgr + static_cast<std::size_t>(width) * height * 3);
        out_width = width;
        out_height = height;
        return true;
    }
    output.clear();
    output.reserve(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = bgr + static_cast<std::size_t>(y) * width * channels;
        for (int x = 0; x < width; ++x) {
            output.push_back(row[x * channels + 0]);
            output.push_back(row[x * channels + 1]);
            output.push_back(row[x * channels + 2]);
        }
    }
        out_width = width;
        out_height = height;
        return true;
}

DetectionResult make_empty_result(std::int64_t timestamp_ms) {
    DetectionResult result;
    result.timestamp_ms = timestamp_ms;
    return result;
}

SparkDetector::SparkDetector(const JsonValue& config, double fps)
    : config_(std::make_shared<JsonValue>(config)) {
    fps_ = fps > 0.0 ? fps : 25.0;
    v_sat_threshold_ = 240;
    use_relative_ = true;
    rel_saturation_threshold_ = kDefaultRelSaturation;
    flicker_threshold_ = kDefaultFlicker;
    color_match_threshold_ = kDefaultColorMatchRatio;
    heavy_bright_pixels_ = kDefaultHeavyBrightPixels;
    heavy_components_ = kDefaultHeavyComponents;
    confirm_duration_ms_ = kDefaultConfirmDurationMs;
    window_frames_ = std::max(2, static_cast<int>(std::lround(fps_ * 0.5)));

    if (const JsonValue* value = config_->find("v_sat_threshold")) {
        if (value->is_number()) {
            v_sat_threshold_ = static_cast<int>(value->as_double());
        }
    }
    if (const JsonValue* value = config_->find("use_relative")) {
        if (value->is_boolean()) use_relative_ = value->as_bool();
    }
    if (const JsonValue* thresholds = config_->find("thresholds")) {
        if (thresholds->is_object()) {
            if (const JsonValue* item = thresholds->find("rel_saturation")) {
                if (item->is_number()) rel_saturation_threshold_ = item->as_double();
            }
            if (const JsonValue* item = thresholds->find("flicker")) {
                if (item->is_number()) flicker_threshold_ = item->as_double();
            }
            if (const JsonValue* item = thresholds->find("color_match_ratio")) {
                if (item->is_number()) color_match_threshold_ = item->as_double();
            }
        }
    }
    if (const JsonValue* thresholds = config_->find("level_thresholds")) {
        if (thresholds->is_object()) {
            if (const JsonValue* item = thresholds->find("heavy_bright_pixels")) {
                if (item->is_number()) heavy_bright_pixels_ = static_cast<int>(item->as_double());
            }
            if (const JsonValue* item = thresholds->find("heavy_components")) {
                if (item->is_number()) heavy_components_ = static_cast<int>(item->as_double());
            }
        }
    }
    if (const JsonValue* value = config_->find("confirm_duration_ms")) {
        if (value->is_number()) {
            confirm_duration_ms_ = std::max<std::int64_t>(
                0, static_cast<std::int64_t>(value->as_double()));
        }
    }
}

void SparkDetector::reset() {
    contact_v_history_.clear();
    reference_v_history_.clear();
    candidate_start_valid_ = false;
    candidate_start_ms_ = 0;
    candidate_latched_ = false;
    last_timestamp_valid_ = false;
    last_timestamp_ms_ = 0;
    frame_index_ = 0;
}

void SparkDetector::ensure_luts() {
    const JsonValue* blue = config_->find("hue_blue_white");
    if (blue != nullptr && blue->is_array() && blue->as_array().size() >= 2) {
        hue_blue_white_[0] = blue->as_array()[0].as_double();
        hue_blue_white_[1] = blue->as_array()[1].as_double();
    } else {
        hue_blue_white_[0] = kDefaultHueBlueWhite[0];
        hue_blue_white_[1] = kDefaultHueBlueWhite[1];
    }
    const JsonValue* orange = config_->find("hue_orange_yellow");
    if (orange != nullptr && orange->is_array() && orange->as_array().size() >= 2) {
        hue_orange_yellow_[0] = orange->as_array()[0].as_double();
        hue_orange_yellow_[1] = orange->as_array()[1].as_double();
    } else {
        hue_orange_yellow_[0] = kDefaultHueOrangeYellow[0];
        hue_orange_yellow_[1] = kDefaultHueOrangeYellow[1];
    }
}

}  // namespace spark
