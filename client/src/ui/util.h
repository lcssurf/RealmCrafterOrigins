#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace rco::ui {

inline std::string AbbreviateNumber(uint64_t value) {
    const char* suffix = "";
    double scaled = static_cast<double>(value);
    if (value >= 1000000000ULL) {
        scaled /= 1000000000.0;
        suffix = "B";
    } else if (value >= 1000000ULL) {
        scaled /= 1000000.0;
        suffix = "M";
    } else if (value >= 1000ULL) {
        scaled /= 1000.0;
        suffix = "K";
    } else {
        return std::to_string(value);
    }

    char buf[32];
    if (scaled >= 100.0) {
        std::snprintf(buf, sizeof(buf), "%.0f%s", scaled, suffix);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f%s", scaled, suffix);
    }
    return std::string(buf);
}

inline float ProgressBetweenThresholds(uint64_t value, uint64_t current, uint64_t next) {
    if (next <= current) {
        return 1.0f;
    }
    if (value <= current) {
        return 0.0f;
    }
    if (value >= next) {
        return 1.0f;
    }
    return static_cast<float>(value - current) / static_cast<float>(next - current);
}

} // namespace rco::ui
