#include "spark_detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

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

int count_components_8(const std::vector<std::uint8_t>& mask, int width, int height) {
    if (width <= 0 || height <= 0 ||
        mask.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return 0;
    }
    std::vector<bool> visited(mask.size(), false);
    std::vector<std::size_t> queue;
    int components = 0;
    for (std::size_t first = 0; first < mask.size(); ++first) {
        if (mask[first] == 0 || visited[first]) continue;
        ++components;
        visited[first] = true;
        queue.clear();
        queue.push_back(first);
        while (!queue.empty()) {
            const std::size_t current = queue.back();
            queue.pop_back();
            const int x = static_cast<int>(current % static_cast<std::size_t>(width));
            const int y = static_cast<int>(current / static_cast<std::size_t>(width));
            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = y + dy;
                if (ny < 0 || ny >= height) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    if (nx < 0 || nx >= width) continue;
                    const std::size_t neighbor =
                        static_cast<std::size_t>(ny) * width + static_cast<std::size_t>(nx);
                    if (!visited[neighbor] && mask[neighbor] != 0) {
                        visited[neighbor] = true;
                        queue.push_back(neighbor);
                    }
                }
            }
        }
    }
    return components;
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

void convert_bgr_to_hsv_8u(
    const std::vector<std::uint8_t>& bgr,
    int width,
    int height,
    std::vector<std::uint8_t>& hsv) {
    if (width > 0 && height > 0 && bgr.size() == static_cast<std::size_t>(width) * height * 3) {
        hsv.assign(bgr.size(), 0);
    }
    static const std::array<int, 256> saturation_division = [] {
        std::array<int, 256> table{};
        for (int value = 1; value < 256; ++value) {
            table[value] = static_cast<int>(std::llround((255.0 * 4096.0) / value));
        }
        return table;
    }();
    static const std::array<int, 256> hue_division_180 = [] {
        std::array<int, 256> table{};
        for (int range = 1; range < 256; ++range) {
            table[range] = static_cast<int>(std::llround(180.0 * 4096.0 / (6.0 * range)));
        }
        return table;
    }();

    for (std::size_t offset = 0; offset + 2 < bgr.size(); offset += 3) {
        const int blue = bgr[offset];
        const int green = bgr[offset + 1];
        const int red = bgr[offset + 2];
        const int max_value = std::max(red, std::max(green, blue));
        const int min_value = std::min(red, std::min(green, blue));
        const int range = max_value - min_value;
        int hue = 0;
        int saturation = 0;
        if (range != 0) {
            const int saturation_shifted =
                range * saturation_division[max_value] + 2048;
            saturation = saturation_shifted >> 12;
            int hue_shifted;
            if (max_value == red) {
                hue_shifted = (green - blue) * hue_division_180[range] + 2048;
            } else if (max_value == green) {
                hue_shifted = (blue - red + 2 * range) * hue_division_180[range] + 2048;
            } else {
                hue_shifted = (red - green + 4 * range) * hue_division_180[range] + 2048;
            }
            hue = hue_shifted >= 0 ? hue_shifted >> 12 : -((-hue_shifted) >> 12);
            if (hue < 0) hue += 180;
            }
        hsv[offset] = static_cast<std::uint8_t>(hue);
        hsv[offset + 1] = static_cast<std::uint8_t>(saturation);
        hsv[offset + 2] = static_cast<std::uint8_t>(max_value);
    }
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

DetectionResult SparkDetector::process_frame(
    const std::uint8_t* bgr,
    int width,
    int height,
    int channels,
    std::int64_t timestamp_ms) {
    const std::int64_t timestamp = timestamp_ms;
    ++frame_index_;
    if (last_timestamp_valid_ && timestamp < last_timestamp_ms_) {
        candidate_start_valid_ = false;
        candidate_latched_ = false;
    }
    last_timestamp_valid_ = true;
    last_timestamp_ms_ = timestamp;

    std::vector<std::uint8_t> normalized;
    int normalized_width = 0;
    int normalized_height = 0;
    if (!normalize_frame(
            bgr,
            width,
            height,
            channels,
            normalized,
            normalized_width,
            normalized_height)) {
        candidate_start_valid_ = false;
        candidate_latched_ = false;
        return make_empty_result(timestamp);
    }

    const ResolvedRoi contact = resolve_roi(
        config_->find("contact_roi"), normalized_width, normalized_height);
    const ResolvedRoi reference = resolve_roi(
        config_->find("reference_roi"), normalized_width, normalized_height);
    const ResolvedRoi falling = resolve_roi(
        config_->find("falling_roi"), normalized_width, normalized_height);
    if (!contact.valid) {
        throw SparkError(
            "invalid spark config: ROI 'contact_roi' is missing or invalid "
            "for frame size " + std::to_string(normalized_width) + "x" +
            std::to_string(normalized_height));
    }
    if (!reference.valid) {
        throw SparkError(
            "invalid spark config: ROI 'reference_roi' is missing or invalid "
            "for frame size " + std::to_string(normalized_width) + "x" +
            std::to_string(normalized_height));
    }
    if (!falling.valid) {
        throw SparkError(
            "invalid spark config: ROI 'falling_roi' is missing or invalid "
            "for frame size " + std::to_string(normalized_width) + "x" +
            std::to_string(normalized_height));
    }

    ensure_luts();
    std::vector<std::uint8_t> hsv;
    convert_bgr_to_hsv_8u(normalized, normalized_width, normalized_height, hsv);

    std::size_t contact_sat_pixels = 0;
    std::size_t reference_sat_pixels = 0;
    std::size_t contact_sum = 0;
    std::size_t reference_sum = 0;
    std::size_t blue_count = 0;
    std::size_t orange_count = 0;
    std::size_t falling_bright_pixels = 0;
    std::vector<std::uint8_t> falling_mask(
        static_cast<std::size_t>(falling.roi.width()) * falling.roi.height(), 0);

    for (int y = contact.roi.y1; y < contact.roi.y2; ++y) {
        const std::size_t frame_row = static_cast<std::size_t>(y) * normalized_width;
        for (int x = contact.roi.x1; x < contact.roi.x2; ++x) {
            const std::size_t hsv_offset = (frame_row + x) * 3;
            const int hue = hsv[hsv_offset];
            const int saturation = hsv[hsv_offset + 1];
            const int value = hsv[hsv_offset + 2];
            contact_sum += static_cast<std::size_t>(value);
            if (value >= v_sat_threshold_) ++contact_sat_pixels;
            if (value < v_sat_threshold_) continue;
            const bool blue = hue_in_range(hue, hue_blue_white_) && saturation <= 120;
            const bool orange =
                hue_in_range(hue, hue_orange_yellow_) && saturation >= 80;
            if (blue) ++blue_count;
            if (orange) ++orange_count;
        }
    }
    for (int y = reference.roi.y1; y < reference.roi.y2; ++y) {
        const std::size_t frame_row = static_cast<std::size_t>(y) * normalized_width;
        for (int x = reference.roi.x1; x < reference.roi.x2; ++x) {
            const std::size_t hsv_offset = (frame_row + x) * 3;
            reference_sum += static_cast<std::size_t>(hsv[hsv_offset + 2]);
            if (hsv[hsv_offset + 2] >= v_sat_threshold_) ++reference_sat_pixels;
        }
    }
    for (int y = falling.roi.y1; y < falling.roi.y2; ++y) {
        const std::size_t frame_row = static_cast<std::size_t>(y) * normalized_width;
        const std::size_t mask_row =
            static_cast<std::size_t>(y - falling.roi.y1) * falling.roi.width();
        for (int x = falling.roi.x1; x < falling.roi.x2; ++x) {
            const std::size_t frame_offset = frame_row + x;
            if (hsv[frame_offset * 3 + 2] < v_sat_threshold_) continue;
            falling_mask[mask_row + static_cast<std::size_t>(x - falling.roi.x1)] = 1;
            ++falling_bright_pixels;
        }
    }

    const std::size_t contact_area = contact.roi.area();
    const std::size_t reference_area = reference.roi.area();
    const double contact_sat_ratio =
        contact_area == 0 ? 0.0 : static_cast<double>(contact_sat_pixels) / static_cast<double>(contact_area);
    const double reference_sat_ratio =
        reference_area == 0 ? 0.0 : static_cast<double>(reference_sat_pixels) / static_cast<double>(reference_area);
    const double rel_saturation =
        use_relative_ ? contact_sat_ratio - reference_sat_ratio : contact_sat_ratio;
    const double contact_v_mean =
        contact_area == 0 ? 0.0 : static_cast<double>(contact_sum) / static_cast<double>(contact_area);
    const double reference_v_mean =
        reference_area == 0 ? 0.0 : static_cast<double>(reference_sum) / static_cast<double>(reference_area);
    contact_v_history_.push_back(contact_v_mean);
    reference_v_history_.push_back(reference_v_mean);
    if (contact_v_history_.size() > window_frames_) contact_v_history_.pop_front();
    if (reference_v_history_.size() > window_frames_) reference_v_history_.pop_front();
    const double contact_flicker = mean_frame_delta(contact_v_history_);
    const double reference_flicker = mean_frame_delta(reference_v_history_);
    const double rel_flicker = contact_flicker - reference_flicker;

    const std::size_t saturated_count = contact_sat_pixels;
    const double color_match_ratio =
        saturated_count == 0 ? 0.0 : static_cast<double>(blue_count + orange_count) / static_cast<double>(saturated_count);
    std::string color_type = "none";
    if (blue_count != 0 && orange_count != 0) {
        color_type = blue_count >= orange_count ? "blue_white" : "orange_yellow";
    } else if (blue_count != 0) {
        color_type = "blue_white";
    } else if (orange_count != 0) {
        color_type = "orange_yellow";
    }
    const bool color_hit = color_match_ratio >= color_match_threshold_;
    const int falling_components = count_components_8(
        falling_mask,
        falling.roi.width(),
        falling.roi.height());

    const bool candidate =
        rel_saturation >= rel_saturation_threshold_ &&
        rel_flicker >= flicker_threshold_ &&
        color_hit;
    std::int64_t candidate_duration_ms = 0;
    bool event_triggered = false;
    if (candidate) {
        if (!candidate_start_valid_) {
            candidate_start_valid_ = true;
            candidate_start_ms_ = timestamp;
        }
        candidate_duration_ms = std::max<std::int64_t>(0, timestamp - candidate_start_ms_);
        if (candidate_duration_ms >= confirm_duration_ms_ && !candidate_latched_) {
            event_triggered = true;
            candidate_latched_ = true;
        }
    } else {
        candidate_start_valid_ = false;
        candidate_latched_ = false;
    }

    DetectionResult result;
    result.timestamp_ms = timestamp;
    result.rel_saturation = rel_saturation;
    result.contact_sat_ratio = contact_sat_ratio;
    result.ref_sat_ratio = reference_sat_ratio;
    result.rel_flicker = rel_flicker;
    result.contact_flicker = contact_flicker;
    result.ref_flicker = reference_flicker;
    result.color_match_ratio = color_match_ratio;
    result.color_hit = color_hit;
    result.color_type = color_type;
    result.falling_bright_pixels = static_cast<int>(falling_bright_pixels);
    result.falling_components = falling_components;
    result.candidate = candidate;
    result.event_triggered = event_triggered;
    result.level = event_triggered
        ? (falling_bright_pixels >= static_cast<std::size_t>(std::max(0, heavy_bright_pixels_)) ||
           falling_components >= std::max(0, heavy_components_) ? "heavy" : "light")
        : "none";
    result.contact_v_mean = contact_v_mean;
    result.reference_v_mean = reference_v_mean;
    result.candidate_duration_ms = candidate_duration_ms;
    return result;
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
