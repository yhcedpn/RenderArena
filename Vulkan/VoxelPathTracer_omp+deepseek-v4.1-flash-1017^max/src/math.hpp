#pragma once

#include <cmath>
#include <cstdint>

namespace vpt {

constexpr float kPi = 3.14159265358979323846f;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3(float s) : x(s), y(s), z(s) {}
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
inline Vec3& operator*=(Vec3& a, float s) { a = a * s; return a; }
inline Vec3& operator-=(Vec3& a, Vec3 b) { a = a - b; return a; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    const float len = length(a);
    return len > 0.0f ? a / len : Vec3(0.0f, 0.0f, 0.0f);
}
inline float luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// 列主序 4x4 矩阵，m[col * 4 + row]，与 GLSL mat4 内存布局一致。
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return Mat4{}; }

    float& at(int col, int row) { return m[col * 4 + row]; }
    float at(int col, int row) const { return m[col * 4 + row]; }
};

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(k, row) * b.at(c, k);
            }
            r.at(c, row) = sum;
        }
    }
    return r;
}

// 右手视图矩阵，look 方向为 -Z。
inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x;  r.at(1, 0) = s.y;  r.at(2, 0) = s.z;  r.at(3, 0) = -dot(s, eye);
    r.at(0, 1) = u.x;  r.at(1, 1) = u.y;  r.at(2, 1) = u.z;  r.at(3, 1) = -dot(u, eye);
    r.at(0, 2) = -f.x; r.at(1, 2) = -f.y; r.at(2, 2) = -f.z; r.at(3, 2) = dot(f, eye);
    r.at(0, 3) = 0.0f; r.at(1, 3) = 0.0f; r.at(2, 3) = 0.0f; r.at(3, 3) = 1.0f;
    return r;
}

// Vulkan 裁剪空间 z ∈ [0,1]；y 取负以把世界 +Y 映射到屏幕上方。
inline Mat4 perspective(float fovY, float aspect, float nearZ, float farZ) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = 0.0f;
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = -f;
    r.at(2, 2) = farZ / (nearZ - farZ);
    r.at(2, 3) = -1.0f;
    r.at(3, 2) = nearZ * farZ / (nearZ - farZ);
    return r;
}

inline Vec4 transformPoint(const Mat4& m, Vec3 p) {
    Vec4 r;
    r.x = m.at(0, 0) * p.x + m.at(1, 0) * p.y + m.at(2, 0) * p.z + m.at(3, 0);
    r.y = m.at(0, 1) * p.x + m.at(1, 1) * p.y + m.at(2, 1) * p.z + m.at(3, 1);
    r.z = m.at(0, 2) * p.x + m.at(1, 2) * p.y + m.at(2, 2) * p.z + m.at(3, 2);
    r.w = m.at(0, 3) * p.x + m.at(1, 3) * p.y + m.at(2, 3) * p.z + m.at(3, 3);
    return r;
}

inline Vec3 clampColor(Vec3 c) {
    return {c.x < 0.0f ? 0.0f : (c.x > 1.0f ? 1.0f : c.x),
            c.y < 0.0f ? 0.0f : (c.y > 1.0f ? 1.0f : c.y),
            c.z < 0.0f ? 0.0f : (c.z > 1.0f ? 1.0f : c.z)};
}

inline Vec3 srgbToLinear(Vec3 c) {
    auto f = [](float v) {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    };
    return {f(c.x), f(c.y), f(c.z)};
}

inline Vec3 linearToSrgb(Vec3 c) {
    auto f = [](float v) {
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    };
    return {f(c.x), f(c.y), f(c.z)};
}

} // namespace vpt
