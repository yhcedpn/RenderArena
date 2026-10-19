#include "materials.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace arena {
namespace {

using Json = nlohmann::json;

[[noreturn]] void invalid(const std::filesystem::path& path, const std::string& message) {
    throw std::runtime_error("材质文件 " + path.string() + " 无效：" + message);
}

float requireFloat(const Json& object, std::string_view key, const std::filesystem::path& path) {
    if (!object.contains(key) || !object.at(key).is_number()) {
        invalid(path, "字段 " + std::string(key) + " 必须是数值");
    }
    const double value = object.at(key).get<double>();
    if (!std::isfinite(value) || value < -std::numeric_limits<float>::max() || value > std::numeric_limits<float>::max()) {
        invalid(path, "字段 " + std::string(key) + " 必须是有限 float");
    }
    return static_cast<float>(value);
}

Vec3 requireVec3(const Json& object, std::string_view key, const std::filesystem::path& path) {
    if (!object.contains(key) || !object.at(key).is_array() || object.at(key).size() != 3) {
        invalid(path, "字段 " + std::string(key) + " 必须是三个数的数组");
    }
    const auto& a = object.at(key);
    for (std::size_t i = 0; i < 3; ++i) {
        if (!a[i].is_number() || !std::isfinite(a[i].get<double>())) {
            invalid(path, "字段 " + std::string(key) + " 包含非法分量");
        }
    }
    return {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
}

void requireRange(float value, float lo, float hi, const std::filesystem::path& path, std::string_view key) {
    if (value < lo || value > hi) {
        invalid(path, "字段 " + std::string(key) + " 超出范围");
    }
}

MaterialType parseType(const std::string& value, const std::filesystem::path& path) {
    if (value == "diffuse") return MaterialType::Diffuse;
    if (value == "rough_metal") return MaterialType::RoughMetal;
    if (value == "mirror") return MaterialType::Mirror;
    if (value == "dielectric") return MaterialType::Dielectric;
    if (value == "emissive") return MaterialType::Emissive;
    invalid(path, "未知材质类型 " + value);
}

Pattern parsePattern(const std::string& value, const std::filesystem::path& path) {
    if (value == "solid") return Pattern::Solid;
    if (value == "stone_noise") return Pattern::StoneNoise;
    if (value == "wood_grain") return Pattern::WoodGrain;
    invalid(path, "未知纹理模式 " + value);
}

float srgbToLinear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

float linearToSrgb(float c) {
    c = std::max(c, 0.0F);
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
}

float random01(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    return static_cast<float>(hash32(seed ^ hash32(ux * 0x9e3779b9U) ^ hash32(uy * 0x85ebca6bU))) * (1.0F / 4294967295.0F);
}

float smooth(float t) {
    return t * t * (3.0F - 2.0F * t);
}

float valueNoise(float x, float y, std::uint32_t seed) {
    const auto ix = static_cast<std::int32_t>(std::floor(x));
    const auto iy = static_cast<std::int32_t>(std::floor(y));
    const float fx = smooth(x - std::floor(x));
    const float fy = smooth(y - std::floor(y));
    const float a = std::lerp(random01(ix, iy, seed), random01(ix + 1, iy, seed), fx);
    const float b = std::lerp(random01(ix, iy + 1, seed), random01(ix + 1, iy + 1, seed), fx);
    return std::lerp(a, b, fy);
}

float patternValue(const Material& material, float u, float v) {
    if (material.pattern == Pattern::Solid) return 0.5F;
    if (material.pattern == Pattern::StoneNoise) {
        float sum = 0.0F;
        float weight = 0.0F;
        float amplitude = 1.0F;
        for (std::uint32_t octave = 0; octave < 4; ++octave) {
            const float frequency = static_cast<float>(1U << octave) * 3.0F;
            sum += amplitude * valueNoise(u * frequency, v * frequency, material.textureSeed + octave * 1013U);
            weight += amplitude;
            amplitude *= 0.5F;
        }
        return sum / weight;
    }
    const float warp = valueNoise(u * 2.0F, v * 2.0F, material.textureSeed) - 0.5F;
    return 0.5F + 0.5F * std::sin((v * 11.0F + warp * 2.5F) * 6.28318530718F);
}

std::uint8_t quantize(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

void buildBaseMip(MaterialLibrary& result) {
    TextureMip level{};
    level.width = result.textureSize;
    level.height = result.textureSize;
    const std::size_t layerPixels = static_cast<std::size_t>(level.width) * level.height;
    level.colorRgba.resize(layerPixels * result.materials.size() * 4U);
    level.roughness.resize(layerPixels * result.materials.size());

    for (std::size_t layer = 0; layer < result.materials.size(); ++layer) {
        const Material& material = result.materials[layer];
        for (std::uint32_t y = 0; y < level.height; ++y) {
            for (std::uint32_t x = 0; x < level.width; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(level.width);
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(level.height);
                const float p = patternValue(material, u, v) * 2.0F - 1.0F;
                const float colorScale = 1.0F + p * material.baseColorVariation;
                const Vec3 color = clamp(material.baseColorSrgb * colorScale, 0.0F, 1.0F);
                const float roughness = std::clamp(material.roughness + p * material.roughnessVariation, 0.0F, 1.0F);
                const std::size_t pixel = layer * layerPixels + static_cast<std::size_t>(y) * level.width + x;
                level.colorRgba[pixel * 4U + 0U] = quantize(color.x);
                level.colorRgba[pixel * 4U + 1U] = quantize(color.y);
                level.colorRgba[pixel * 4U + 2U] = quantize(color.z);
                level.colorRgba[pixel * 4U + 3U] = 255U;
                level.roughness[pixel] = quantize(roughness);
            }
        }
    }
    result.mips.push_back(std::move(level));
}

void buildRemainingMips(MaterialLibrary& result) {
    while (result.mips.back().width > 1U || result.mips.back().height > 1U) {
        const TextureMip& source = result.mips.back();
        TextureMip target{};
        target.width = std::max(1U, source.width / 2U);
        target.height = std::max(1U, source.height / 2U);
        const std::size_t sourceLayerPixels = static_cast<std::size_t>(source.width) * source.height;
        const std::size_t targetLayerPixels = static_cast<std::size_t>(target.width) * target.height;
        target.colorRgba.resize(targetLayerPixels * result.materials.size() * 4U);
        target.roughness.resize(targetLayerPixels * result.materials.size());

        for (std::size_t layer = 0; layer < result.materials.size(); ++layer) {
            for (std::uint32_t y = 0; y < target.height; ++y) {
                for (std::uint32_t x = 0; x < target.width; ++x) {
                    Vec3 linear{};
                    float roughness = 0.0F;
                    for (std::uint32_t oy = 0; oy < 2; ++oy) {
                        for (std::uint32_t ox = 0; ox < 2; ++ox) {
                            const std::uint32_t sx = std::min(source.width - 1U, x * 2U + ox);
                            const std::uint32_t sy = std::min(source.height - 1U, y * 2U + oy);
                            const std::size_t sourcePixel = layer * sourceLayerPixels + static_cast<std::size_t>(sy) * source.width + sx;
                            linear.x += srgbToLinear(static_cast<float>(source.colorRgba[sourcePixel * 4U + 0U]) / 255.0F);
                            linear.y += srgbToLinear(static_cast<float>(source.colorRgba[sourcePixel * 4U + 1U]) / 255.0F);
                            linear.z += srgbToLinear(static_cast<float>(source.colorRgba[sourcePixel * 4U + 2U]) / 255.0F);
                            roughness += static_cast<float>(source.roughness[sourcePixel]) / 255.0F;
                        }
                    }
                    linear = linear * 0.25F;
                    roughness *= 0.25F;
                    const std::size_t targetPixel = layer * targetLayerPixels + static_cast<std::size_t>(y) * target.width + x;
                    target.colorRgba[targetPixel * 4U + 0U] = quantize(linearToSrgb(linear.x));
                    target.colorRgba[targetPixel * 4U + 1U] = quantize(linearToSrgb(linear.y));
                    target.colorRgba[targetPixel * 4U + 2U] = quantize(linearToSrgb(linear.z));
                    target.colorRgba[targetPixel * 4U + 3U] = 255U;
                    target.roughness[targetPixel] = quantize(roughness);
                }
            }
        }
        result.mips.push_back(std::move(target));
    }
}

bool zero(Vec3 v) {
    return v.x == 0.0F && v.y == 0.0F && v.z == 0.0F;
}

} // 匿名命名空间

MaterialLibrary loadMaterials(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("无法打开材质文件：" + path.string());
    }

    Json root;
    try {
        stream >> root;
    } catch (const std::exception& error) {
        throw std::runtime_error("解析材质文件 " + path.string() + " 失败：" + error.what());
    }
    if (!root.is_object() || !root.contains("schemaVersion") || !root.at("schemaVersion").is_number_unsigned() || root.at("schemaVersion").get<std::uint32_t>() != 1U) {
        invalid(path, "schemaVersion 必须是 1");
    }
    if (!root.contains("textureSize") || !root.at("textureSize").is_number_unsigned()) {
        invalid(path, "textureSize 必须是无符号整数");
    }

    MaterialLibrary result{};
    result.textureSize = root.at("textureSize").get<std::uint32_t>();
    if (result.textureSize < 16U || result.textureSize > 1024U || (result.textureSize & (result.textureSize - 1U)) != 0U) {
        invalid(path, "textureSize 必须是 16 至 1024 的 2 次幂");
    }
    if (!root.contains("materials") || !root.at("materials").is_array() || root.at("materials").empty()) {
        invalid(path, "materials 必须是非空数组");
    }

    for (const Json& entry : root.at("materials")) {
        if (!entry.is_object()) invalid(path, "materials 元素必须是对象");
        Material material{};
        if (!entry.contains("id") || !entry.at("id").is_string() || entry.at("id").get<std::string>().empty()) invalid(path, "id 必须是非空字符串");
        if (!entry.contains("type") || !entry.at("type").is_string()) invalid(path, "type 必须是字符串");
        if (!entry.contains("pattern") || !entry.at("pattern").is_string()) invalid(path, "pattern 必须是字符串");
        if (!entry.contains("textureSeed") || !entry.at("textureSeed").is_number_unsigned()) invalid(path, "textureSeed 必须是 uint32");
        material.id = entry.at("id").get<std::string>();
        material.type = parseType(entry.at("type").get<std::string>(), path);
        material.pattern = parsePattern(entry.at("pattern").get<std::string>(), path);
        const std::uint64_t seed = entry.at("textureSeed").get<std::uint64_t>();
        if (seed > std::numeric_limits<std::uint32_t>::max()) invalid(path, "textureSeed 超出 uint32 范围");
        material.textureSeed = static_cast<std::uint32_t>(seed);
        material.baseColorSrgb = requireVec3(entry, "baseColorSRGB", path);
        material.absorption = requireVec3(entry, "absorption", path);
        material.emission = requireVec3(entry, "emission", path);
        material.metallic = requireFloat(entry, "metallic", path);
        material.roughness = requireFloat(entry, "roughness", path);
        material.ior = requireFloat(entry, "ior", path);
        material.baseColorVariation = requireFloat(entry, "baseColorVariation", path);
        material.roughnessVariation = requireFloat(entry, "roughnessVariation", path);
        requireRange(material.baseColorSrgb.x, 0.0F, 1.0F, path, "baseColorSRGB");
        requireRange(material.baseColorSrgb.y, 0.0F, 1.0F, path, "baseColorSRGB");
        requireRange(material.baseColorSrgb.z, 0.0F, 1.0F, path, "baseColorSRGB");
        requireRange(material.metallic, 0.0F, 1.0F, path, "metallic");
        requireRange(material.roughness, 0.0F, 1.0F, path, "roughness");
        requireRange(material.baseColorVariation, 0.0F, 1.0F, path, "baseColorVariation");
        requireRange(material.roughnessVariation, 0.0F, 1.0F, path, "roughnessVariation");
        if (material.absorption.x < 0.0F || material.absorption.y < 0.0F || material.absorption.z < 0.0F || material.emission.x < 0.0F || material.emission.y < 0.0F || material.emission.z < 0.0F) {
            invalid(path, "absorption 与 emission 不得为负数");
        }
        if (material.type == MaterialType::Dielectric) {
            if (!(material.ior > 1.0F && material.ior <= 3.0F) || material.metallic != 0.0F || material.roughness != 0.0F) invalid(path, material.id + " 不满足 dielectric 约束");
        } else {
            if (material.ior != 1.0F || !zero(material.absorption)) invalid(path, material.id + " 的 ior 或 absorption 不满足非 dielectric 约束");
        }
        if (material.type != MaterialType::Emissive && !zero(material.emission)) invalid(path, material.id + " 的非 emissive emission 必须为零");
        if (material.type == MaterialType::Diffuse && material.metallic != 0.0F) invalid(path, material.id + " 的 diffuse metallic 必须为零");
        if (material.type == MaterialType::RoughMetal && (material.metallic != 1.0F || material.roughness <= 0.0F)) invalid(path, material.id + " 不满足 rough_metal 约束");
        if (material.type == MaterialType::Mirror && (material.metallic != 1.0F || material.roughness != 0.0F)) invalid(path, material.id + " 不满足 mirror 约束");
        if ((material.type == MaterialType::Mirror || material.type == MaterialType::Dielectric || material.type == MaterialType::Emissive) &&
            (material.pattern != Pattern::Solid || material.baseColorVariation != 0.0F || material.roughnessVariation != 0.0F)) {
            invalid(path, material.id + " 必须使用无 variation 的 solid 纹理");
        }
        if (!result.indexById.emplace(material.id, static_cast<std::uint32_t>(result.materials.size())).second) invalid(path, "材质 ID 重复：" + material.id);
        result.materials.push_back(material);
    }

    for (std::uint32_t index = 0; index < result.materials.size(); ++index) {
        const Material& material = result.materials[index];
        GpuMaterial gpu{};
        gpu.baseColorType = {material.baseColorSrgb.x, material.baseColorSrgb.y, material.baseColorSrgb.z, static_cast<float>(material.type)};
        gpu.emissionIor = {material.emission.x, material.emission.y, material.emission.z, material.ior};
        gpu.absorptionRoughness = {material.absorption.x, material.absorption.y, material.absorption.z, material.roughness};
        gpu.textureLayer = index;
        gpu.materialType = static_cast<std::uint32_t>(material.type);
        result.gpuMaterials.push_back(gpu);
    }
    buildBaseMip(result);
    buildRemainingMips(result);
    return result;
}

} // 命名空间 arena
