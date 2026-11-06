#pragma once

#include "gpu_shared.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace arena {

enum class PatternType : std::uint32_t {
    solid,
    stoneNoise,
    woodGrain,
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
    PatternType pattern{};
    std::uint32_t textureSeed{};
    float baseColorVariation{};
    float roughnessVariation{};
};

struct TextureMipChain {
    std::uint32_t baseSize{};
    std::uint32_t mipCount{};
    std::uint32_t layerCount{};
    std::vector<std::vector<std::uint8_t>> baseColorRgba;
    std::vector<std::vector<std::uint8_t>> roughness;
};

struct MaterialSet {
    std::uint32_t textureSize{};
    std::vector<Material> materials;
    std::vector<MaterialGpu> gpuMaterials;
    std::unordered_map<std::string, std::uint32_t> indexById;
    TextureMipChain textures;
};

[[nodiscard]] MaterialSet loadMaterials(const std::filesystem::path& path);

} // namespace arena
