#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace vpt {

struct Vec2 {
    float x{};
    float y{};
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Vec4 {
    float x{};
    float y{};
    float z{};
    float w{};
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator*(float s, Vec3 v) { return v * s; }
inline Vec3 operator/(Vec3 v, float s) { return {v.x / s, v.y / s, v.z / s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) {
    a = a + b;
    return a;
}

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? v / l : Vec3{};
}
inline Vec3 clamp(Vec3 v, float lo, float hi) {
    return {std::clamp(v.x, lo, hi), std::clamp(v.y, lo, hi),
            std::clamp(v.z, lo, hi)};
}
inline float luminance(Vec3 v) { return 0.2126F * v.x + 0.7152F * v.y + 0.0722F * v.z; }

// Column-major, matching GLSL's default matrix convention.
struct Mat4 {
    std::array<float, 16> m{};

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0F;
        return r;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                r.m[c * 4 + row] += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
        }
    }
    return r;
}

inline Mat4 viewMatrix(Vec3 eye, Vec3 forward, Vec3 worldUp) {
    const Vec3 f = normalize(forward);
    const Vec3 r = normalize(cross(f, worldUp));
    const Vec3 u = cross(r, f);
    Mat4 out = Mat4::identity();
    out.m[0] = r.x;
    out.m[1] = u.x;
    out.m[2] = -f.x;
    out.m[4] = r.y;
    out.m[5] = u.y;
    out.m[6] = -f.y;
    out.m[8] = r.z;
    out.m[9] = u.z;
    out.m[10] = -f.z;
    out.m[12] = -dot(r, eye);
    out.m[13] = -dot(u, eye);
    out.m[14] = dot(f, eye);
    return out;
}

inline Mat4 perspective(float verticalFovRadians, float aspect, float nearPlane, float farPlane) {
    const float f = 1.0F / std::tan(verticalFovRadians * 0.5F);
    Mat4 out{};
    out.m[0] = f / aspect;
    // Vulkan framebuffer coordinates have a downward-positive Y axis.
    out.m[5] = -f;
    out.m[10] = farPlane / (nearPlane - farPlane);
    out.m[11] = -1.0F;
    out.m[14] = (nearPlane * farPlane) / (nearPlane - farPlane);
    return out;
}

inline std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7feb352dU;
    x ^= x >> 15U;
    x *= 0x846ca68bU;
    return x ^ (x >> 16U);
}

} // namespace vpt
