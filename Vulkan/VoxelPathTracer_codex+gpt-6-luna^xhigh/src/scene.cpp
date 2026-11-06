#include "scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <tuple>

#include <nlohmann/json.hpp>

namespace voxelpt
{
namespace
{
using json = nlohmann::json;
using Cell = std::tuple<int, int, int>;

[[nodiscard]] bool finite(float value)
{
    return std::isfinite(value);
}

[[nodiscard]] float unit(float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] float srgbToLinear(float value)
{
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linearToSrgb(float value)
{
    value = std::max(value, 0.0F);
    return value <= 0.0031308F ? 12.92F * value : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] std::uint32_t hash32(std::uint32_t value)
{
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] float random01(std::uint32_t seed)
{
    return static_cast<float>(hash32(seed) >> 8U) * (1.0F / 16777216.0F);
}

[[nodiscard]] float smooth(float value)
{
    return value * value * (3.0F - 2.0F * value);
}

[[nodiscard]] float valueNoise(float x, float y, std::uint32_t seed)
{
    // 四个格点的确定性哈希值经平滑双线性插值，供石材多尺度噪声和木纹扰动复用。
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const float fx = smooth(x - static_cast<float>(ix));
    const float fy = smooth(y - static_cast<float>(iy));
    const auto lattice = [seed](int px, int py)
    {
        const auto sx = static_cast<std::uint32_t>(px) * 0x9e3779b9U;
        const auto sy = static_cast<std::uint32_t>(py) * 0x85ebca6bU;
        return random01(seed ^ sx ^ sy);
    };
    const float a = std::lerp(lattice(ix, iy), lattice(ix + 1, iy), fx);
    const float b = std::lerp(lattice(ix, iy + 1), lattice(ix + 1, iy + 1), fx);
    return std::lerp(a, b, fy);
}

[[nodiscard]] float patternValue(const Material& material, float u, float v)
{
    if (material.pattern == Pattern::solid)
    {
        return 0.5F;
    }
    if (material.pattern == Pattern::stoneNoise)
    {
        const float broad = valueNoise(u * 3.0F, v * 3.0F, material.textureSeed);
        const float medium = valueNoise(u * 9.0F, v * 9.0F, material.textureSeed + 0x68bc21ebU);
        const float fine = valueNoise(u * 27.0F, v * 27.0F, material.textureSeed + 0x02e5be93U);
        return (0.55F * broad + 0.30F * medium + 0.15F * fine);
    }

    const float warp = valueNoise(u * 4.0F, v * 5.0F, material.textureSeed);
    const float grain = 0.5F + 0.5F * std::sin((v * 18.0F + (warp - 0.5F) * 1.8F) * 6.28318530718F);
    const float broad = valueNoise(u * 3.0F, v * 4.0F, material.textureSeed + 0x9e3779b9U);
    return unit(0.72F * grain + 0.28F * broad);
}

[[nodiscard]] Color3 requiredColor(const json& value, const std::string& key)
{
    if (!value.contains(key) || !value.at(key).is_array() || value.at(key).size() != 3)
    {
        throw std::runtime_error("字段 '" + key + "' 必须是包含 3 个数的数组");
    }
    Color3 result{};
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const auto& component = value.at(key).at(i);
        if (!component.is_number())
        {
            throw std::runtime_error("字段 '" + key + "' 的分量必须是数值");
        }
        result[i] = component.get<float>();
        if (!finite(result[i]))
        {
            throw std::runtime_error("字段 '" + key + "' 的分量必须有限");
        }
    }
    return result;
}

[[nodiscard]] float requiredNumber(const json& value, const std::string& key)
{
    if (!value.contains(key) || !value.at(key).is_number())
    {
        throw std::runtime_error("字段 '" + key + "' 必须是数值");
    }
    const float result = value.at(key).get<float>();
    if (!finite(result))
    {
        throw std::runtime_error("字段 '" + key + "' 必须有限");
    }
    return result;
}

[[nodiscard]] MaterialType parseType(const std::string& value)
{
    if (value == "diffuse") return MaterialType::diffuse;
    if (value == "rough_metal") return MaterialType::roughMetal;
    if (value == "mirror") return MaterialType::mirror;
    if (value == "dielectric") return MaterialType::dielectric;
    if (value == "emissive") return MaterialType::emissive;
    throw std::runtime_error("未知材质类型 '" + value + "'");
}

[[nodiscard]] Pattern parsePattern(const std::string& value)
{
    if (value == "solid") return Pattern::solid;
    if (value == "stone_noise") return Pattern::stoneNoise;
    if (value == "wood_grain") return Pattern::woodGrain;
    throw std::runtime_error("未知纹理模式 '" + value + "'");
}

[[nodiscard]] bool isZero(const Color3& value)
{
    return value[0] == 0.0F && value[1] == 0.0F && value[2] == 0.0F;
}

void validateMaterial(const Material& material)
{
    const auto inUnitRange = [](float value) { return value >= 0.0F && value <= 1.0F; };
    if (material.id.empty()) throw std::runtime_error("材质 id 不能为空");
    for (float component : material.baseColorSrgb)
    {
        if (!inUnitRange(component)) throw std::runtime_error("材质 '" + material.id + "' 的 baseColorSRGB 超出 [0,1]");
    }
    for (float component : material.absorption)
    {
        if (component < 0.0F) throw std::runtime_error("材质 '" + material.id + "' 的 absorption 不能为负数");
    }
    for (float component : material.emission)
    {
        if (component < 0.0F) throw std::runtime_error("材质 '" + material.id + "' 的 emission 不能为负数");
    }
    if (!inUnitRange(material.metallic) || !inUnitRange(material.roughness) ||
        !inUnitRange(material.baseColorVariation) || !inUnitRange(material.roughnessVariation))
    {
        throw std::runtime_error("材质 '" + material.id + "' 的比例字段超出 [0,1]");
    }
    const bool isDielectric = material.type == MaterialType::dielectric;
    if (isDielectric ? !(material.ior > 1.0F && material.ior <= 3.0F) : material.ior != 1.0F)
    {
        throw std::runtime_error("材质 '" + material.id + "' 的 ior 与材质类型不符");
    }
    if (material.type == MaterialType::diffuse && material.metallic != 0.0F)
        throw std::runtime_error("diffuse 材质的 metallic 必须为 0");
    if (material.type == MaterialType::roughMetal && (material.metallic != 1.0F || material.roughness <= 0.0F))
        throw std::runtime_error("rough_metal 材质必须 metallic=1 且 roughness>0");
    if (material.type == MaterialType::mirror && (material.metallic != 1.0F || material.roughness != 0.0F))
        throw std::runtime_error("mirror 材质必须 metallic=1 且 roughness=0");
    if (isDielectric && (material.metallic != 0.0F || material.roughness != 0.0F))
        throw std::runtime_error("dielectric 材质必须 metallic=0 且 roughness=0");
    if (!isDielectric && !isZero(material.absorption))
        throw std::runtime_error("非 dielectric 材质的 absorption 必须为零");
    if (material.type != MaterialType::emissive && !isZero(material.emission))
        throw std::runtime_error("非 emissive 材质的 emission 必须为零");
    if (material.type == MaterialType::mirror || isDielectric || material.type == MaterialType::emissive)
    {
        if (material.pattern != Pattern::solid || material.baseColorVariation != 0.0F || material.roughnessVariation != 0.0F)
            throw std::runtime_error("mirror、dielectric、emissive 材质必须使用无变化的 solid 纹理");
    }
}

void addVoxel(std::map<Cell, std::uint32_t>& voxels, int x, int y, int z, std::uint32_t material)
{
    voxels[Cell{x, y, z}] = material;
}

[[nodiscard]] std::array<int, 3> coordinate(const Cell& cell)
{
    return {std::get<0>(cell), std::get<1>(cell), std::get<2>(cell)};
}

[[nodiscard]] std::array<int, 3> axisCoordinates(int axis, int plane, int uAxis, int u, int vAxis, int v)
{
    std::array<int, 3> result{};
    result[static_cast<std::size_t>(axis)] = plane;
    result[static_cast<std::size_t>(uAxis)] = u;
    result[static_cast<std::size_t>(vAxis)] = v;
    return result;
}

[[nodiscard]] std::array<float, 3> toFloat(const std::array<int, 3>& value)
{
    return {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])};
}

