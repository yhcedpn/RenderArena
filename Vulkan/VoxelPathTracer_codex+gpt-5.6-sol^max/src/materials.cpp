#include "materials.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace vpt {
namespace {

using Json = nlohmann::json;

[[noreturn]] void materialError(const std::filesystem::path& path, const std::string& message) {
    throw std::runtime_error("材质配置错误 " + path.string() + ": " + message);
}

float requireFiniteNumber(const Json& object, std::string_view key,
                          const std::filesystem::path& path) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        materialError(path, "字段 '" + std::string(key) + "' 必须是数值");
    }
    const double value = it->get<double>();
    if (!std::isfinite(value) || value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value > static_cast<double>(std::numeric_limits<float>::max())) {
        materialError(path, "字段 '" + std::string(key) + "' 必须是有限 float 数值");
    }
    return static_cast<float>(value);
}

float requireRange(const Json& object, std::string_view key, float lo, float hi,
                   const std::filesystem::path& path) {
    const float value = requireFiniteNumber(object, key, path);
    if (value < lo || value > hi) {
        materialError(path, "字段 '" + std::string(key) + "' 超出允许范围");
    }
    return value;
}

Vec3 requireVec3(const Json& object, std::string_view key, float lo, float hi,
                 const std::filesystem::path& path) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array() || it->size() != 3U) {
        materialError(path, "字段 '" + std::string(key) + "' 必须是三个数的数组");
    }
    Vec3 out{};
    float* components[] = {&out.x, &out.y, &out.z};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!(*it)[i].is_number()) {
            materialError(path, "字段 '" + std::string(key) + "' 含非数值分量");
        }
        const double value = (*it)[i].get<double>();
        if (!std::isfinite(value) || value < lo || value > hi) {
            materialError(path, "字段 '" + std::string(key) + "' 含越界或非有限分量");
        }
        *components[i] = static_cast<float>(value);
    }
    return out;
}

std::string requireString(const Json& object, std::string_view key,
                          const std::filesystem::path& path) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        materialError(path, "字段 '" + std::string(key) + "' 必须是字符串");
    }
    return it->get<std::string>();
}

MaterialType parseType(const std::string& value, const std::filesystem::path& path) {
    if (value == "diffuse") return MaterialType::Diffuse;
    if (value == "rough_metal") return MaterialType::RoughMetal;
    if (value == "mirror") return MaterialType::Mirror;
    if (value == "dielectric") return MaterialType::Dielectric;
    if (value == "emissive") return MaterialType::Emissive;
    materialError(path, "未知材质类型 '" + value + "'");
}

Pattern parsePattern(const std::string& value, const std::filesystem::path& path) {
    if (value == "solid") return Pattern::Solid;
    if (value == "stone_noise") return Pattern::StoneNoise;
    if (value == "wood_grain") return Pattern::WoodGrain;
    materialError(path, "未知纹理模式 '" + value + "'");
}

