#include "synthesize.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace spark {
namespace {

constexpr int kSyntheticWidth = 640;
constexpr int kSyntheticHeight = 480;
constexpr int kSyntheticContactRoi[4] = {100, 200, 540, 232};
constexpr int kSyntheticReferenceRoi[4] = {100, 80, 540, 112};
constexpr int kSyntheticFallingRoi[4] = {240, 232, 400, 420};

const char* const kScenarios[] = {
    "normal",
    "normal_global_drift",
    "blue_white_spark",
    "orange_spark_falling",
    "short_flash",
    "steady_lamp",
    "spark_outside_contact",
};

struct RgbColor {
    double blue = 0.0;
    double green = 0.0;
    double red = 0.0;
};

RgbColor spark_color(const std::string& color_type) {
    if (color_type == "blue_white") return {255.0, 200.0, 190.0};
    if (color_type == "orange_yellow") return {15.0, 105.0, 255.0};
    throw std::invalid_argument("unsupported color_type: " + color_type);
}

double phase_modulation(double phase) {
    const double wave = 0.5 + 0.5 * std::sin(2.0 * M_PI * phase);
    return 0.85 + 0.15 * wave;
}

SyntheticFrame make_background(int width, int height, std::int64_t seed) {
    width = std::max(1, width);
    height = std::max(1, height);
    std::mt19937_64 random(static_cast<std::uint64_t>(seed));
    std::uniform_real_distribution<double> texture_noise(-2.5, 2.5);
    std::normal_distribution<double> pixel_noise(0.0, 0.9);

    std::vector<float> background(static_cast<std::size_t>(width) * height, 0.0f);
    for (int y = 0; y < height; ++y) {
        const float y_gradient = static_cast<float>(26.0 + 24.0 * y / static_cast<double>(height - 1));
        for (int x = 0; x < width; ++x) {
            const float x_gradient = static_cast<float>(-5.0 + 10.0 * x / static_cast<double>(width - 1));
            background[static_cast<std::size_t>(y) * width + x] = y_gradient + x_gradient;
        }
    }

    const int band_y1 = static_cast<int>(std::llround(height * 0.40));
    const int band_y2 = std::max(band_y1 + 1, static_cast<int>(std::llround(height * 0.50)));
    for (int y = band_y1; y < std::min(band_y2, height); ++y) {
        for (int x = 0; x < width; ++x) {
            background[static_cast<std::size_t>(y) * width + x] +=
                static_cast<float>(7.0 + texture_noise(random));
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            background[static_cast<std::size_t>(y) * width + x] +=
                static_cast<float>(pixel_noise(random));
        }
    }

    SyntheticFrame frame;
    frame.width = width;
    frame.height = height;
    frame.bgr.resize(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t offset = 0; offset < background.size(); ++offset) {
        const long rounded = std::lround(background[offset]);
        const std::uint8_t gray = static_cast<std::uint8_t>(std::max(0L, std::min(255L, rounded)));
        frame.bgr[offset * 3] = gray;
        frame.bgr[offset * 3 + 1] = gray;
        frame.bgr[offset * 3 + 2] = gray;
    }
    return frame;
}

SyntheticFrame add_spark(
    const SyntheticFrame& source,
    double center_x,
    double center_y,
    double radius,
    const std::string& color_type,
    double intensity,
    double phase) {
    if (source.width <= 0 || source.height <= 0 || source.bgr.size() !=
        static_cast<std::size_t>(source.width) * source.height * 3) {
        throw std::invalid_argument("frame must be a non-empty BGR image");
    }
    const RgbColor color = spark_color(color_type);
    SyntheticFrame output = source;
    const int center_x_int = static_cast<int>(std::llround(center_x));
    const int center_y_int = static_cast<int>(std::llround(center_y));
    const double spark_radius = std::max(1.0, radius);
    const int box_radius = static_cast<int>(std::ceil(spark_radius * 1.6));
    const int x1 = std::max(0, center_x_int - box_radius);
    const int x2 = std::min(source.width, center_x_int + box_radius + 1);
    const int y1 = std::max(0, center_y_int - box_radius);
    const int y2 = std::min(source.height, center_y_int + box_radius + 1);
    if (x1 >= x2 || y1 >= y2) return output;

    const double sigma = std::max(spark_radius * 0.55, 1e-6);
    const double modulation = phase_modulation(phase);
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            const double distance_squared =
                static_cast<double>((x - center_x_int) * (x - center_x_int)) +
                static_cast<double>((y - center_y_int) * (y - center_y_int));
            const double radial = std::exp(-0.5 * distance_squared / (sigma * sigma));
            const double alpha = radial * std::max(0.0, intensity) * modulation;
            const std::size_t offset =
                (static_cast<std::size_t>(y) * source.width + x) * 3;
            const double blue = output.bgr[offset] + alpha * color.blue;
            const double green = output.bgr[offset + 1] + alpha * color.green;
            const double red = output.bgr[offset + 2] + alpha * color.red;
            output.bgr[offset] = static_cast<std::uint8_t>(
                std::max(0.0, std::min(255.0, std::rint(blue))));
            output.bgr[offset + 1] = static_cast<std::uint8_t>(
                std::max(0.0, std::min(255.0, std::rint(green))));
            output.bgr[offset + 2] = static_cast<std::uint8_t>(
                std::max(0.0, std::min(255.0, std::rint(red))));
        }
    }
    return output;
}

