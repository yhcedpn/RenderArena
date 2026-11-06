#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace arena {

struct Vec3 { float x, y, z; };
using Color = std::array<float, 3>;
enum class MaterialType { diffuse, roughMetal, mirror, dielectric, emissive };
enum class Pattern { solid, stoneNoise, woodGrain };

struct MipLevel {
    std::uint32_t size;
    std::vector<std::uint8_t> color; // RGBA sRGB
    std::vector<std::uint8_t> roughness; // R unorm, linear
};
struct Material {
    std::string id;
    MaterialType type;
    Color baseColor, absorption, emission;
    float metallic, roughness, ior, colorVariation, roughnessVariation;
    Pattern pattern;
    std::uint32_t seed;
    std::vector<MipLevel> mip;
};
struct Vertex {
    Vec3 position, normal;
    std::array<float, 2> uv;
};
struct Triangle {
    std::array<Vertex, 3> vertices;
    std::uint32_t material, surface;
};
struct Light {
    std::uint32_t triangle;
    float area, cumulativePower;
};
struct Scene {
    std::uint32_t textureSize;
    std::vector<Material> materials;
    std::vector<Triangle> triangles;
    std::vector<Light> lights;
    float lightPower = 0;
};

Scene loadScene(const std::filesystem::path& materialPath);

}
