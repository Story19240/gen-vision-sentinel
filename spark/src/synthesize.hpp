#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spark {

struct SyntheticFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgr;
};

struct SyntheticSequence {
    std::vector<SyntheticFrame> frames;
    std::string scenario;
};

std::size_t scenario_count();
const std::string& scenario_name(std::size_t index);
bool is_known_scenario(const std::string& scenario);
SyntheticSequence make_sequence(
    const std::string& scenario,
    int number_of_frames,
    double fps,
    std::int64_t seed);

}  // namespace spark