[[nodiscard]] TriangleMesh& meshFor(std::vector<TriangleMesh>& meshes, std::map<std::uint32_t, std::size_t>& lookup, std::uint32_t material)
{
    const auto found = lookup.find(material);
    if (found != lookup.end()) return meshes[found->second];
    const std::size_t index = meshes.size();
    meshes.push_back(TriangleMesh{material, 0U, {}, {}});
    lookup.emplace(material, index);
    return meshes.back();
}

void appendQuad(TriangleMesh& mesh, const std::array<std::array<float, 3>, 4>& positions,
                const std::array<float, 3>& normal, float width, float height)
{
    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto surfaceId = mesh.nextSurfaceId++;
    mesh.vertices.push_back(Vertex{positions[0], normal, {0.0F, 0.0F}, surfaceId});
    mesh.vertices.push_back(Vertex{positions[1], normal, {width, 0.0F}, surfaceId});
    mesh.vertices.push_back(Vertex{positions[2], normal, {width, height}, surfaceId});
    mesh.vertices.push_back(Vertex{positions[3], normal, {0.0F, height}, surfaceId});
    mesh.indices.insert(mesh.indices.end(), {base, base + 1U, base + 2U, base, base + 2U, base + 3U});
}

[[nodiscard]] std::vector<TriangleMesh> greedyMesh(const std::map<Cell, std::uint32_t>& voxels)
{
    // 按轴、朝向、平面、V、U 固定扫描；只有材质相同的暴露单元会被扩成矩形。
    std::vector<TriangleMesh> meshes;
    std::map<std::uint32_t, std::size_t> meshLookup;
    if (voxels.empty()) return meshes;

    std::array<int, 3> minimum{std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
    std::array<int, 3> maximum{std::numeric_limits<int>::min(), std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
    for (const auto& [cell, material] : voxels)
    {
        (void)material;
        const auto point = coordinate(cell);
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = std::min(minimum[axis], point[axis]);
            maximum[axis] = std::max(maximum[axis], point[axis]);
        }
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        const int uAxis = (axis + 1) % 3;
        const int vAxis = (axis + 2) % 3;
        for (int sign : {-1, 1})
        {
            for (int plane = minimum[static_cast<std::size_t>(axis)]; plane <= maximum[static_cast<std::size_t>(axis)] + 1; ++plane)
            {
                const int uCount = maximum[static_cast<std::size_t>(uAxis)] - minimum[static_cast<std::size_t>(uAxis)] + 1;
                const int vCount = maximum[static_cast<std::size_t>(vAxis)] - minimum[static_cast<std::size_t>(vAxis)] + 1;
                std::vector<int> mask(static_cast<std::size_t>(uCount * vCount), -1);
                for (int v = 0; v < vCount; ++v)
                {
                    for (int u = 0; u < uCount; ++u)
                    {
                        auto cell = axisCoordinates(axis, plane, uAxis,
                            minimum[static_cast<std::size_t>(uAxis)] + u,
                            vAxis, minimum[static_cast<std::size_t>(vAxis)] + v);
                        if (sign > 0) --cell[static_cast<std::size_t>(axis)];
                        const auto current = voxels.find(Cell{cell[0], cell[1], cell[2]});
                        if (current == voxels.end()) continue;
                        auto neighbour = cell;
                        neighbour[static_cast<std::size_t>(axis)] += sign;
                        if (voxels.contains(Cell{neighbour[0], neighbour[1], neighbour[2]})) continue;
                        mask[static_cast<std::size_t>(v * uCount + u)] = static_cast<int>(current->second);
                    }
                }

                for (int v = 0; v < vCount; ++v)
                {
                    for (int u = 0; u < uCount;)
                    {
                        const int material = mask[static_cast<std::size_t>(v * uCount + u)];
                        if (material < 0)
                        {
                            ++u;
                            continue;
                        }
                        int width = 1;
                        while (u + width < uCount && mask[static_cast<std::size_t>(v * uCount + u + width)] == material) ++width;
                        int height = 1;
                        bool canGrow = true;
                        while (v + height < vCount && canGrow)
                        {
                            for (int du = 0; du < width; ++du)
                            {
                                if (mask[static_cast<std::size_t>((v + height) * uCount + u + du)] != material)
                                {
                                    canGrow = false;
                                    break;
                                }
                            }
                            if (canGrow) ++height;
                        }
                        for (int dv = 0; dv < height; ++dv)
                        {
                            for (int du = 0; du < width; ++du)
                            {
                                mask[static_cast<std::size_t>((v + dv) * uCount + u + du)] = -1;
                            }
                        }

                        const int u0 = minimum[static_cast<std::size_t>(uAxis)] + u;
                        const int v0 = minimum[static_cast<std::size_t>(vAxis)] + v;
                        const auto p00 = toFloat(axisCoordinates(axis, plane, uAxis, u0, vAxis, v0));
                        const auto p10 = toFloat(axisCoordinates(axis, plane, uAxis, u0 + width, vAxis, v0));
                        const auto p11 = toFloat(axisCoordinates(axis, plane, uAxis, u0 + width, vAxis, v0 + height));
                        const auto p01 = toFloat(axisCoordinates(axis, plane, uAxis, u0, vAxis, v0 + height));
                        std::array<std::array<float, 3>, 4> points{p00, p10, p11, p01};
                        std::array<float, 3> normal{};
                        normal[static_cast<std::size_t>(axis)] = static_cast<float>(sign);
                        if (sign < 0) points = {p00, p01, p11, p10};
                        appendQuad(meshFor(meshes, meshLookup, static_cast<std::uint32_t>(material)), points, normal,
                                   static_cast<float>(width), static_cast<float>(height));
                        u += width;
                    }
                }
            }
        }
    }
    return meshes;
}

void appendBoxFace(TriangleMesh& mesh, int axis, float plane, float u0, float v0, float u1, float v1,
                   const std::array<float, 3>& normal)
{
    const int uAxis = (axis + 1) % 3;
    const int vAxis = (axis + 2) % 3;
    auto point = [axis, plane, uAxis, vAxis](float u, float v)
    {
        std::array<float, 3> p{};
        p[static_cast<std::size_t>(axis)] = plane;
        p[static_cast<std::size_t>(uAxis)] = u;
        p[static_cast<std::size_t>(vAxis)] = v;
        return p;
    };
    auto points = std::array{point(u0, v0), point(u1, v0), point(u1, v1), point(u0, v1)};
    if (normal[static_cast<std::size_t>(axis)] < 0.0F) points = {points[0], points[3], points[2], points[1]};
    appendQuad(mesh, points, normal, u1 - u0, v1 - v0);
}

void appendGlassBox(TriangleMesh& mesh)
{
    constexpr float x0 = 7.0F, x1 = 9.0F;
    constexpr float y0 = 0.125F, y1 = 2.125F;
    constexpr float z0 = 7.0F, z1 = 9.0F;
    appendBoxFace(mesh, 0, x0, y0, z0, y1, z1, {-1.0F, 0.0F, 0.0F});
    appendBoxFace(mesh, 0, x1, y0, z0, y1, z1, {1.0F, 0.0F, 0.0F});
    appendBoxFace(mesh, 1, y0, z0, x0, z1, x1, {0.0F, -1.0F, 0.0F});
    appendBoxFace(mesh, 1, y1, z0, x0, z1, x1, {0.0F, 1.0F, 0.0F});
    appendBoxFace(mesh, 2, z0, x0, y0, x1, y1, {0.0F, 0.0F, -1.0F});
    appendBoxFace(mesh, 2, z1, x0, y0, x1, y1, {0.0F, 0.0F, 1.0F});
}

[[nodiscard]] std::vector<TextureMip> generateTexture(const Material& material, std::uint32_t size)
{
    std::vector<TextureMip> mips;
    TextureMip base;
    base.width = size;
    base.height = size;
    base.baseColorSrgb.resize(static_cast<std::size_t>(size) * size);
    base.roughness.resize(static_cast<std::size_t>(size) * size);
    for (std::uint32_t y = 0; y < size; ++y)
    {
        for (std::uint32_t x = 0; x < size; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(size);
            const float v = static_cast<float>(y) / static_cast<float>(size);
            const float pattern = patternValue(material, u, v);
            const float modulation = (pattern * 2.0F - 1.0F) * material.baseColorVariation;
            const float roughModulation = (pattern * 2.0F - 1.0F) * material.roughnessVariation;
            const std::size_t index = static_cast<std::size_t>(y) * size + x;
            for (std::size_t channel = 0; channel < 3; ++channel)
                base.baseColorSrgb[index][channel] = unit(material.baseColorSrgb[channel] + modulation);
            base.roughness[index] = unit(material.roughness + roughModulation);
        }
    }
    mips.push_back(std::move(base));

    // 基础色先解码到线性空间平均再编码；粗糙度作为线性标量平均。
    while (mips.back().width > 1 || mips.back().height > 1)
    {
        const auto& source = mips.back();
        TextureMip target;
        target.width = std::max(1U, source.width / 2U);
        target.height = std::max(1U, source.height / 2U);
        target.baseColorSrgb.resize(static_cast<std::size_t>(target.width) * target.height);
        target.roughness.resize(static_cast<std::size_t>(target.width) * target.height);
        for (std::uint32_t y = 0; y < target.height; ++y)
        {
            for (std::uint32_t x = 0; x < target.width; ++x)
            {
                Color3 linearAverage{};
                float roughnessAverage = 0.0F;
                for (std::uint32_t dy = 0; dy < 2; ++dy)
                {
                    for (std::uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const auto sx = std::min(source.width - 1U, x * 2U + dx);
                        const auto sy = std::min(source.height - 1U, y * 2U + dy);
                        const std::size_t sourceIndex = static_cast<std::size_t>(sy) * source.width + sx;
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            linearAverage[channel] += srgbToLinear(source.baseColorSrgb[sourceIndex][channel]) * 0.25F;
                        roughnessAverage += source.roughness[sourceIndex] * 0.25F;
                    }
                }
                const std::size_t index = static_cast<std::size_t>(y) * target.width + x;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    target.baseColorSrgb[index][channel] = unit(linearToSrgb(linearAverage[channel]));
                target.roughness[index] = roughnessAverage;
            }
        }
        mips.push_back(std::move(target));
    }
    return mips;
}
}

