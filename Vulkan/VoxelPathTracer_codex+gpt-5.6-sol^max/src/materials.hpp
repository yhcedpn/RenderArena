#pragma once

#include "common.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace arena {

enum class MaterialType : std::uint32_t {
    diffuse = 0,
    roughMetal = 1,
    mirror = 2,
    dielectric = 3,
    emissive = 4,
};

enum class PatternType : std::uint32_t {
    solid = 0,
    stoneNoise = 1,
    woodGrain = 2,
};

struct Material {
    std::string id;
    MaterialType type{};
    std::array<float, 3> baseColorSrgb{};
    float metallic{};
    float roughness{};
    float ior{};
    std::array<float, 3> absorption{};
    std::array<float, 3> emission{};
    PatternType pattern{};
    std::uint32_t textureSeed{};
    float baseColorVariation{};
    float roughnessVariation{};
};

struct TextureMip {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> texels;
};

struct MaterialTextureSet {
    std::uint32_t baseSize{};
    std::uint32_t mipLevels{};
    std::vector<TextureMip> colorMips;
    std::vector<TextureMip> roughnessMips;
};

class MaterialLibrary final {
public:
    static MaterialLibrary load(const std::filesystem::path& path);

    [[nodiscard]] std::uint32_t indexOf(const std::string& id) const;
    [[nodiscard]] const Material& at(std::uint32_t index) const;
    [[nodiscard]] std::uint32_t textureSize() const noexcept { return textureSize_; }
    [[nodiscard]] const std::vector<Material>& materials() const noexcept { return materials_; }
    [[nodiscard]] MaterialTextureSet generateTextures() const;

private:
    std::uint32_t textureSize_{};
    std::vector<Material> materials_;
    std::unordered_map<std::string, std::uint32_t> indices_;
};

struct alignas(16) GpuMaterial {
    std::array<float, 4> baseMetal{};
    std::array<float, 4> absorptionIor{};
    std::array<float, 4> emissionRoughness{};
    std::array<std::uint32_t, 4> typeTexture{};
};

[[nodiscard]] std::vector<GpuMaterial> makeGpuMaterials(const MaterialLibrary& library);

} // namespace arena
