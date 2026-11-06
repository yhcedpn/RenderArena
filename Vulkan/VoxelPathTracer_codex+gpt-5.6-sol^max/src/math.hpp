#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace arena {

struct Vec2 {
    float x{};
    float y{};
};

struct alignas(16) Vec4 {
    float x{};
    float y{};
    float z{};
    float w{};
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] constexpr Vec3 operator-(Vec3 v) noexcept { return {-v.x, -v.y, -v.z}; }
[[nodiscard]] constexpr Vec3 operator*(Vec3 v, float s) noexcept { return {v.x * s, v.y * s, v.z * s}; }
[[nodiscard]] constexpr Vec3 operator*(float s, Vec3 v) noexcept { return v * s; }
[[nodiscard]] constexpr Vec3 operator/(Vec3 v, float s) noexcept { return {v.x / s, v.y / s, v.z / s}; }
constexpr Vec3& operator+=(Vec3& a, Vec3 b) noexcept {
    a = a + b;
    return a;
}
constexpr Vec3& operator-=(Vec3& a, Vec3 b) noexcept {
    a = a - b;
    return a;
}
constexpr Vec3& operator*=(Vec3& a, float s) noexcept {
    a = a * s;
    return a;
}

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Vec3 v) noexcept { return std::sqrt(dot(v, v)); }
[[nodiscard]] inline Vec3 normalize(Vec3 v) noexcept {
    const float l = length(v);
    return l > 0.0F ? v / l : Vec3{};
}
[[nodiscard]] constexpr Vec3 clamp(Vec3 v, float lo, float hi) noexcept {
    return {std::clamp(v.x, lo, hi), std::clamp(v.y, lo, hi), std::clamp(v.z, lo, hi)};
}
[[nodiscard]] constexpr Vec4 makeVec4(Vec3 v, float w = 0.0F) noexcept { return {v.x, v.y, v.z, w}; }
[[nodiscard]] constexpr Vec3 xyz(Vec4 v) noexcept { return {v.x, v.y, v.z}; }

[[nodiscard]] inline float radians(float degrees) noexcept {
    return degrees * 0.01745329251994329577F;
}

} // namespace arena
