#include "json.hpp"
#include "offline_io.hpp"
#include "opencv_source.hpp"
#include "spark_detector.h"
#include "synthesize.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <iostream>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string input;
    std::string scenario;
    std::string config = "spark/spark_config.example.json";
    std::string output = "spark/prototype/output";
    double fps = 0.0;
    int frames = 60;
    std::int64_t seed = 20261105;
    bool baseline = false;
};

void print_usage() {
    std::cout <<
        "Usage: spark_offline (--input FILE | --scenario NAME)\n"
        "                     [--config FILE] [--output DIR] [--baseline]\n"
        "                     [--fps FPS] [--frames N] [--seed SEED]\n";
}

double parse_double(const std::string& text) {
    std::size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("expected a finite number: " + text);
    }
    return value;
}

int parse_int(const std::string& text) {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);
    if (consumed != text.size() || value < 1 ||
        value > static_cast<long long>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("expected a positive integer: " + text);
    }
    return static_cast<int>(value);
}

std::int64_t parse_int64(const std::string& text) {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);
    if (consumed != text.size()) {
        throw std::invalid_argument("expected an integer: " + text);
    }
    return static_cast<std::int64_t>(value);
}

Options parse_options(int argc, char** argv) {
    Options options;
    bool has_input = false;
    bool has_scenario = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value_argument = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string("missing value for ") + name);
            }
            return argv[++index];
        };
        if (argument == "--help" || argument == "-h") {
            print_usage();
            std::exit(0);
        } else if (argument == "--input") {
            if (has_scenario) throw std::invalid_argument("--input and --scenario are mutually exclusive");
            options.input = value_argument("--input");
            has_input = true;
        } else if (argument == "--scenario") {
            if (has_input) throw std::invalid_argument("--input and --scenario are mutually exclusive");
            options.scenario = value_argument("--scenario");
            has_scenario = true;
        } else if (argument == "--config") {
            options.config = value_argument("--config");
        } else if (argument == "--output") {
            options.output = value_argument("--output");
        } else if (argument == "--baseline") {
            options.baseline = true;
        } else if (argument == "--fps") {
            options.fps = parse_double(value_argument("--fps"));
        } else if (argument == "--frames") {
            options.frames = parse_int(value_argument("--frames"));
        } else if (argument == "--seed") {
            options.seed = parse_int64(value_argument("--seed"));
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (!has_input && !has_scenario) {
        throw std::invalid_argument("one of --input or --scenario is required");
    }
    if (has_scenario && !spark::is_known_scenario(options.scenario)) {
        std::string names;
        for (std::size_t index = 0; index < spark::scenario_count(); ++index) {
            if (index != 0) names += ", ";
            names += spark::scenario_name(index);
        }
        throw std::invalid_argument("unknown scenario '" + options.scenario +
            "'; expected one of " + names);
    }
    return options;
}

void validate_roi_config(
    const spark::JsonValue& config,
    int frame_width,
    int frame_height) {
    const char* const names[] = {"contact_roi", "reference_roi", "falling_roi"};
    for (const char* const name : names) {
        const spark::JsonValue* roi_config = config.find(name);
        if (roi_config == nullptr) {
            throw spark::SparkError(
                std::string("invalid spark config: missing required ROI key '") +
                name + "'");
        }
        const spark::ResolvedRoi roi = spark::resolve_roi(
            roi_config,
            frame_width,
            frame_height);
        if (!roi.valid) {
            throw spark::SparkError(
                std::string("invalid spark config: ROI '") + name +
                "' is invalid for frame size " + std::to_string(frame_width) + "x" +
                std::to_string(frame_height));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        spark::JsonValue config = spark::load_json_file(options.config);
        if (!config.is_object()) {
            throw spark::SparkError("config root must be a JSON object");
        }

        std::vector<spark::SyntheticFrame> frames;
        double fps = 25.0;
        if (!options.scenario.empty()) {
            fps = options.fps > 0.0 ? options.fps : 25.0;
            frames = spark::make_sequence(
                options.scenario,
                options.frames,
                fps,
                options.seed).frames;
        }
#ifdef SPARK_WITHOUT_OPENCV
        else {
            throw spark::SparkError(
                "file input requires OpenCV support; rebuild without "
                "SPARK_WITHOUT_OPENCV or use --scenario");
        }
#else
        else if (spark::is_image_path(options.input)) {
            fps = 25.0;
            frames.push_back(spark::read_image_file(options.input));
        } else {
            std::pair<std::vector<spark::SyntheticFrame>, double> source =
                spark::read_video_file(options.input, options.fps);
            frames = std::move(source.first);
            fps = source.second;
        }
#endif
        if (frames.empty()) throw spark::SparkError("input contains no frames");
        validate_roi_config(
            config,
            frames.front().width,
            frames.front().height);

        spark::SparkDetector detector(config, fps);
        std::vector<spark::DetectionResult> results;
        results.reserve(frames.size());
        for (std::size_t index = 0; index < frames.size(); ++index) {
            const spark::SyntheticFrame& frame = frames[index];
            const std::int64_t timestamp_ms = static_cast<std::int64_t>(
                std::llround(static_cast<double>(index) * 1000.0 / fps));
            results.push_back(detector.process_frame(
                frame.bgr.data(),
                frame.width,
                frame.height,
                3,
                timestamp_ms));
        }

        spark::ensure_output_directory(options.output);
        spark::write_metrics_csv(options.output + "/metrics.csv", results);
        const spark::JsonValue summary = spark::summarize_results(results);
        spark::write_json_file(options.output + "/summary.json", summary);

#ifndef SPARK_WITHOUT_OPENCV
        if (!options.scenario.empty() || spark::is_image_path(options.input)) {
            std::size_t event_index = results.empty() ? 0 : results.size() - 1;
            for (std::size_t index = 0; index < results.size(); ++index) {
                if (results[index].event_triggered) {
                    event_index = index;
                    break;
                }
            }
            spark::write_annotated(
                options.output + "/annotated.png",
                frames[event_index],
                results[event_index],
                config);
        }
#endif
        if (options.baseline) {
            spark::write_baseline_files(
                options.output,
                config,
                results,
                options.input.empty()
                    ? "synthetic scenario '" + options.scenario + "'"
                    : options.input);
        }

        std::cout << "processed " << results.size() << " frame(s) at "
                  << std::fixed << std::setprecision(3) << fps << " FPS\n";
        std::cout << "events: " << summary.find("event_count")->as_int64() << "\n";
        std::cout << "output: " << options.output << "\n";
        return 0;
    } catch (const spark::JsonError& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    } catch (const spark::SparkError& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    }
}
