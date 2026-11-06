#include "materials.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <sstream>
#include <unordered_set>

namespace arena {
namespace {

using Json = nlohmann::json;

[[nodiscard]] float requireFiniteNumber(const Json& object, const char* field)
{
    if (!object.contains(field) || !object.at(field).is_number()) {
        throw Error(std::string("材质字段缺失或类型错误：") + field);
    }
    const double value = object.at(field).get<double>();
    if (!std::isfinite(value) || value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value > static_cast<double>(std::numeric_limits<float>::max())) {
        throw Error(std::string("材质字段不是有限 float：") + field);
    }
    return static_cast<float>(value);
}

[[nodiscard]] std::array<float, 3> requireVec3(const Json& object, const char* field)
{
    if (!object.contains(field) || !object.at(field).is_array() || object.at(field).size() != 3U) {
        throw Error(std::string("材质字段必须是 3 元数组：") + field);
    }
    std::array<float, 3> values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto& element = object.at(field).at(index);
        if (!element.is_number()) {
            throw Error(std::string("材质向量分量类型错误：") + field);
        }
        const double value = element.get<double>();
        if (!std::isfinite(value) || value < -static_cast<double>(std::numeric_limits<float>::max()) ||
            value > static_cast<double>(std::numeric_limits<float>::max())) {
            throw Error(std::string("材质向量包含非有限分量：") + field);
        }
        values[index] = static_cast<float>(value);
    }
    return values;
}

void requireRange(const float value, const float low, const float high, const std::string_view label)
{
    if (value < low || value > high) {
        throw Error(std::string(label) + " 超出范围 [" + std::to_string(low) + ", " +
                    std::to_string(high) + "]");
    }
}

[[nodiscard]] MaterialType parseMaterialType(const std::string& value)
{
    if (value == "diffuse") {
        return MaterialType::diffuse;
    }
    if (value == "rough_metal") {
        return MaterialType::roughMetal;
    }
    if (value == "mirror") {
        return MaterialType::mirror;
    }
    if (value == "dielectric") {
        return MaterialType::dielectric;
    }
    if (value == "emissive") {
        return MaterialType::emissive;
    }
    throw Error("未知材质 type：" + value);
}

[[nodiscard]] PatternType parsePattern(const std::string& value)
{
    if (value == "solid") {
        return PatternType::solid;
    }
    if (value == "stone_noise") {
        return PatternType::stoneNoise;
    }
    if (value == "wood_grain") {
        return PatternType::woodGrain;
    }
    throw Error("未知材质 pattern：" + value);
}

[[nodiscard]] bool nearlyEqual(const float a, const float b) noexcept
{
    return std::abs(a - b) <= 1.0e-6F;
}

void validateSemanticConstraints(const Material& material)
{
    const auto allZero = [](const std::array<float, 3>& value) {
        return nearlyEqual(value[0], 0.0F) && nearlyEqual(value[1], 0.0F) &&
               nearlyEqual(value[2], 0.0F);
    };
    if (material.type == MaterialType::diffuse && !nearlyEqual(material.metallic, 0.0F)) {
        throw Error("diffuse 材质 metallic 必须为 0：" + material.id);
    }
    if (material.type == MaterialType::roughMetal &&
        (!nearlyEqual(material.metallic, 1.0F) || !(material.roughness > 0.0F))) {
        throw Error("rough_metal 材质必须 metallic=1 且 roughness>0：" + material.id);
    }
    if (material.type == MaterialType::mirror &&
        (!nearlyEqual(material.metallic, 1.0F) || !nearlyEqual(material.roughness, 0.0F))) {
        throw Error("mirror 材质必须 metallic=1 且 roughness=0：" + material.id);
    }
    if (material.type == MaterialType::dielectric &&
        (!nearlyEqual(material.metallic, 0.0F) || !nearlyEqual(material.roughness, 0.0F) ||
         !(material.ior > 1.0F && material.ior <= 3.0F))) {
        throw Error("dielectric 材质必须 metallic=0、roughness=0、ior∈(1,3]：" + material.id);
    }
    if (material.type != MaterialType::dielectric && !nearlyEqual(material.ior, 1.0F)) {
        throw Error("非 dielectric 材质 ior 必须为 1：" + material.id);
    }
    if (material.type != MaterialType::dielectric && !allZero(material.absorption)) {
        throw Error("非 dielectric 材质 absorption 必须为零：" + material.id);
    }
    if (material.type != MaterialType::emissive && !allZero(material.emission)) {
        throw Error("非 emissive 材质 emission 必须为零：" + material.id);
    }
    if ((material.type == MaterialType::mirror || material.type == MaterialType::dielectric ||
         material.type == MaterialType::emissive) &&
        (material.pattern != PatternType::solid || !nearlyEqual(material.baseColorVariation, 0.0F) ||
         !nearlyEqual(material.roughnessVariation, 0.0F))) {
        throw Error("mirror/dielectric/emissive 必须使用 solid 且 variation 为零：" + material.id);
    }
}

[[nodiscard]] std::uint32_t hash32(std::uint32_t value) noexcept
{
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] float random01(const std::int32_t x, const std::int32_t y, const std::uint32_t seed) noexcept
{
    const std::uint32_t ux = std::bit_cast<std::uint32_t>(x);
    const std::uint32_t uy = std::bit_cast<std::uint32_t>(y);
    const std::uint32_t bits = hash32(ux * 0x9e3779b9U ^ uy * 0x85ebca6bU ^ seed);
    return static_cast<float>(bits & 0x00ffffffU) / 16777216.0F;
}

[[nodiscard]] float smooth(const float value) noexcept
{
    return value * value * (3.0F - 2.0F * value);
}

[[nodiscard]] float valueNoise(const float x, const float y, const std::uint32_t seed) noexcept
{
    const auto x0 = static_cast<std::int32_t>(std::floor(x));
    const auto y0 = static_cast<std::int32_t>(std::floor(y));
    const float tx = smooth(x - static_cast<float>(x0));
    const float ty = smooth(y - static_cast<float>(y0));
    const float n00 = random01(x0, y0, seed);
    const float n10 = random01(x0 + 1, y0, seed);
    const float n01 = random01(x0, y0 + 1, seed);
    const float n11 = random01(x0 + 1, y0 + 1, seed);
    const float a = n00 + (n10 - n00) * tx;
    const float b = n01 + (n11 - n01) * tx;
    return a + (b - a) * ty;
}

[[nodiscard]] float patternValue(const Material& material, const float u, const float v) noexcept
{
    if (material.pattern == PatternType::solid) {
        return 0.0F;
    }
    if (material.pattern == PatternType::stoneNoise) {
        float amplitude = 0.5714286F;
        float frequency = 3.0F;
        float value = 0.0F;
        for (std::uint32_t octave = 0; octave < 3U; ++octave) {
            value += amplitude * (valueNoise(u * frequency, v * frequency,
                                             material.textureSeed + octave * 977U) - 0.5F) * 2.0F;
            frequency *= 2.0F;
            amplitude *= 0.5F;
        }
        return clamp(value, -1.0F, 1.0F);
    }
    const float perturbation = (valueNoise(u * 2.0F, v * 2.0F, material.textureSeed) - 0.5F) * 0.55F;
    const float rings = std::sin((u * 11.0F + perturbation) * 2.0F * std::numbers::pi_v<float>);
    return clamp(rings * 0.72F + (valueNoise(u * 5.0F, v * 5.0F,
                                             material.textureSeed ^ 0x51ed270bU) - 0.5F) * 0.28F,
                 -1.0F, 1.0F);
}

[[nodiscard]] float srgbToLinear(const float value) noexcept
{
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linearToSrgb(const float value) noexcept
{
    const float clamped = clamp(value, 0.0F, 1.0F);
    return clamped <= 0.0031308F ? clamped * 12.92F : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] std::uint8_t quantize(const float value) noexcept
{
    return static_cast<std::uint8_t>(std::lround(clamp(value, 0.0F, 1.0F) * 255.0F));
}

} // namespace

MaterialLibrary MaterialLibrary::load(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream) {
        throw Error("无法打开材质配置：" + path.string());
    }