float srgbToLinear(float value) {
    return value <= 0.04045F ? value / 12.92F
                             : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float linearToSrgb(float value) {
    value = std::clamp(value, 0.0F, 1.0F);
    return value <= 0.0031308F ? value * 12.92F
                              : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

float fade(float t) { return t * t * (3.0F - 2.0F * t); }

float randomCorner(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    const std::uint32_t ux = static_cast<std::uint32_t>(x);
    const std::uint32_t uy = static_cast<std::uint32_t>(y);
    return static_cast<float>(hash32(seed ^ hash32(ux * 0x9e3779b9U) ^
                                     hash32(uy * 0x85ebca6bU)) & 0x00ffffffU) /
           static_cast<float>(0x01000000U);
}

float valueNoise(float x, float y, std::uint32_t seed) {
    const auto ix = static_cast<std::int32_t>(std::floor(x));
    const auto iy = static_cast<std::int32_t>(std::floor(y));
    const float fx = fade(x - static_cast<float>(ix));
    const float fy = fade(y - static_cast<float>(iy));
    const float a = std::lerp(randomCorner(ix, iy, seed), randomCorner(ix + 1, iy, seed), fx);
    const float b =
        std::lerp(randomCorner(ix, iy + 1, seed), randomCorner(ix + 1, iy + 1, seed), fx);
    return std::lerp(a, b, fy);
}

float patternValue(Pattern pattern, float u, float v, std::uint32_t seed) {
    if (pattern == Pattern::Solid) return 0.0F;
    if (pattern == Pattern::StoneNoise) {
        float sum = 0.0F;
        float norm = 0.0F;
        float amplitude = 1.0F;
        float frequency = 3.0F;
        for (std::uint32_t octave = 0; octave < 4; ++octave) {
            sum += amplitude * valueNoise(u * frequency, v * frequency,
                                          seed + octave * 0x9e3779b9U);
            norm += amplitude;
            amplitude *= 0.5F;
            frequency *= 2.0F;
        }
        return sum / norm * 2.0F - 1.0F;
    }
    const float low = valueNoise(u * 2.0F, v * 2.0F, seed);
    const float warped = u * 18.0F + (low - 0.5F) * 4.0F;
    return std::sin(warped * 2.0F * std::numbers::pi_v<float>);
}

std::uint8_t quantize(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

std::vector<TextureMip> buildBaseColorMips(const Material& material, std::uint32_t size) {
    std::vector<TextureMip> result;
    TextureMip base{size, size, std::vector<std::uint8_t>(size * size * 4U)};
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
            const float modulation =
                1.0F + patternValue(material.pattern, u, v, material.textureSeed) *
                           material.baseColorVariation;
            const Vec3 color = clamp(material.baseColorSrgb * modulation, 0.0F, 1.0F);
            const std::size_t offset = (static_cast<std::size_t>(y) * size + x) * 4U;
            base.texels[offset] = quantize(color.x);
            base.texels[offset + 1U] = quantize(color.y);
            base.texels[offset + 2U] = quantize(color.z);
            base.texels[offset + 3U] = 255U;
        }
    }
    result.push_back(std::move(base));
    while (result.back().width > 1U || result.back().height > 1U) {
        const TextureMip& source = result.back();
        const std::uint32_t width = std::max(1U, source.width / 2U);
        const std::uint32_t height = std::max(1U, source.height / 2U);
        TextureMip target{width, height, std::vector<std::uint8_t>(width * height * 4U)};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                Vec3 linear{};
                for (std::uint32_t oy = 0; oy < 2U; ++oy) {
                    for (std::uint32_t ox = 0; ox < 2U; ++ox) {
                        const std::uint32_t sx = std::min(source.width - 1U, x * 2U + ox);
                        const std::uint32_t sy = std::min(source.height - 1U, y * 2U + oy);
                        const std::size_t so =
                            (static_cast<std::size_t>(sy) * source.width + sx) * 4U;
                        linear += Vec3{srgbToLinear(source.texels[so] / 255.0F),
                                       srgbToLinear(source.texels[so + 1U] / 255.0F),
                                       srgbToLinear(source.texels[so + 2U] / 255.0F)};
                    }
                }
                linear = linear / 4.0F;
                const std::size_t to = (static_cast<std::size_t>(y) * width + x) * 4U;
                target.texels[to] = quantize(linearToSrgb(linear.x));
                target.texels[to + 1U] = quantize(linearToSrgb(linear.y));
                target.texels[to + 2U] = quantize(linearToSrgb(linear.z));
                target.texels[to + 3U] = 255U;
            }
        }
        result.push_back(std::move(target));
    }
    return result;
}

std::vector<TextureMip> buildRoughnessMips(const Material& material, std::uint32_t size) {
    std::vector<TextureMip> result;
    TextureMip base{size, size, std::vector<std::uint8_t>(size * size)};
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
            const float noise = patternValue(material.pattern, u, v,
                                             material.textureSeed ^ 0xa511e9b3U);
            const float roughness = std::clamp(
                material.roughness + noise * material.roughnessVariation, 0.0F, 1.0F);
            base.texels[static_cast<std::size_t>(y) * size + x] = quantize(roughness);
        }
    }
    result.push_back(std::move(base));
    while (result.back().width > 1U || result.back().height > 1U) {
        const TextureMip& source = result.back();
        const std::uint32_t width = std::max(1U, source.width / 2U);
        const std::uint32_t height = std::max(1U, source.height / 2U);
        TextureMip target{width, height, std::vector<std::uint8_t>(width * height)};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                float sum = 0.0F;
                for (std::uint32_t oy = 0; oy < 2U; ++oy) {
                    for (std::uint32_t ox = 0; ox < 2U; ++ox) {
                        const std::uint32_t sx = std::min(source.width - 1U, x * 2U + ox);
                        const std::uint32_t sy = std::min(source.height - 1U, y * 2U + oy);
                        sum += source.texels[static_cast<std::size_t>(sy) * source.width + sx] /
                               255.0F;
                    }
                }
                target.texels[static_cast<std::size_t>(y) * width + x] = quantize(sum / 4.0F);
            }
        }
        result.push_back(std::move(target));
    }
    return result;
}

bool isZero(Vec3 value) { return value.x == 0.0F && value.y == 0.0F && value.z == 0.0F; }