SyntheticFrame add_falling_sparks(
    const SyntheticFrame& source,
    const int falling_roi[4],
    int count,
    std::int64_t seed,
    int frame_index) {
    SyntheticFrame output = source;
    const int x1 = falling_roi[0];
    const int y1 = falling_roi[1];
    const int width = std::max(0, falling_roi[2] - falling_roi[0]);
    const int height = std::max(0, falling_roi[3] - falling_roi[1]);
    if (width <= 0 || height <= 0 || count <= 0) return output;

    std::mt19937_64 random(static_cast<std::uint64_t>(seed));
    std::uniform_real_distribution<double> base_x_distribution(0.08, 0.92);
    std::uniform_real_distribution<double> lane_jitter(-0.025, 0.025);
    std::uniform_real_distribution<double> speed_distribution(0.018, 0.035);
    std::uniform_real_distribution<double> offset_distribution(0.0, 1.0);
    std::uniform_real_distribution<double> radius_distribution(2.0, 3.1);

    for (int index = 0; index < count; ++index) {
        const double unused_base_x = base_x_distribution(random);
        (void)unused_base_x;
        const double lane = 0.08 + (0.92 - 0.08) * index / std::max(1, count - 1) +
                            lane_jitter(random);
        const double progress =
            std::fmod(offset_distribution(random) +
                static_cast<double>(frame_index) * speed_distribution(random), 1.0);
        const double particle_x = x1 + lane * width;
        const double particle_y = y1 + progress * (height - 1);
        const double particle_radius = radius_distribution(random);
        output = add_spark(
            output,
            particle_x,
            particle_y,
            particle_radius,
            "orange_yellow",
            1.0,
            0.0);
    }
    return output;
}

SyntheticFrame drift_frame(
    const SyntheticFrame& source,
    int frame_index,
    double fps) {
    const double phase = frame_index / std::max(fps, 1.0);
    const int offset = static_cast<int>(std::llround(10.0 *
        std::sin(2.0 * M_PI * phase / 2.5)));
    SyntheticFrame output = source;
    for (std::uint8_t& channel : output.bgr) {
        const int value = channel + offset;
        channel = static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
    }
    return output;
}

}  // namespace

std::size_t scenario_count() {
    return sizeof(kScenarios) / sizeof(kScenarios[0]);
}

const std::string& scenario_name(std::size_t index) {
    if (index >= scenario_count()) throw std::out_of_range("scenario index");
    static thread_local std::string name;
    name = kScenarios[index];
    return name;
}

bool is_known_scenario(const std::string& scenario) {
    for (std::size_t index = 0; index < scenario_count(); ++index) {
        if (scenario == kScenarios[index]) return true;
    }
    return false;
}

SyntheticSequence make_sequence(
    const std::string& scenario,
    int number_of_frames,
    double fps,
    std::int64_t seed) {
    if (!is_known_scenario(scenario)) {
        throw std::invalid_argument("unknown scenario: " + scenario);
    }
    const int frame_count = std::max(1, number_of_frames);
    const double frame_rate = std::max(1.0, fps);
    const int start_frame = std::max(1, static_cast<int>(std::llround(frame_rate * 0.2)));
    const int spark_frames = std::max(2, static_cast<int>(std::llround(frame_rate * 0.6)));
    const int end_frame = std::min(frame_count - 1, start_frame + spark_frames - 1);
    const int effect_end_frame = scenario == "short_flash"
        ? std::min(start_frame + 1, frame_count - 1)
        : end_frame;
    const double contact_center_x = (kSyntheticContactRoi[0] + kSyntheticContactRoi[2]) / 2.0;
    const double contact_center_y = (kSyntheticContactRoi[1] + kSyntheticContactRoi[3]) / 2.0;
    const double reference_center_x = (kSyntheticReferenceRoi[0] + kSyntheticReferenceRoi[2]) / 2.0;
    const double reference_center_y = (kSyntheticReferenceRoi[1] + kSyntheticReferenceRoi[3]) / 2.0;

    SyntheticSequence sequence;
    sequence.scenario = scenario;
    sequence.frames.reserve(static_cast<std::size_t>(frame_count));
    for (int frame_index = 0; frame_index < frame_count; ++frame_index) {
        const std::int64_t background_seed = scenario == "steady_lamp"
            ? seed
            : seed + static_cast<std::int64_t>(frame_index) * 1009;
        SyntheticFrame frame = make_background(
            kSyntheticWidth,
            kSyntheticHeight,
            background_seed);

        if (scenario == "normal_global_drift") {
            frame = drift_frame(frame, frame_index, frame_rate);
        }
        if (scenario == "steady_lamp") {
            frame = add_spark(
                frame,
                contact_center_x,
                contact_center_y,
                22.0,
                "blue_white",
                1.0,
                0.0);
        }
        if ((scenario == "blue_white_spark" ||
             scenario == "orange_spark_falling" ||
             scenario == "short_flash") &&
            frame_index >= start_frame && frame_index <= effect_end_frame) {
            const double phase = (frame_index - start_frame) * 0.25;
            const std::string color_type = scenario == "orange_spark_falling"
                ? "orange_yellow"
                : "blue_white";
            frame = add_spark(
                frame,
                contact_center_x,
                contact_center_y,
                34.0,
                color_type,
                1.0,
                phase);
            if (scenario == "orange_spark_falling") {
                frame = add_falling_sparks(
                    frame,
                    kSyntheticFallingRoi,
                    6,
                    seed + 17,
                    frame_index - start_frame);
            }
        }
        if (scenario == "spark_outside_contact" &&
            frame_index >= start_frame && frame_index <= end_frame) {
            const double phase = (frame_index - start_frame) * 0.25;
            frame = add_spark(
                frame,
                reference_center_x,
                reference_center_y,
                34.0,
                "blue_white",
                1.0,
                phase);
        }
        sequence.frames.push_back(std::move(frame));
    }
    return sequence;
}

}  // namespace spark
