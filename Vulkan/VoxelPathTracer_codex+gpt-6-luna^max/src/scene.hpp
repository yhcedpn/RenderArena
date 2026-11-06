#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace voxel {

enum class MaterialType : std::uint32_t { diffuse, roughMetal, mirror, dielectric, emissive };
enum class Pattern : std::uint32_t { solid, stoneNoise, woodGrain };

struct Material {
    std::string id;
    MaterialType type{};
    std::array<float, 3> baseColorSRGB{};
    float metallic{};
    float roughness{};
    float ior{};
    std::array<float, 3> absorption{};
    std::array<float, 3> emission{};
    Pattern pattern{};
    std::uint32_t textureSeed{};
    float baseColorVariation{};
    float roughnessVariation{};
};

struct TextureMip {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
    std::vector<std::uint8_t> roughness;
};

struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 2> uv{};
};

struct Triangle {
    std::array<std::uint32_t, 3> vertex{};
    std::uint32_t material{};
    std::uint32_t surfaceId{};
};

struct LightTriangle {
    std::uint32_t triangle{};
    float area{};
    float power{};
    float cdf{};
};

struct Scene {
    std::uint32_t textureSize{};
    std::uint32_t mipCount{};
    std::vector<Material> materials;
    std::vector<TextureMip> textures;
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    std::vector<LightTriangle> lights;
    float totalLightPower{};
    std::array<std::uint32_t, 8> materialByRole{};
};

Scene loadScene(const std::filesystem::path& materialsPath);

} // namespace voxel
