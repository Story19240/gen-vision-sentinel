#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include "spark_detector.h"

namespace spark {

void ensure_output_directory(const std::string& path);
void write_metrics_csv(
    const std::string& path,
    const std::vector<DetectionResult>& results);
JsonValue summarize_results(const std::vector<DetectionResult>& results);
void write_json_file(const std::string& path, const JsonValue& value);
void write_baseline_files(
    const std::string& output_directory,
    const JsonValue& source_config,
    const std::vector<DetectionResult>& results,
    const std::string& baseline_source);

}  // namespace spark
