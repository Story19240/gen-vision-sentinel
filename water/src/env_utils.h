#pragma once

#include <cstdlib>
#include <string>
#include <fstream>
#include <iostream>

inline void load_dotenv_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            size_t k_start = key.find_first_not_of(" \t\r\n");
            size_t k_end = key.find_last_not_of(" \t\r\n");
            if (k_start != std::string::npos && k_end != std::string::npos) {
                key = key.substr(k_start, k_end - k_start + 1);
            }
            size_t v_start = val.find_first_not_of(" \t\r\n\"'");
            size_t v_end = val.find_last_not_of(" \t\r\n\"'");
            if (v_start != std::string::npos && v_end != std::string::npos) {
                val = val.substr(v_start, v_end - v_start + 1);
            } else {
                val = "";
            }
            setenv(key.c_str(), val.c_str(), 1); // 覆盖写入环境变量
        }
    }
}

inline void init_dotenv() {
    load_dotenv_file("/data/workspace/.env");
    load_dotenv_file("/data/workspace/water/.env");
    load_dotenv_file("../../.env");
    load_dotenv_file("../.env");
    load_dotenv_file(".env");
}

inline std::string env_or(const char* name, const std::string& fallback = "") {
    init_dotenv();
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : fallback;
}
