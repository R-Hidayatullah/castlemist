/// @file
/// @brief Implementation of the `GW2_*` environment hooks.

#include "castlemist/core/env.h"

#include <cstdlib>
#include <cstring>

namespace castlemist::core::env {

bool is_set(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0';
}

bool flag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return std::strcmp(v, "0") != 0;
}

int32_t integer(const char* name, int32_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return static_cast<int32_t>(std::strtol(v, nullptr, 0));
}

uint32_t unsigned_integer(const char* name, uint32_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
}

float real(const char* name, float fallback, float minimum) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    double d = std::atof(v);
    return (d > minimum) ? static_cast<float>(d) : fallback;
}

std::string text(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

} // namespace castlemist::core::env
