#pragma once
#include <string>
#include <vector>

struct Detection {
    int class_id = -1;
    float confidence = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    std::string class_name;
};
