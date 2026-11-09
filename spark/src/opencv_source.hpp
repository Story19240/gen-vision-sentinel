#pragma once

#include <string>
#include <vector>

#include "json.hpp"
#include <utility>
#include "spark_detector.h"
#include "synthesize.hpp"

namespace spark {

bool is_image_path(const std::string& path);
SyntheticFrame read_image_file(const std::string& path);
std::pair<std::vector<SyntheticFrame>, double> read_video_file(
    const std::string& path,
    double fps_override);
void write_annotated(
    const std::string& path,
    const SyntheticFrame& frame,
    const DetectionResult& result,
    const JsonValue& config);

}  // namespace spark