void validateSemanticConstraints(const Material& material,
                                 const std::filesystem::path& path) {
    const bool solidOnly = material.type == MaterialType::Mirror ||
                           material.type == MaterialType::Dielectric ||
                           material.type == MaterialType::Emissive;
    if (solidOnly && (material.pattern != Pattern::Solid || material.baseColorVariation != 0.0F ||
                      material.roughnessVariation != 0.0F)) {
        materialError(path, "mirror、dielectric 与 emissive 必须使用无 variation 的 solid 纹理");
    }
    if (material.type == MaterialType::Diffuse && material.metallic != 0.0F) {
        materialError(path, "diffuse 的 metallic 必须为 0");
    }
    if (material.type == MaterialType::RoughMetal &&
        (material.metallic != 1.0F || material.roughness <= 0.0F)) {
        materialError(path, "rough_metal 必须 metallic=1 且 roughness>0");
    }
    if (material.type == MaterialType::Mirror &&
        (material.metallic != 1.0F || material.roughness != 0.0F)) {
        materialError(path, "mirror 必须 metallic=1 且 roughness=0");
    }
    if (material.type == MaterialType::Dielectric &&
        (material.metallic != 0.0F || material.roughness != 0.0F || material.ior <= 1.0F ||
         material.ior > 3.0F)) {
        materialError(path, "dielectric 必须 metallic=0、roughness=0 且 1<ior<=3");
    }
    if (material.type != MaterialType::Dielectric && material.ior != 1.0F) {
        materialError(path, "非 dielectric 的 ior 必须为 1");
    }
    if (material.type != MaterialType::Dielectric && !isZero(material.absorption)) {
        materialError(path, "非 dielectric 的 absorption 必须为零");
    }
    if (material.type != MaterialType::Emissive && !isZero(material.emission)) {
        materialError(path, "非 emissive 的 emission 必须为零");
    }
}

} // namespace

std::uint32_t MaterialSet::require(std::string_view id) const {
    const auto it = indexById.find(std::string(id));
    if (it == indexById.end()) {
        throw std::runtime_error("场景引用了 materials.json 中不存在的材质 ID: " +
                                 std::string(id));
    }
    return it->second;
}

MaterialSet loadMaterials(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("无法打开必需材质文件: " + path.string());
    }

    Json root;
    try {
        stream >> root;
    } catch (const std::exception& exception) {
        materialError(path, std::string("JSON 解析失败: ") + exception.what());
    }
    if (!root.is_object()) materialError(path, "根节点必须是对象");
    if (!root.contains("schemaVersion") || !root["schemaVersion"].is_number_integer() ||
        root["schemaVersion"].get<int>() != 1) {
        materialError(path, "schemaVersion 必须是 1");
    }
    if (!root.contains("textureSize") || !root["textureSize"].is_number_unsigned()) {
        materialError(path, "textureSize 必须是无符号整数");
    }
    const auto size64 = root["textureSize"].get<std::uint64_t>();
    if (size64 < 16U || size64 > 1024U || (size64 & (size64 - 1U)) != 0U) {
        materialError(path, "textureSize 必须是 [16,1024] 内的 2 的整数次幂");
    }
    if (!root.contains("materials") || !root["materials"].is_array() ||
        root["materials"].empty()) {
        materialError(path, "materials 必须是非空数组");
    }

    MaterialSet result{};
    result.textureSize = static_cast<std::uint32_t>(size64);
    for (const Json& entry : root["materials"]) {
        if (!entry.is_object()) materialError(path, "materials 元素必须是对象");
        Material material{};
        material.id = requireString(entry, "id", path);
        if (material.id.empty()) materialError(path, "材质 id 不能为空");
        if (result.indexById.contains(material.id)) {
            materialError(path, "材质 id 重复: " + material.id);
        }
        material.type = parseType(requireString(entry, "type", path), path);
        material.baseColorSrgb = requireVec3(entry, "baseColorSRGB", 0.0F, 1.0F, path);
        material.metallic = requireRange(entry, "metallic", 0.0F, 1.0F, path);
        material.roughness = requireRange(entry, "roughness", 0.0F, 1.0F, path);
        material.ior = requireFiniteNumber(entry, "ior", path);
        material.absorption =
            requireVec3(entry, "absorption", 0.0F, std::numeric_limits<float>::max(), path);
        material.emission =
            requireVec3(entry, "emission", 0.0F, std::numeric_limits<float>::max(), path);
        material.pattern = parsePattern(requireString(entry, "pattern", path), path);
        if (!entry.contains("textureSeed") || !entry["textureSeed"].is_number_unsigned()) {
            materialError(path, "textureSeed 必须是 uint32 整数");
        }
        const std::uint64_t seed = entry["textureSeed"].get<std::uint64_t>();
        if (seed > std::numeric_limits<std::uint32_t>::max()) {
            materialError(path, "textureSeed 超出 uint32 范围");
        }
        material.textureSeed = static_cast<std::uint32_t>(seed);
        material.baseColorVariation =
            requireRange(entry, "baseColorVariation", 0.0F, 1.0F, path);
        material.roughnessVariation =
            requireRange(entry, "roughnessVariation", 0.0F, 1.0F, path);
        validateSemanticConstraints(material, path);

        result.indexById.emplace(material.id,
                                 static_cast<std::uint32_t>(result.materials.size()));
        result.materials.push_back(std::move(material));
    }

    result.baseColorMips.reserve(result.materials.size());
    result.roughnessMips.reserve(result.materials.size());
    for (const Material& material : result.materials) {
        result.baseColorMips.push_back(buildBaseColorMips(material, result.textureSize));
        result.roughnessMips.push_back(buildRoughnessMips(material, result.textureSize));
    }
    return result;
}

} // namespace vpt
