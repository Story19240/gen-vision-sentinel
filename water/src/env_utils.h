#pragma once

#include <cstdlib>
#include <string>

inline std::string env_or(const char* name, const std::string& fallback = "") {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : fallback;
}
