#pragma once

#include "math.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace arena {

enum class MaterialType : std::uint32_t {
    Diffuse = 0,
    RoughMetal = 1,
    Mirror = 2,
    Dielectric = 3,
    Emissive = 4,
};

enum class Pattern : std::uint32_t {
    Solid = 0,
    StoneNoise = 1,
    WoodGrain = 2,
};

struct Material {
    std::string id;
    MaterialType type{};
    Vec3 baseColorSrgb{};
    float metallic{};
    float roughness{};
    float ior{};
    Vec3 absorption{};
    Vec3 emission{};
    Pattern pattern{};
    std::uint32_t textureSeed{};
    float baseColorVariation{};
    float roughnessVariation{};
};

struct alignas(16) GpuMaterial {
    Vec4 baseColorType{};
    Vec4 emissionIor{};
    Vec4 absorptionRoughness{};
    std::uint32_t textureLayer{};
    std::uint32_t materialType{};
    std::uint32_t pad0{};
    std::uint32_t pad1{};
};

struct TextureMip {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> colorRgba;
    std::vector<std::uint8_t> roughness;
};

struct MaterialLibrary {
    std::uint32_t textureSize{};
    std::vector<Material> materials;
    std::vector<GpuMaterial> gpuMaterials;
    std::vector<TextureMip> mips;
    std::unordered_map<std::string, std::uint32_t> indexById;
};

MaterialLibrary loadMaterials(const std::filesystem::path& path);

} // 命名空间 arena