    Json root;
    try {
        stream >> root;
    } catch (const Json::exception& exception) {
        throw Error("材质 JSON 解析失败：" + std::string(exception.what()));
    }
    if (!root.is_object()) {
        throw Error("材质配置根节点必须是对象");
    }
    if (!root.contains("schemaVersion") || !root.at("schemaVersion").is_number_unsigned() ||
        root.at("schemaVersion").get<std::uint32_t>() != 1U) {
        throw Error("材质 schemaVersion 缺失、类型错误或不受支持（要求 1）");
    }
    if (!root.contains("textureSize") || !root.at("textureSize").is_number_unsigned()) {
        throw Error("材质 textureSize 缺失或类型错误");
    }
    const std::uint32_t textureSize = root.at("textureSize").get<std::uint32_t>();
    if (textureSize < 16U || textureSize > 1024U || !std::has_single_bit(textureSize)) {
        throw Error("textureSize 必须是 [16,1024] 内的 2 的整数次幂");
    }
    if (!root.contains("materials") || !root.at("materials").is_array() ||
        root.at("materials").empty()) {
        throw Error("materials 必须是非空数组");
    }

    MaterialLibrary library;
    library.textureSize_ = textureSize;
    for (const Json& entry : root.at("materials")) {
        if (!entry.is_object()) {
            throw Error("materials 元素必须是对象");
        }
        const auto requireString = [&entry](const char* field) -> std::string {
            if (!entry.contains(field) || !entry.at(field).is_string()) {
                throw Error(std::string("材质字段缺失或类型错误：") + field);
            }
            return entry.at(field).get<std::string>();
        };

        Material material;
        material.id = requireString("id");
        if (material.id.empty()) {
            throw Error("材质 id 不能为空");
        }
        if (library.indices_.contains(material.id)) {
            throw Error("材质 id 重复：" + material.id);
        }
        material.type = parseMaterialType(requireString("type"));
        material.baseColorSrgb = requireVec3(entry, "baseColorSRGB");
        material.metallic = requireFiniteNumber(entry, "metallic");
        material.roughness = requireFiniteNumber(entry, "roughness");
        material.ior = requireFiniteNumber(entry, "ior");
        material.absorption = requireVec3(entry, "absorption");
        material.emission = requireVec3(entry, "emission");
        material.pattern = parsePattern(requireString("pattern"));
        if (!entry.contains("textureSeed") || !entry.at("textureSeed").is_number_unsigned()) {
            throw Error("textureSeed 缺失、不是整数或超出 uint32 范围：" + material.id);
        }
        material.textureSeed = entry.at("textureSeed").get<std::uint32_t>();
        material.baseColorVariation = requireFiniteNumber(entry, "baseColorVariation");
        material.roughnessVariation = requireFiniteNumber(entry, "roughnessVariation");

        for (const float component : material.baseColorSrgb) {
            requireRange(component, 0.0F, 1.0F, material.id + ".baseColorSRGB");
        }
        requireRange(material.metallic, 0.0F, 1.0F, material.id + ".metallic");
        requireRange(material.roughness, 0.0F, 1.0F, material.id + ".roughness");
        for (const float component : material.absorption) {
            if (component < 0.0F) {
                throw Error(material.id + ".absorption 不能为负");
            }
        }
        for (const float component : material.emission) {
            if (component < 0.0F) {
                throw Error(material.id + ".emission 不能为负");
            }
        }
        requireRange(material.baseColorVariation, 0.0F, 1.0F,
                     material.id + ".baseColorVariation");
        requireRange(material.roughnessVariation, 0.0F, 1.0F,
                     material.id + ".roughnessVariation");
        validateSemanticConstraints(material);

        const auto index = static_cast<std::uint32_t>(library.materials_.size());
        library.indices_.emplace(material.id, index);
        library.materials_.push_back(std::move(material));
    }
    return library;
}