std::uint32_t MaterialLibrary::requireId(const std::string& id) const
{
    const auto found = ids.find(id);
    if (found == ids.end()) throw std::runtime_error("materials.json 缺少场景所需材质 '" + id + "'");
    return found->second;
}

MaterialLibrary loadMaterials(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("无法读取材质配置：" + path.string());
    json root;
    try
    {
        input >> root;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("材质配置解析失败：" + std::string(error.what()));
    }
    if (!root.is_object() || !root.contains("schemaVersion") || !root.at("schemaVersion").is_number_integer() ||
        root.at("schemaVersion").get<int>() != 1)
    {
        throw std::runtime_error("materials.json 的 schemaVersion 缺失或不受支持");
    }
    if (!root.contains("textureSize") || !root.at("textureSize").is_number_unsigned())
        throw std::runtime_error("materials.json 的 textureSize 必须是无符号整数");
    MaterialLibrary library;
    library.textureSize = root.at("textureSize").get<std::uint32_t>();
    if (library.textureSize < 16U || library.textureSize > 1024U ||
        (library.textureSize & (library.textureSize - 1U)) != 0U)
    {
        throw std::runtime_error("materials.json 的 textureSize 必须是 16 至 1024 范围内的 2 的幂");
    }
    if (!root.contains("materials") || !root.at("materials").is_array())
        throw std::runtime_error("materials.json 的 materials 必须是数组");

    for (const auto& item : root.at("materials"))
    {
        if (!item.is_object()) throw std::runtime_error("materials 中每个元素必须是对象");
        for (const char* key : {"id", "type", "baseColorSRGB", "metallic", "roughness", "ior", "absorption", "emission", "pattern", "textureSeed", "baseColorVariation", "roughnessVariation"})
            if (!item.contains(key)) throw std::runtime_error("材质记录缺少必填字段 '" + std::string(key) + "'");
        if (!item.at("id").is_string() || !item.at("type").is_string() || !item.at("pattern").is_string())
            throw std::runtime_error("材质 id、type 和 pattern 必须是字符串");
        if (!item.at("textureSeed").is_number_unsigned()) throw std::runtime_error("textureSeed 必须是 uint32 范围内的整数");
        const auto seed = item.at("textureSeed").get<std::uint64_t>();
        if (seed > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("textureSeed 超出 uint32 范围");

        Material material;
        material.id = item.at("id").get<std::string>();
        material.type = parseType(item.at("type").get<std::string>());
        material.baseColorSrgb = requiredColor(item, "baseColorSRGB");
        material.metallic = requiredNumber(item, "metallic");
        material.roughness = requiredNumber(item, "roughness");
        material.ior = requiredNumber(item, "ior");
        material.absorption = requiredColor(item, "absorption");
        material.emission = requiredColor(item, "emission");
        material.pattern = parsePattern(item.at("pattern").get<std::string>());
        material.textureSeed = static_cast<std::uint32_t>(seed);
        material.baseColorVariation = requiredNumber(item, "baseColorVariation");
        material.roughnessVariation = requiredNumber(item, "roughnessVariation");
        validateMaterial(material);
        const auto index = static_cast<std::uint32_t>(library.materials.size());
        if (!library.ids.emplace(material.id, index).second)
            throw std::runtime_error("材质 id 重复：'" + material.id + "'");
        library.materials.push_back(std::move(material));
    }

    for (const std::string id : {"stone", "red", "blue", "mirror", "wood", "rough_metal", "glass", "glow"})
        (void)library.requireId(id);
    library.textures.reserve(library.materials.size());
    for (const auto& material : library.materials)
        library.textures.push_back(generateTexture(material, library.textureSize));
    return library;
}

Scene buildScene(const MaterialLibrary& materials)
{
    std::map<Cell, std::uint32_t> voxels;
    const auto stone = materials.requireId("stone");
    const auto red = materials.requireId("red");
    const auto blue = materials.requireId("blue");
    const auto mirror = materials.requireId("mirror");
    const auto wood = materials.requireId("wood");
    const auto metal = materials.requireId("rough_metal");
    const auto glow = materials.requireId("glow");
    const auto glass = materials.requireId("glass");

    for (int x = 0; x <= 15; ++x) for (int z = 0; z <= 15; ++z) addVoxel(voxels, x, -1, z, stone);
    for (int y = 0; y <= 5; ++y) for (int z = 0; z <= 15; ++z)
    {
        addVoxel(voxels, 0, y, z, red);
        addVoxel(voxels, 15, y, z, blue);
    }
    for (int x = 1; x <= 14; ++x) for (int y = 0; y <= 5; ++y) addVoxel(voxels, x, y, 0, stone);
    for (int x = 0; x <= 15; ++x) for (int z = 0; z <= 15; ++z)
        if (!(x >= 6 && x <= 9 && z >= 6 && z <= 9)) addVoxel(voxels, x, 6, z, stone);
    for (int x = 5; x <= 10; ++x) for (int y = 1; y <= 4; ++y) addVoxel(voxels, x, y, 1, mirror);
    for (int z = 4; z <= 7; ++z) for (int y = 1; y <= 4; ++y) addVoxel(voxels, 1, y, z, mirror);
    for (int y = 0; y <= 2; ++y) addVoxel(voxels, 3, y, 5, red);
    for (int y = 0; y <= 2; ++y) addVoxel(voxels, 12, y, 6, blue);
    addVoxel(voxels, 4, 0, 10, wood);
    addVoxel(voxels, 4, 1, 10, wood);
    addVoxel(voxels, 5, 0, 10, wood);
    addVoxel(voxels, 10, 0, 10, metal);
    addVoxel(voxels, 10, 1, 10, metal);
    addVoxel(voxels, 4, 5, 4, glow);
    addVoxel(voxels, 11, 5, 4, glow);
    addVoxel(voxels, 4, 5, 11, glow);
    addVoxel(voxels, 11, 5, 11, glow);

    Scene scene;
    scene.opaqueMeshes = greedyMesh(voxels);
    scene.glassMesh.material = glass;
    appendGlassBox(scene.glassMesh);
    for (const auto& mesh : scene.opaqueMeshes)
    {
        if (materials.materials[mesh.material].type == MaterialType::emissive)
            scene.exposedLightTriangles += static_cast<std::uint32_t>(mesh.indices.size() / 3U);
    }
    return scene;
}
}
