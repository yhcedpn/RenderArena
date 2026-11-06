#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace voxelpt
{
using Color3 = std::array<float, 3>;

enum class MaterialType
{
    diffuse,
    roughMetal,
    mirror,
    dielectric,
    emissive
};

enum class Pattern
{
    solid,
    stoneNoise,
    woodGrain
};

struct Material
{
    std::string id;
    MaterialType type{};
    Color3 baseColorSrgb{};
    float metallic{};
    float roughness{};
    float ior{};
    Color3 absorption{};
    Color3 emission{};
    Pattern pattern{};
    std::uint32_t textureSeed{};
    float baseColorVariation{};
    float roughnessVariation{};
};

struct TextureMip
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<Color3> baseColorSrgb;
    std::vector<float> roughness;
};

struct MaterialLibrary
{
    std::uint32_t textureSize{};
    std::vector<Material> materials;
    std::map<std::string, std::uint32_t, std::less<>> ids;
    std::vector<std::vector<TextureMip>> textures;

    [[nodiscard]] std::uint32_t requireId(const std::string& id) const;
};

struct Vertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv{};
    std::uint32_t surfaceId{};
};

struct TriangleMesh
{
    std::uint32_t material{};
    std::uint32_t nextSurfaceId{};
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

struct Scene
{
    std::vector<TriangleMesh> opaqueMeshes;
    TriangleMesh glassMesh;
    std::uint32_t exposedLightTriangles{};
};

[[nodiscard]] MaterialLibrary loadMaterials(const std::filesystem::path& path);
[[nodiscard]] Scene buildScene(const MaterialLibrary& materials);
}
