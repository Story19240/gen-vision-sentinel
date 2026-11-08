#include <cerrno>
#include "offline_io.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace spark {
namespace {

const char* const kCsvFieldNames[] = {
    "rel_saturation",
    "contact_sat_ratio",
    "ref_sat_ratio",
    "rel_flicker",
    "contact_flicker",
    "ref_flicker",
    "color_match_ratio",
    "color_hit",
    "color_type",
    "falling_bright_pixels",
    "falling_components",
    "candidate",
    "event_triggered",
    "level",
    "contact_v_mean",
    "reference_v_mean",
    "candidate_duration_ms",
};

void write_text_file(const std::string& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw SparkError("could not write output file: " + path);
    output << text;
    output.flush();
    if (!output) throw SparkError("could not write output file: " + path);
}

std::string format_double(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0.0 ? "inf" : "-inf";
    for (int precision = 1; precision <= 17; ++precision) {
        std::ostringstream stream;
        stream << std::setprecision(precision) << value;
        std::string candidate = stream.str();
        if (std::strtod(candidate.c_str(), nullptr) == value) {
            if (candidate.find('.') == std::string::npos &&
                candidate.find('e') == std::string::npos &&
                candidate.find('E') == std::string::npos) {
                candidate += ".0";
            }
            return candidate;
        }
    }
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

std::string csv_bool(bool value) {
    return value ? "True" : "False";
}

std::string csv_field(const std::string& field, const std::string& name) {
    const bool quote = field.find(',') != std::string::npos ||
                       field.find('"') != std::string::npos ||
                       field.find('\n') != std::string::npos ||
                       field.find('\r') != std::string::npos;
    if (!quote) return field;
    std::string output = "\"";
    for (const char character : field) {
        if (character == '"') output.push_back('"');
        output.push_back(character);
    }
    output.push_back('"');
    (void)name;
    return output;
}

std::string result_csv_field(
    const DetectionResult& result,
    const std::string& name) {
    if (name == "rel_saturation") return format_double(result.rel_saturation);
    if (name == "contact_sat_ratio") return format_double(result.contact_sat_ratio);
    if (name == "ref_sat_ratio") return format_double(result.ref_sat_ratio);
    if (name == "rel_flicker") return format_double(result.rel_flicker);
    if (name == "contact_flicker") return format_double(result.contact_flicker);
    if (name == "ref_flicker") return format_double(result.ref_flicker);
    if (name == "color_match_ratio") return format_double(result.color_match_ratio);
    if (name == "color_hit") return csv_bool(result.color_hit);
    if (name == "color_type") return result.color_type;
    if (name == "falling_bright_pixels") return std::to_string(result.falling_bright_pixels);
    if (name == "falling_components") return std::to_string(result.falling_components);
    if (name == "candidate") return csv_bool(result.candidate);
    if (name == "event_triggered") return csv_bool(result.event_triggered);
    if (name == "level") return result.level;
    if (name == "contact_v_mean") return format_double(result.contact_v_mean);
    if (name == "reference_v_mean") return format_double(result.reference_v_mean);
    if (name == "candidate_duration_ms") return std::to_string(result.candidate_duration_ms);
    throw SparkError("unknown detector metric: " + name);
}

double metric_value(const DetectionResult& result, const std::string& name) {
    if (name == "rel_saturation") return result.rel_saturation;
    if (name == "contact_sat_ratio") return result.contact_sat_ratio;
    if (name == "ref_sat_ratio") return result.ref_sat_ratio;
    if (name == "rel_flicker") return result.rel_flicker;
    if (name == "contact_flicker") return result.contact_flicker;
    if (name == "ref_flicker") return result.ref_flicker;
    if (name == "color_match_ratio") return result.color_match_ratio;
    if (name == "color_hit") return result.color_hit ? 1.0 : 0.0;
    if (name == "falling_bright_pixels") return static_cast<double>(result.falling_bright_pixels);
    if (name == "falling_components") return static_cast<double>(result.falling_components);
    if (name == "candidate") return result.candidate ? 1.0 : 0.0;
    if (name == "event_triggered") return result.event_triggered ? 1.0 : 0.0;
    if (name == "contact_v_mean") return result.contact_v_mean;
    if (name == "reference_v_mean") return result.reference_v_mean;
    if (name == "candidate_duration_ms") return static_cast<double>(result.candidate_duration_ms);
    throw SparkError("unknown numeric metric: " + name);
}

bool is_bool_metric(const std::string& name) {
    return name == "color_hit" || name == "candidate" || name == "event_triggered";
}

bool is_integer_metric(const std::string& name) {
    return name == "falling_bright_pixels" ||
           name == "falling_components" ||
           name == "candidate_duration_ms";
}

JsonValue& get_or_create_member(JsonValue& object, const std::string& key, JsonValue fallback) {
    if (!object.is_object()) throw SparkError("expected JSON object while updating config");
    if (JsonValue* existing = object.find(key)) {
        *existing = std::move(fallback);
        return *existing;
    }
    auto& members = object.as_object();
    members.push_back(JsonValue::Member{key, std::move(fallback)});
    return members.back().value;
}

double percentile_linear(std::vector<double> values, double percentile) {
    if (values.empty()) throw SparkError("cannot calculate baseline for zero frames");
    std::sort(values.begin(), values.end());
    if (values.size() == 1) return values.front();
    const double position = percentile / 100.0 *
        static_cast<double>(values.size() - 1);
    const double lower_index = std::floor(position);
    const double upper_index = std::ceil(position);
    const double fraction = position - lower_index;
    const std::size_t lower = static_cast<std::size_t>(lower_index);
    const std::size_t upper = static_cast<std::size_t>(upper_index);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

struct MetricStatistics {
    double mean = 0.0;
    double standard_deviation = 0.0;
    double percentile_99_9 = 0.0;
    double suggestion = 0.0;
};

MetricStatistics calculate_metric_statistics(
    const std::vector<DetectionResult>& results,
    const std::string& metric_name) {
    std::vector<double> values;
    values.reserve(results.size());
    double sum = 0.0;
    for (const DetectionResult& result : results) {
        const double value = metric_value(result, metric_name);
        values.push_back(value);
        sum += value;
    }
    MetricStatistics statistics;
    statistics.mean = sum / static_cast<double>(values.size());
    for (const double value : values) {
        statistics.standard_deviation += (value - statistics.mean) * (value - statistics.mean);
    }
    statistics.standard_deviation =
        std::sqrt(statistics.standard_deviation / static_cast<double>(values.size()));
    statistics.percentile_99_9 = percentile_linear(values, 99.9);
    statistics.suggestion = statistics.mean + 5.0 * statistics.standard_deviation;
    return statistics;
}

JsonValue statistics_json(const MetricStatistics& statistics) {
    std::vector<JsonValue::Member> members;
    members.push_back({"mean", JsonValue::number(statistics.mean)});
    members.push_back({"std", JsonValue::number(statistics.standard_deviation)});
    members.push_back({"p99_9", JsonValue::number(statistics.percentile_99_9)});
    members.push_back({
        "suggested_threshold_mean_plus_5_sigma",
        JsonValue::number(statistics.suggestion)});
    return JsonValue::object(std::move(members));
}

}  // namespace

void ensure_output_directory(const std::string& path) {
    if (path.empty() || path == ".") return;
    std::string current;
    std::size_t offset = 0;
    if (path[0] == '/') {
        current = "/";
        offset = 1;
    }
    while (offset <= path.size()) {
        const std::size_t separator = path.find('/', offset);
        const std::size_t end = separator == std::string::npos ? path.size() : separator;
        if (end > offset) {
            if (!current.empty() && current.back() != '/') current += '/';
            current += path.substr(offset, end - offset);
            if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                throw SparkError("could not create output directory: " + current);
            }
        }
        if (separator == std::string::npos) break;
        offset = separator + 1;
    }
}

void write_metrics_csv(
    const std::string& path,
    const std::vector<DetectionResult>& results) {
    std::ostringstream output;
    output << "frame_index,timestamp_ms";
    for (const char* const name : kCsvFieldNames) {
        output << ',' << name;
    }
    output << "\r\n";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const DetectionResult& result = results[index];
        output << index << ',' << result.timestamp_ms;
        for (const char* const name : kCsvFieldNames) {
            output << ',' << csv_field(result_csv_field(result, name), name);
        }
        output << "\r\n";
    }
    write_text_file(path, output.str());
}