std::uint32_t MaterialLibrary::indexOf(const std::string& id) const
{
    const auto iterator = indices_.find(id);
    if (iterator == indices_.end()) {
        throw Error("场景引用了不存在的材质 id：" + id);
    }
    return iterator->second;
}

const Material& MaterialLibrary::at(const std::uint32_t index) const
{
    if (index >= materials_.size()) {
        throw Error("材质索引越界");
    }
    return materials_[index];
}

MaterialTextureSet MaterialLibrary::generateTextures() const
{
    MaterialTextureSet result;
    result.baseSize = textureSize_;
    result.mipLevels = static_cast<std::uint32_t>(std::bit_width(textureSize_));

    const std::size_t layerCount = materials_.size();
    std::uint32_t width = textureSize_;
    std::uint32_t height = textureSize_;
    result.colorMips.reserve(result.mipLevels);
    result.roughnessMips.reserve(result.mipLevels);

    TextureMip colorBase{width, height,
                         std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height *
                                                   layerCount * 4U)};
    TextureMip roughBase{width, height,
                         std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height *
                                                   layerCount)};
    for (std::size_t layer = 0; layer < layerCount; ++layer) {
        const Material& material = materials_[layer];
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
                const float modulation = patternValue(material, u, v);
                const std::size_t pixel = (layer * static_cast<std::size_t>(width) * height +
                                           static_cast<std::size_t>(y) * width + x);
                for (std::size_t component = 0; component < 3U; ++component) {
                    const float encoded = clamp(material.baseColorSrgb[component] *
                                                    (1.0F + material.baseColorVariation * modulation),
                                                0.0F, 1.0F);
                    colorBase.texels[pixel * 4U + component] = quantize(encoded);
                }
                colorBase.texels[pixel * 4U + 3U] = 255U;
                const float roughness = clamp(material.roughness +
                                                  material.roughnessVariation * modulation,
                                              0.0F, 1.0F);
                roughBase.texels[pixel] = quantize(roughness);
            }
        }
    }
    result.colorMips.push_back(std::move(colorBase));
    result.roughnessMips.push_back(std::move(roughBase));

    for (std::uint32_t mip = 1; mip < result.mipLevels; ++mip) {
        const TextureMip& previousColor = result.colorMips.back();
        const TextureMip& previousRough = result.roughnessMips.back();
        width = std::max(1U, previousColor.width / 2U);
        height = std::max(1U, previousColor.height / 2U);
        TextureMip color{width, height,
                         std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height *
                                                   layerCount * 4U)};
        TextureMip rough{width, height,
                         std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height *
                                                   layerCount)};
        for (std::size_t layer = 0; layer < layerCount; ++layer) {
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint32_t sx = x * 2U;
                    const std::uint32_t sy = y * 2U;
                    const std::size_t destination = layer * static_cast<std::size_t>(width) * height +
                                                    static_cast<std::size_t>(y) * width + x;
                    std::array<float, 3> linear{};
                    float roughness = 0.0F;
                    for (std::uint32_t oy = 0; oy < 2U; ++oy) {
                        for (std::uint32_t ox = 0; ox < 2U; ++ox) {
                            const std::uint32_t px = std::min(sx + ox, previousColor.width - 1U);
                            const std::uint32_t py = std::min(sy + oy, previousColor.height - 1U);
                            const std::size_t source =
                                layer * static_cast<std::size_t>(previousColor.width) * previousColor.height +
                                static_cast<std::size_t>(py) * previousColor.width + px;
                            for (std::size_t component = 0; component < 3U; ++component) {
                                linear[component] += srgbToLinear(
                                    static_cast<float>(previousColor.texels[source * 4U + component]) / 255.0F);
                            }
                            roughness += static_cast<float>(previousRough.texels[source]) / 255.0F;
                        }
                    }
                    for (std::size_t component = 0; component < 3U; ++component) {
                        color.texels[destination * 4U + component] =
                            quantize(linearToSrgb(linear[component] * 0.25F));
                    }
                    color.texels[destination * 4U + 3U] = 255U;
                    rough.texels[destination] = quantize(roughness * 0.25F);
                }
            }
        }
        result.colorMips.push_back(std::move(color));
        result.roughnessMips.push_back(std::move(rough));
    }
    return result;
}

std::vector<GpuMaterial> makeGpuMaterials(const MaterialLibrary& library)
{
    std::vector<GpuMaterial> result;
    result.reserve(library.materials().size());
    for (std::uint32_t index = 0; index < library.materials().size(); ++index) {
        const Material& material = library.materials()[index];
        GpuMaterial gpu;
        gpu.baseMetal = {material.baseColorSrgb[0], material.baseColorSrgb[1],
                         material.baseColorSrgb[2], material.metallic};
        gpu.absorptionIor = {material.absorption[0], material.absorption[1],
                             material.absorption[2], material.ior};
        gpu.emissionRoughness = {material.emission[0], material.emission[1],
                                 material.emission[2], material.roughness};
        gpu.typeTexture = {static_cast<std::uint32_t>(material.type), index,
                           library.textureSize(), 0U};
        result.push_back(gpu);
    }
    return result;
}

} // namespace arena
