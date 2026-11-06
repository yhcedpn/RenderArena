#pragma once

#include <volk.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace arena {

class Error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void vkCheck(const VkResult result, const std::string_view operation)
{
    if (result != VK_SUCCESS) {
        throw Error(std::string(operation) + " 失败，VkResult=" + std::to_string(result));
    }
}

struct Vec2 {
    float x{};
    float y{};
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

constexpr Vec3 operator+(const Vec3 a, const Vec3 b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

constexpr Vec3 operator-(const Vec3 a, const Vec3 b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

constexpr Vec3 operator-(const Vec3 v) noexcept
{
    return {-v.x, -v.y, -v.z};
}

constexpr Vec3 operator*(const Vec3 v, const float s) noexcept
{
    return {v.x * s, v.y * s, v.z * s};
}

constexpr Vec3 operator*(const float s, const Vec3 v) noexcept
{
    return v * s;
}

constexpr Vec3 operator/(const Vec3 v, const float s) noexcept
{
    return {v.x / s, v.y / s, v.z / s};
}

constexpr float dot(const Vec3 a, const Vec3 b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3 cross(const Vec3 a, const Vec3 b) noexcept
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

inline float length(const Vec3 v) noexcept
{
    return std::sqrt(dot(v, v));
}

inline Vec3 normalize(const Vec3 v)
{
    const float value = length(v);
    if (!(value > 0.0F) || !std::isfinite(value)) {
        throw Error("尝试归一化无效向量");
    }
    return v / value;
}

inline float clamp(const float value, const float low, const float high) noexcept
{
    return std::fmax(low, std::fmin(value, high));
}

constexpr std::uint32_t alignUp(const std::uint32_t value, const std::uint32_t alignment) noexcept
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

constexpr VkDeviceSize alignUp(const VkDeviceSize value, const VkDeviceSize alignment) noexcept
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

inline std::vector<std::byte> readBinaryFile(const std::filesystem::path& path)
{
    std::FILE* file = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
#else
    file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
#endif
        throw Error("无法读取二进制文件：" + path.string());
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        throw Error("无法定位二进制文件末尾：" + path.string());
    }
    const long fileSize = std::ftell(file);
    if (fileSize <= 0 || (fileSize % 4L) != 0L) {
        std::fclose(file);
        throw Error("SPIR-V 文件大小无效：" + path.string());
    }
    std::rewind(file);
    std::vector<std::byte> bytes(static_cast<std::size_t>(fileSize));
    const std::size_t readCount = std::fread(bytes.data(), 1U, bytes.size(), file);
    std::fclose(file);
    if (readCount != bytes.size()) {
        throw Error("读取二进制文件不完整：" + path.string());
    }
    return bytes;
}

inline std::uint32_t fnv1a(const std::string_view text) noexcept
{
    std::uint32_t hash = 2166136261U;
    for (const char ch : text) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= 16777619U;
    }
    return hash;
}

} // namespace arena