JsonValue summarize_results(const std::vector<DetectionResult>& results) {
    std::vector<JsonValue> events;
    for (std::size_t index = 0; index < results.size(); ++index) {
        const DetectionResult& result = results[index];
        if (!result.event_triggered) continue;
        std::vector<JsonValue::Member> event_members;
        event_members.push_back({"frame_index", JsonValue::integer(static_cast<std::int64_t>(index))});
        event_members.push_back({"frame_number", JsonValue::integer(static_cast<std::int64_t>(index + 1))});
        event_members.push_back({"timestamp_ms", JsonValue::integer(result.timestamp_ms)});
        event_members.push_back({"level", JsonValue::string(result.level)});
        event_members.push_back({"color_type", JsonValue::string(result.color_type)});
        events.push_back(JsonValue::object(std::move(event_members)));
    }

    std::vector<JsonValue::Member> max_metrics;
    for (const char* const name : kCsvFieldNames) {
        const std::string metric_name(name);
        if (metric_name == "color_type" || metric_name == "level") continue;
        bool first = true;
        double maximum = 0.0;
        for (const DetectionResult& result : results) {
            const double value = metric_value(result, metric_name);
            if (first || value > maximum) {
                maximum = value;
                first = false;
            }
        }
        if (first) continue;
        if (is_bool_metric(metric_name)) {
            max_metrics.push_back({metric_name, JsonValue::integer(static_cast<std::int64_t>(maximum))});
        } else if (is_integer_metric(metric_name)) {
            max_metrics.push_back({metric_name, JsonValue::integer(static_cast<std::int64_t>(maximum))});
        } else {
            max_metrics.push_back({metric_name, JsonValue::number(maximum)});
        }
    }

    std::vector<JsonValue::Member> members;
    members.push_back({"total_frames", JsonValue::integer(static_cast<std::int64_t>(results.size()))});
    members.push_back({"event_count", JsonValue::integer(static_cast<std::int64_t>(events.size()))});
    members.push_back({"events", JsonValue::array(std::move(events))});
    members.push_back({"max_metrics", JsonValue::object(std::move(max_metrics))});
    return JsonValue::object(std::move(members));
}

