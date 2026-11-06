#include "materials.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace arena {
namespace {

using Json = nlohmann::json;

[[noreturn]] void materialError(const std::filesystem::path& path, const std::string& detail) {
    throw std::runtime_error("材质配置错误 [" + path.string() + "]: " + detail);
}

const Json& required(const Json& object, const char* name, const std::filesystem::path& path) {
    const auto iterator = object.find(name);
    if (iterator == object.end()) {
        materialError(path, std::string("缺少必填字段 '") + name + "'");
    }
    return *iterator;
}

float finiteNumber(const Json& value, const std::string& field, const std::filesystem::path& path) {
    if (!value.is_number()) {
        materialError(path, field + " 必须是数值");
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        number > static_cast<double>(std::numeric_limits<float>::max())) {
        materialError(path, field + " 必须是有限 float 范围数值");
    }
    return static_cast<float>(number);
}

float rangedNumber(const Json& value, const std::string& field, float minimum, float maximum,
                   const std::filesystem::path& path) {
    const float number = finiteNumber(value, field, path);
    if (number < minimum || number > maximum) {
        std::ostringstream stream;
        stream << field << " 超出范围 [" << minimum << ", " << maximum << ']';
        materialError(path, stream.str());
    }
    return number;
}

Vec3 vec3Field(const Json& value, const std::string& field, float minimum, float maximum,
               const std::filesystem::path& path) {
    if (!value.is_array() || value.size() != 3) {
        materialError(path, field + " 必须是恰含 3 个数值的数组");
    }
    return {
        rangedNumber(value[0], field + "[0]", minimum, maximum, path),
        rangedNumber(value[1], field + "[1]", minimum, maximum, path),
        rangedNumber(value[2], field + "[2]", minimum, maximum, path),
    };
}

MaterialType parseMaterialType(const std::string& text, const std::filesystem::path& path) {
    if (text == "diffuse") return MaterialType::diffuse;
    if (text == "rough_metal") return MaterialType::roughMetal;
    if (text == "mirror") return MaterialType::mirror;
    if (text == "dielectric") return MaterialType::dielectric;
    if (text == "emissive") return MaterialType::emissive;
    materialError(path, "未知材质 type: " + text);
}

PatternType parsePattern(const std::string& text, const std::filesystem::path& path) {
    if (text == "solid") return PatternType::solid;
    if (text == "stone_noise") return PatternType::stoneNoise;
    if (text == "wood_grain") return PatternType::woodGrain;
    materialError(path, "未知材质 pattern: " + text);
}

std::uint32_t hash2(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept {
    std::uint32_t value = seed ^ (static_cast<std::uint32_t>(x) * 0x9E3779B9U) ^
                          (static_cast<std::uint32_t>(y) * 0x85EBCA6BU);
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;
    value *= 0x846CA68BU;
    return value ^ (value >> 16U);
}

float hashUnit(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept {
    return static_cast<float>(hash2(x, y, seed) >> 8U) * (1.0F / 16777215.0F);
}

float smooth(float value) noexcept { return value * value * (3.0F - 2.0F * value); }

float valueNoise(float x, float y, std::uint32_t seed) noexcept {
    const auto ix = static_cast<std::int32_t>(std::floor(x));
    const auto iy = static_cast<std::int32_t>(std::floor(y));
    const float fx = smooth(x - static_cast<float>(ix));
    const float fy = smooth(y - static_cast<float>(iy));
    const float a = std::lerp(hashUnit(ix, iy, seed), hashUnit(ix + 1, iy, seed), fx);
    const float b = std::lerp(hashUnit(ix, iy + 1, seed), hashUnit(ix + 1, iy + 1, seed), fx);
    return std::lerp(a, b, fy);
}

float modulation(const Material& material, float u, float v) noexcept {
    if (material.pattern == PatternType::solid) return 0.0F;
    if (material.pattern == PatternType::stoneNoise) {
        const float n0 = valueNoise(u * 3.0F, v * 3.0F, material.textureSeed);
        const float n1 = valueNoise(u * 7.0F, v * 7.0F, material.textureSeed ^ 0xA511E9B3U);
        const float n2 = valueNoise(u * 15.0F, v * 15.0F, material.textureSeed ^ 0x63D83595U);
        return (0.55F * n0 + 0.30F * n1 + 0.15F * n2) * 2.0F - 1.0F;
    }
    const float warp = valueNoise(u * 2.0F, v * 2.0F, material.textureSeed) - 0.5F;
    const float grain = 0.5F + 0.5F * std::sin((v + 0.12F * warp) * 50.265482F);
    const float low = valueNoise(u * 3.0F, v * 3.0F, material.textureSeed ^ 0xD1B54A35U);
    return std::clamp((grain * 0.7F + low * 0.3F) * 2.0F - 1.0F, -1.0F, 1.0F);
}

float srgbToLinear(float value) noexcept {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float linearToSrgb(float value) noexcept {
    value = std::clamp(value, 0.0F, 1.0F);
    return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

std::uint8_t quantize(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

TextureMipChain generateTextures(const std::vector<Material>& materials, std::uint32_t size) {
    TextureMipChain result{};
    result.baseSize = size;
    result.mipCount = std::bit_width(size);
    result.layerCount = static_cast<std::uint32_t>(materials.size());
    result.baseColorRgba.resize(result.mipCount);
    result.roughness.resize(result.mipCount);

    const std::size_t baseTexels = static_cast<std::size_t>(size) * size * materials.size();
    result.baseColorRgba[0].resize(baseTexels * 4U);
    result.roughness[0].resize(baseTexels);
    for (std::size_t layer = 0; layer < materials.size(); ++layer) {
        const Material& material = materials[layer];
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
                const float m = modulation(material, u, v);
                const Vec3 color = clamp(material.baseColorSrgb * (1.0F + material.baseColorVariation * m), 0.0F, 1.0F);
                const float roughness = std::clamp(material.roughness + material.roughnessVariation * m, 0.0F, 1.0F);
                const std::size_t texel = layer * static_cast<std::size_t>(size) * size +
                                          static_cast<std::size_t>(y) * size + x;
                result.baseColorRgba[0][texel * 4U + 0U] = quantize(color.x);
                result.baseColorRgba[0][texel * 4U + 1U] = quantize(color.y);
                result.baseColorRgba[0][texel * 4U + 2U] = quantize(color.z);
                result.baseColorRgba[0][texel * 4U + 3U] = 255U;
                result.roughness[0][texel] = quantize(roughness);
            }
        }
    }

    std::uint32_t sourceSize = size;
    for (std::uint32_t level = 1; level < result.mipCount; ++level) {
        const std::uint32_t destinationSize = std::max(1U, sourceSize / 2U);
        const std::size_t destinationTexels = static_cast<std::size_t>(destinationSize) * destinationSize * materials.size();
        result.baseColorRgba[level].resize(destinationTexels * 4U);
        result.roughness[level].resize(destinationTexels);
        for (std::size_t layer = 0; layer < materials.size(); ++layer) {
            for (std::uint32_t y = 0; y < destinationSize; ++y) {
                for (std::uint32_t x = 0; x < destinationSize; ++x) {
                    Vec3 linear{};
                    float roughness = 0.0F;
                    for (std::uint32_t oy = 0; oy < 2; ++oy) {
                        for (std::uint32_t ox = 0; ox < 2; ++ox) {
                            const std::uint32_t sx = std::min(sourceSize - 1U, x * 2U + ox);
                            const std::uint32_t sy = std::min(sourceSize - 1U, y * 2U + oy);
                            const std::size_t sourceTexel = layer * static_cast<std::size_t>(sourceSize) * sourceSize +
                                                            static_cast<std::size_t>(sy) * sourceSize + sx;
                            linear += Vec3{
                                srgbToLinear(static_cast<float>(result.baseColorRgba[level - 1U][sourceTexel * 4U + 0U]) / 255.0F),
                                srgbToLinear(static_cast<float>(result.baseColorRgba[level - 1U][sourceTexel * 4U + 1U]) / 255.0F),
                                srgbToLinear(static_cast<float>(result.baseColorRgba[level - 1U][sourceTexel * 4U + 2U]) / 255.0F),
                            };
                            roughness += static_cast<float>(result.roughness[level - 1U][sourceTexel]) / 255.0F;
                        }
                    }
                    linear *= 0.25F;
                    roughness *= 0.25F;
                    const std::size_t destinationTexel = layer * static_cast<std::size_t>(destinationSize) * destinationSize +
                                                         static_cast<std::size_t>(y) * destinationSize + x;
                    result.baseColorRgba[level][destinationTexel * 4U + 0U] = quantize(linearToSrgb(linear.x));
                    result.baseColorRgba[level][destinationTexel * 4U + 1U] = quantize(linearToSrgb(linear.y));
                    result.baseColorRgba[level][destinationTexel * 4U + 2U] = quantize(linearToSrgb(linear.z));
                    result.baseColorRgba[level][destinationTexel * 4U + 3U] = 255U;
                    result.roughness[level][destinationTexel] = quantize(roughness);
                }
            }
        }
        sourceSize = destinationSize;
    }
    return result;
}

bool isZero(Vec3 value) noexcept { return value.x == 0.0F && value.y == 0.0F && value.z == 0.0F; }

} // namespace

MaterialSet loadMaterials(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("无法打开必需材质文件: " + path.string());
    }

    Json root;
    try {
        input >> root;
    } catch (const Json::exception& error) {
        materialError(path, std::string("JSON 解析失败: ") + error.what());
    }
    if (!root.is_object()) materialError(path, "根节点必须是对象");
    const Json& schema = required(root, "schemaVersion", path);
    if (!schema.is_number_integer() || schema.get<std::int64_t>() != 1) {
        materialError(path, "schemaVersion 必须为 1");
    }
    const Json& textureSizeJson = required(root, "textureSize", path);
    if (!textureSizeJson.is_number_unsigned() && !textureSizeJson.is_number_integer()) {
        materialError(path, "textureSize 必须是整数");
    }
    const std::int64_t textureSize64 = textureSizeJson.get<std::int64_t>();
    if (textureSize64 < 16 || textureSize64 > 1024 || !std::has_single_bit(static_cast<std::uint32_t>(textureSize64))) {
        materialError(path, "textureSize 必须是 [16,1024] 内的 2 的整数次幂");
    }
    const Json& list = required(root, "materials", path);
    if (!list.is_array() || list.empty()) materialError(path, "materials 必须是非空数组");

    MaterialSet result{};
    result.textureSize = static_cast<std::uint32_t>(textureSize64);
    result.materials.reserve(list.size());
    for (std::size_t index = 0; index < list.size(); ++index) {
        const Json& object = list[index];
        const std::string prefix = "materials[" + std::to_string(index) + "].";
        if (!object.is_object()) materialError(path, "materials[" + std::to_string(index) + "] 必须是对象");
        const Json& idJson = required(object, "id", path);
        const Json& typeJson = required(object, "type", path);
        const Json& patternJson = required(object, "pattern", path);
        if (!idJson.is_string() || idJson.get_ref<const std::string&>().empty()) materialError(path, prefix + "id 必须是非空字符串");
        if (!typeJson.is_string()) materialError(path, prefix + "type 必须是字符串");
        if (!patternJson.is_string()) materialError(path, prefix + "pattern 必须是字符串");

        Material material{};
        material.id = idJson.get<std::string>();
        if (result.indexById.contains(material.id)) materialError(path, "材质 id 重复: " + material.id);
        material.type = parseMaterialType(typeJson.get<std::string>(), path);
        material.baseColorSrgb = vec3Field(required(object, "baseColorSRGB", path), prefix + "baseColorSRGB", 0.0F, 1.0F, path);
        material.metallic = rangedNumber(required(object, "metallic", path), prefix + "metallic", 0.0F, 1.0F, path);
        material.roughness = rangedNumber(required(object, "roughness", path), prefix + "roughness", 0.0F, 1.0F, path);
        material.ior = finiteNumber(required(object, "ior", path), prefix + "ior", path);
        material.absorption = vec3Field(required(object, "absorption", path), prefix + "absorption", 0.0F,
                                        std::numeric_limits<float>::max(), path);
        material.emission = vec3Field(required(object, "emission", path), prefix + "emission", 0.0F,
                                      std::numeric_limits<float>::max(), path);
        material.pattern = parsePattern(patternJson.get<std::string>(), path);
        const Json& seed = required(object, "textureSeed", path);
        if ((!seed.is_number_unsigned() && !seed.is_number_integer()) || seed.get<std::int64_t>() < 0 ||
            static_cast<std::uint64_t>(seed.get<std::int64_t>()) > std::numeric_limits<std::uint32_t>::max()) {
            materialError(path, prefix + "textureSeed 必须在 uint32 范围内");
        }
        material.textureSeed = static_cast<std::uint32_t>(seed.get<std::uint64_t>());
        material.baseColorVariation = rangedNumber(required(object, "baseColorVariation", path),
                                                    prefix + "baseColorVariation", 0.0F, 1.0F, path);
        material.roughnessVariation = rangedNumber(required(object, "roughnessVariation", path),
                                                   prefix + "roughnessVariation", 0.0F, 1.0F, path);

        const bool solidOnly = material.type == MaterialType::mirror || material.type == MaterialType::dielectric ||
                               material.type == MaterialType::emissive;
        if (material.type == MaterialType::diffuse && material.metallic != 0.0F) materialError(path, material.id + ": diffuse 的 metallic 必须为 0");
        if (material.type == MaterialType::roughMetal && (material.metallic != 1.0F || material.roughness <= 0.0F)) {
            materialError(path, material.id + ": rough_metal 要求 metallic=1 且 roughness>0");
        }
        if (material.type == MaterialType::mirror && (material.metallic != 1.0F || material.roughness != 0.0F)) {
            materialError(path, material.id + ": mirror 要求 metallic=1 且 roughness=0");
        }
        if (material.type == MaterialType::dielectric && (material.metallic != 0.0F || material.roughness != 0.0F ||
                                                           material.ior <= 1.0F || material.ior > 3.0F)) {
            materialError(path, material.id + ": dielectric 要求 metallic=0、roughness=0、ior 位于 (1,3]");
        }
        if (material.type != MaterialType::dielectric && material.ior != 1.0F) materialError(path, material.id + ": 非 dielectric 的 ior 必须为 1");
        if (material.type != MaterialType::dielectric && !isZero(material.absorption)) materialError(path, material.id + ": 非 dielectric 的 absorption 必须为零");
        if (material.type != MaterialType::emissive && !isZero(material.emission)) materialError(path, material.id + ": 非 emissive 的 emission 必须为零");
        if (solidOnly && (material.pattern != PatternType::solid || material.baseColorVariation != 0.0F ||
                          material.roughnessVariation != 0.0F)) {
            materialError(path, material.id + ": mirror/dielectric/emissive 必须使用 solid 且 variation 为零");
        }

        const std::uint32_t materialIndex = static_cast<std::uint32_t>(result.materials.size());
        result.indexById.emplace(material.id, materialIndex);
        result.materials.push_back(material);
    }

    result.textures = generateTextures(result.materials, result.textureSize);
    result.gpuMaterials.reserve(result.materials.size());
    for (std::uint32_t index = 0; index < result.materials.size(); ++index) {
        const Material& material = result.materials[index];
        result.gpuMaterials.push_back({
            {material.baseColorSrgb.x, material.baseColorSrgb.y, material.baseColorSrgb.z, material.metallic},
            {material.emission.x, material.emission.y, material.emission.z, material.roughness},
            {material.absorption.x, material.absorption.y, material.absorption.z, material.ior},
            {static_cast<std::uint32_t>(material.type), index, result.textureSize, result.textures.mipCount},
        });
    }
    return result;
}

} // namespace arena
