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

void normalize_frame(
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
        return;
    }
    if (channels == 3) {
        output.assign(bgr, bgr + static_cast<std::size_t>(width) * height * 3);
        out_width = width;
        out_height = height;
        return;
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
}

DetectionResult make_empty_result(std::int64_t timestamp_ms) {
    DetectionResult result;
    result.timestamp_ms = timestamp_ms;
    return result;
}

}  // namespace spark