void write_json_file(const std::string& path, const JsonValue& value) {
    write_text_file(path, json_dump(value, 2) + "\n");
}

void write_baseline_files(
    const std::string& output_directory,
    const JsonValue& source_config,
    const std::vector<DetectionResult>& results,
    const std::string& baseline_source) {
    const char* const metric_names[] = {
        "rel_saturation",
        "rel_flicker",
        "falling_bright_pixels",
    };
    MetricStatistics statistics[3];
    std::vector<JsonValue::Member> baseline_members;
    for (std::size_t index = 0; index < 3; ++index) {
        statistics[index] = calculate_metric_statistics(results, metric_names[index]);
        baseline_members.push_back({metric_names[index], statistics_json(statistics[index])});
    }
    write_json_file(
        output_directory + "/baseline.json",
        JsonValue::object(std::move(baseline_members)));

    JsonValue calibrated = source_config;
    if (!calibrated.is_object()) throw SparkError("config root must be a JSON object");
    JsonValue empty_thresholds = JsonValue::object({});
    JsonValue& thresholds = get_or_create_member(
        calibrated, "thresholds", std::move(empty_thresholds));
    get_or_create_member(
        thresholds,
        "rel_saturation",
        JsonValue::number(statistics[0].suggestion));
    get_or_create_member(
        thresholds,
        "flicker",
        JsonValue::number(statistics[1].suggestion));

    JsonValue empty_level_thresholds = JsonValue::object({});
    JsonValue& level_thresholds = get_or_create_member(
        calibrated, "level_thresholds", std::move(empty_level_thresholds));
    get_or_create_member(
        level_thresholds,
        "heavy_bright_pixels",
        JsonValue::integer(static_cast<std::int64_t>(
            std::ceil(statistics[2].suggestion))));

    get_or_create_member(
        calibrated,
        "_comment",
        JsonValue::string(
            "由 run_offline.py --baseline 自动生成。基线输入：" + baseline_source +
            "；本次统计帧数：" + std::to_string(results.size()) +
            "。阈值按相机各自标定，不可跨相机套用；使用前必须核对本文件中的 ROI 与目标相机画面一致。"));

    std::vector<JsonValue::Member> calibration_members;
    for (std::size_t index = 0; index < 3; ++index) {
        std::vector<JsonValue::Member> metric_members;
        metric_members.push_back({"mean", JsonValue::number(statistics[index].mean)});
        metric_members.push_back({"std", JsonValue::number(statistics[index].standard_deviation)});
        metric_members.push_back({
            "suggested",
            JsonValue::number(statistics[index].suggestion)});
        calibration_members.push_back({
            metric_names[index],
            JsonValue::object(std::move(metric_members))});
    }
    get_or_create_member(
        calibrated,
        "calibration",
        JsonValue::object(std::move(calibration_members)));
    write_json_file(
        output_directory + "/calibrated_config.json",
        calibrated);
}

}  // namespace spark
