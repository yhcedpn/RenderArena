#include "scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>

namespace voxel {
namespace {
using Json = nlohmann::json;

constexpr int kMinX = 0, kMaxX = 15;
constexpr int kMinY = -1, kMaxY = 6;
constexpr int kMinZ = 0, kMaxZ = 15;
constexpr std::uint32_t kNoMaterial = std::numeric_limits<std::uint32_t>::max();

float finiteNumber(const Json& object, const char* key)
{
    const auto& value = object.at(key);
    if (!value.is_number()) throw std::runtime_error(std::string("字段必须为数值: ") + key);
    const float result = value.get<float>();
    if (!std::isfinite(result)) throw std::runtime_error(std::string("字段必须为有限数: ") + key);
    return result;
}

std::array<float, 3> vector3(const Json& object, const char* key)
{
    const auto& value = object.at(key);
    if (!value.is_array() || value.size() != 3) throw std::runtime_error(std::string("字段必须包含 3 个数值: ") + key);
    std::array<float, 3> result{};
    for (std::size_t i = 0; i != 3; ++i) {
        if (!value[i].is_number()) throw std::runtime_error(std::string("字段必须包含 3 个数值: ") + key);
        result[i] = value[i].get<float>();
        if (!std::isfinite(result[i])) throw std::runtime_error(std::string("字段必须为有限数: ") + key);
    }
    return result;
}

void requireRange(float v, float lo, float hi, const std::string& name)
{
    if (v < lo || v > hi) throw std::runtime_error(name + " 超出允许范围");
}

std::uint32_t parseUint32(const Json& object, const char* key)
{
    const auto& value = object.at(key);
    if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
        throw std::runtime_error(std::string("字段必须为 uint32: ") + key);
    const auto n = value.get<std::uint64_t>();
    if (n > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error(std::string("字段超出 uint32: ") + key);
    return static_cast<std::uint32_t>(n);
}

Material parseMaterial(const Json& entry)
{
    if (!entry.is_object()) throw std::runtime_error("materials 项必须为对象");
    Material m;
    if (!entry.at("id").is_string() || entry.at("id").get<std::string>().empty())
        throw std::runtime_error("材质 id 必须为非空字符串");
    m.id = entry.at("id").get<std::string>();
    const auto type = entry.at("type");
    if (!type.is_string()) throw std::runtime_error(m.id + ": type 必须为字符串");
    const std::string typeText = type.get<std::string>();
    if (typeText == "diffuse") m.type = MaterialType::diffuse;
    else if (typeText == "rough_metal") m.type = MaterialType::roughMetal;
    else if (typeText == "mirror") m.type = MaterialType::mirror;
    else if (typeText == "dielectric") m.type = MaterialType::dielectric;
    else if (typeText == "emissive") m.type = MaterialType::emissive;
    else throw std::runtime_error(m.id + ": 不支持的材质 type " + typeText);

    m.baseColorSRGB = vector3(entry, "baseColorSRGB");
    m.metallic = finiteNumber(entry, "metallic");
    m.roughness = finiteNumber(entry, "roughness");
    m.ior = finiteNumber(entry, "ior");
    m.absorption = vector3(entry, "absorption");
    m.emission = vector3(entry, "emission");
    m.baseColorVariation = finiteNumber(entry, "baseColorVariation");
    m.roughnessVariation = finiteNumber(entry, "roughnessVariation");
    m.textureSeed = parseUint32(entry, "textureSeed");
    const auto pattern = entry.at("pattern");
    if (!pattern.is_string()) throw std::runtime_error(m.id + ": pattern 必须为字符串");
    const std::string patternText = pattern.get<std::string>();
    if (patternText == "solid") m.pattern = Pattern::solid;
    else if (patternText == "stone_noise") m.pattern = Pattern::stoneNoise;
    else if (patternText == "wood_grain") m.pattern = Pattern::woodGrain;
    else throw std::runtime_error(m.id + ": 不支持的材质 pattern " + patternText);

    for (float c : m.baseColorSRGB) requireRange(c, 0.0f, 1.0f, m.id + ".baseColorSRGB");
    for (float c : m.absorption) if (c < 0.0f) throw std::runtime_error(m.id + ": absorption 不得为负数");
    for (float c : m.emission) if (c < 0.0f) throw std::runtime_error(m.id + ": emission 不得为负数");
    requireRange(m.metallic, 0.0f, 1.0f, m.id + ".metallic");
    requireRange(m.roughness, 0.0f, 1.0f, m.id + ".roughness");
    requireRange(m.baseColorVariation, 0.0f, 1.0f, m.id + ".baseColorVariation");
    requireRange(m.roughnessVariation, 0.0f, 1.0f, m.id + ".roughnessVariation");
    if (m.type == MaterialType::dielectric) {
        requireRange(m.ior, std::nextafter(1.0f, 2.0f), 3.0f, m.id + ".ior");
    } else if (m.ior != 1.0f) throw std::runtime_error(m.id + ": 非 dielectric 的 ior 必须为 1");

    auto zero3 = [](const std::array<float, 3>& c) { return c[0] == 0 && c[1] == 0 && c[2] == 0; };
    if (m.type == MaterialType::diffuse && m.metallic != 0.0f) throw std::runtime_error(m.id + ": diffuse metallic 必须为 0");
    if (m.type == MaterialType::roughMetal && (m.metallic != 1.0f || m.roughness == 0.0f)) throw std::runtime_error(m.id + ": rough_metal 要求 metallic=1 且 roughness>0");
    if (m.type == MaterialType::mirror && (m.metallic != 1.0f || m.roughness != 0.0f)) throw std::runtime_error(m.id + ": mirror 要求 metallic=1 且 roughness=0");
    if (m.type == MaterialType::dielectric && (m.metallic != 0.0f || m.roughness != 0.0f)) throw std::runtime_error(m.id + ": dielectric 要求 metallic=0 且 roughness=0");
    if (m.type != MaterialType::dielectric && !zero3(m.absorption)) throw std::runtime_error(m.id + ": 非 dielectric 的 absorption 必须为零");
    if (m.type != MaterialType::emissive && !zero3(m.emission)) throw std::runtime_error(m.id + ": 非 emissive 的 emission 必须为零");
    if ((m.type == MaterialType::mirror || m.type == MaterialType::dielectric || m.type == MaterialType::emissive) &&
        (m.pattern != Pattern::solid || m.baseColorVariation != 0.0f || m.roughnessVariation != 0.0f))
        throw std::runtime_error(m.id + ": mirror/dielectric/emissive 必须使用 solid 且 variation 为零");
    return m;
}

std::uint32_t hash2(int x, int y, std::uint32_t seed)
{
    std::uint32_t h = seed ^ (static_cast<std::uint32_t>(x) * 0x8da6b343u) ^ (static_cast<std::uint32_t>(y) * 0xd8163841u);
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}

float random01(int x, int y, std::uint32_t seed)
{
    return static_cast<float>(hash2(x, y, seed) & 0x00ffffffu) / 16777215.0f;
}

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float valueNoise(float x, float y, std::uint32_t seed)
{
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const float fx = smooth(x - std::floor(x)), fy = smooth(y - std::floor(y));
    const float a = random01(ix, iy, seed), b = random01(ix + 1, iy, seed);
    const float c = random01(ix, iy + 1, seed), d = random01(ix + 1, iy + 1, seed);
    const float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
    return ab + (cd - ab) * fy;
}

float srgbToLinear(float x)
{
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float x)
{
    x = std::max(x, 0.0f);
    return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}
std::uint8_t unorm8(float v)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

TextureMip makeBaseTexture(const Material& m, std::uint32_t size)
{
    TextureMip mip;
    mip.width = size; mip.height = size;
    mip.rgba.resize(static_cast<std::size_t>(size) * size * 4);
    mip.roughness.resize(static_cast<std::size_t>(size) * size);
    for (std::uint32_t y = 0; y < size; ++y) for (std::uint32_t x = 0; x < size; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / size;
        const float v = (static_cast<float>(y) + 0.5f) / size;
        float n = 0.5f;
        if (m.pattern == Pattern::stoneNoise) {
            n = 0.55f * valueNoise(u * 5.0f, v * 5.0f, m.textureSeed) +
                0.30f * valueNoise(u * 13.0f, v * 13.0f, m.textureSeed ^ 0x9e3779b9u) +
                0.15f * valueNoise(u * 31.0f, v * 31.0f, m.textureSeed ^ 0x85ebca6bu);
        } else if (m.pattern == Pattern::woodGrain) {
            const float warp = (valueNoise(u * 3.0f, v * 4.0f, m.textureSeed) - 0.5f) * 0.06f;
            const float grain = 0.5f + 0.5f * std::sin((v + warp) * 2.0f * 3.1415926535f * 18.0f);
            n = 0.72f * grain + 0.28f * valueNoise(u * 5.0f, v * 7.0f, m.textureSeed ^ 0x27d4eb2du);
        }
        const float modulation = (n - 0.5f) * 2.0f;
        const std::size_t pixel = static_cast<std::size_t>(y) * size + x;
        for (std::size_t c = 0; c < 3; ++c)
            mip.rgba[pixel * 4 + c] = unorm8(m.baseColorSRGB[c] * (1.0f + modulation * m.baseColorVariation));
        mip.rgba[pixel * 4 + 3] = 255;
        auto roughnessByte = unorm8(m.roughness * (1.0f + modulation * m.roughnessVariation));
        if (m.type == MaterialType::roughMetal) roughnessByte = std::max<std::uint8_t>(roughnessByte, 1);
        mip.roughness[pixel] = roughnessByte;
    }
    return mip;
}

std::vector<TextureMip> generateMips(const Material& material, std::uint32_t size, std::uint32_t mipCount)
{
    std::vector<TextureMip> result;
    result.reserve(mipCount);
    result.push_back(makeBaseTexture(material, size));
    while (result.size() < mipCount) {
        const auto& prev = result.back();
        TextureMip next;
        next.width = std::max(1u, prev.width / 2);
        next.height = std::max(1u, prev.height / 2);
        const std::size_t pixels = static_cast<std::size_t>(next.width) * next.height;
        next.rgba.resize(pixels * 4); next.roughness.resize(pixels);
        for (std::uint32_t y = 0; y < next.height; ++y) for (std::uint32_t x = 0; x < next.width; ++x) {
            const std::size_t dst = static_cast<std::size_t>(y) * next.width + x;
            std::array<float, 3> linear{}; float rough = 0;
            for (std::uint32_t oy = 0; oy < 2; ++oy) for (std::uint32_t ox = 0; ox < 2; ++ox) {
                const std::size_t src = static_cast<std::size_t>(y * 2 + oy) * prev.width + (x * 2 + ox);
                for (std::size_t c = 0; c < 3; ++c) linear[c] += srgbToLinear(prev.rgba[src * 4 + c] / 255.0f) * 0.25f;
                rough += prev.roughness[src] / 255.0f * 0.25f;
            }
            for (std::size_t c = 0; c < 3; ++c) next.rgba[dst * 4 + c] = unorm8(linearToSrgb(linear[c]));
            next.rgba[dst * 4 + 3] = 255;
            next.roughness[dst] = unorm8(rough);
        }
        result.push_back(std::move(next));
    }
    return result;
}

struct Grid {
    std::array<std::uint32_t, 16 * 8 * 16> data{};
    Grid() { data.fill(kNoMaterial); }
    static std::size_t index(int x, int y, int z) {
        return (static_cast<std::size_t>(y - kMinY) * 16 + static_cast<std::size_t>(z)) * 16 + static_cast<std::size_t>(x);
    }
    std::uint32_t get(int x, int y, int z) const {
        if (x < kMinX || x > kMaxX || y < kMinY || y > kMaxY || z < kMinZ || z > kMaxZ) return kNoMaterial;
        return data[index(x, y, z)];
    }
    void set(int x, int y, int z, std::uint32_t m) { data[index(x, y, z)] = m; }
};

struct FaceCell { std::uint32_t material{kNoMaterial}; std::uint8_t side{}; };

std::uint32_t stableSurfaceId(int axis, int plane, int a, int b, int width, int height, std::uint32_t material, int side)
{
    std::uint32_t h = 2166136261u;
    auto add = [&h](std::uint32_t v) { h = (h ^ v) * 16777619u; };
    add(static_cast<std::uint32_t>(axis)); add(static_cast<std::uint32_t>(plane));
    add(static_cast<std::uint32_t>(a)); add(static_cast<std::uint32_t>(b));
    add(static_cast<std::uint32_t>(width)); add(static_cast<std::uint32_t>(height));
    add(material); add(static_cast<std::uint32_t>(side));
    return h ? h : 1u;
}

void appendQuad(Scene& scene, std::uint32_t material, std::uint32_t surfaceId,
                const std::array<std::array<float, 3>, 4>& p,
                const std::array<std::array<float, 2>, 4>& uv,
                const std::array<float, 3>& normal)
{
    const std::uint32_t base = static_cast<std::uint32_t>(scene.vertices.size());
    for (int i = 0; i < 4; ++i) scene.vertices.push_back(Vertex{p[i], uv[i]});
    const float cx = (p[0][1] - p[1][1]) * (p[0][2] - p[2][2]) - (p[0][2] - p[1][2]) * (p[0][1] - p[2][1]);
    const float cy = (p[0][2] - p[1][2]) * (p[0][0] - p[2][0]) - (p[0][0] - p[1][0]) * (p[0][2] - p[2][2]);
    const float cz = (p[0][0] - p[1][0]) * (p[0][1] - p[2][1]) - (p[0][1] - p[1][1]) * (p[0][0] - p[2][0]);
    const float dot = cx * normal[0] + cy * normal[1] + cz * normal[2];
    if (dot >= 0.0f) {
        scene.triangles.push_back({{base, base + 1, base + 2}, material, surfaceId});
        scene.triangles.push_back({{base, base + 2, base + 3}, material, surfaceId});
    } else {
        scene.triangles.push_back({{base, base + 2, base + 1}, material, surfaceId});
        scene.triangles.push_back({{base, base + 3, base + 2}, material, surfaceId});
    }
}

void emitGreedyFace(Scene& scene, int axis, int plane, int u0, int v0, int width, int height,
                    const FaceCell& cell, const std::array<int, 3>& origin)
{
    // 2D mask axes are the two world axes other than the face normal. UV spans
    // equal their world-unit lengths, so merging never changes texture density.
    int ua = (axis + 1) % 3, va = (axis + 2) % 3;
    std::array<float, 3> lo{}, hi{};
    lo[axis] = hi[axis] = static_cast<float>(plane);
    lo[ua] = static_cast<float>(u0); hi[ua] = static_cast<float>(u0 + width);
    lo[va] = static_cast<float>(v0); hi[va] = static_cast<float>(v0 + height);
    // axis cyclic order has the positive cross-product direction. Reverse when
    // the exposed face points toward the negative axis.
    std::array<std::array<float, 3>, 4> p{};
    std::array<std::array<float, 2>, 4> uv{};
    p[0] = lo; p[1] = lo; p[1][ua] = hi[ua];
    p[2] = hi; p[2][axis] = lo[axis];
    p[3] = lo; p[3][va] = hi[va]; p[3][axis] = lo[axis];
    // Replace the normal coordinate by the requested plane and use local U/V.
    for (auto& q : p) q[axis] = static_cast<float>(plane);
    uv = {{{0,0}, {static_cast<float>(width),0}, {static_cast<float>(width),static_cast<float>(height)}, {0,static_cast<float>(height)}}};
    const bool positive = cell.side != 0;
    const std::array<float, 3> normal = axis == 0 ? std::array<float,3>{positive ? 1.f : -1.f,0,0} :
        axis == 1 ? std::array<float,3>{0,positive ? 1.f : -1.f,0} : std::array<float,3>{0,0,positive ? 1.f : -1.f};
    if (!positive) {
        std::swap(p[1], p[3]); std::swap(uv[1], uv[3]);
    }
    const int worldU = u0, worldV = v0;
    const std::uint32_t surface = stableSurfaceId(axis, plane, worldU, worldV, width, height, cell.material, positive ? 1 : 0);
    appendQuad(scene, cell.material, surface, p, uv, normal);
    (void)origin;
}

void buildOpaqueMesh(Scene& scene, const Grid& grid)
{
    for (int axis = 0; axis < 3; ++axis) {
        const int ua = (axis + 1) % 3, va = (axis + 2) % 3;
        const int minAxis = axis == 0 ? kMinX : axis == 1 ? kMinY : kMinZ;
        const int maxAxis = axis == 0 ? kMaxX : axis == 1 ? kMaxY : kMaxZ;
        const int minU = ua == 0 ? kMinX : ua == 1 ? kMinY : kMinZ;
        const int maxU = ua == 0 ? kMaxX : ua == 1 ? kMaxY : kMaxZ;
        const int minV = va == 0 ? kMinX : va == 1 ? kMinY : kMinZ;
        const int maxV = va == 0 ? kMaxX : va == 1 ? kMaxY : kMaxZ;
        const int countU = maxU - minU + 1, countV = maxV - minV + 1;
        std::vector<FaceCell> mask(static_cast<std::size_t>(countU) * countV);
        for (int plane = minAxis; plane <= maxAxis + 1; ++plane) {
            std::fill(mask.begin(), mask.end(), FaceCell{});
            for (int v = minV; v <= maxV; ++v) for (int u = minU; u <= maxU; ++u) {
                std::array<int, 3> c0{}, c1{};
                c0[axis] = plane - 1; c1[axis] = plane;
                c0[ua] = c1[ua] = u; c0[va] = c1[va] = v;
                auto get = [&](const std::array<int,3>& c) { return grid.get(c[0], c[1], c[2]); };
                const auto negative = get(c0), positive = get(c1);
                FaceCell f{};
                if (negative != kNoMaterial && positive == kNoMaterial) f = {negative, 1};
                else if (positive != kNoMaterial && negative == kNoMaterial) f = {positive, 0};
                mask[static_cast<std::size_t>(v - minV) * countU + (u - minU)] = f;
            }
            // Deterministic row-major greedy rectangle extraction; every chosen
            // rectangle is maximal for this scan order and compatible material/side.
            for (int v = 0; v < countV; ++v) for (int u = 0; u < countU; ) {
                FaceCell cell = mask[static_cast<std::size_t>(v) * countU + u];
                if (cell.material == kNoMaterial) { ++u; continue; }
                int width = 1;
                while (u + width < countU) {
                    const auto& n = mask[static_cast<std::size_t>(v) * countU + u + width];
                    if (n.material != cell.material || n.side != cell.side) break;
                    ++width;
                }
                int height = 1;
                for (; v + height < countV; ++height) {
                    bool compatible = true;
                    for (int x = 0; x < width; ++x) {
                        const auto& n = mask[static_cast<std::size_t>(v + height) * countU + u + x];
                        if (n.material != cell.material || n.side != cell.side) { compatible = false; break; }
                    }
                    if (!compatible) break;
                }
                // Emit in world-grid coordinates. The mask starts may be negative
                // on Y; U/V offsets are converted in emitGreedyFace's stable key.
                emitGreedyFace(scene, axis, plane, minU + u, minV + v, width, height, cell, {});
                for (int yy = 0; yy < height; ++yy) for (int xx = 0; xx < width; ++xx)
                    mask[static_cast<std::size_t>(v + yy) * countU + u + xx] = {};
                u += width;
            }
        }
    }
}

void appendGlass(Scene& scene, std::uint32_t glassMaterial)
{
    constexpr float x0 = 7, x1 = 9, y0 = 0.125f, y1 = 2.125f, z0 = 7, z1 = 9;
    const std::uint32_t seed = stableSurfaceId(3, 0, 0, 0, 0, 0, glassMaterial, 0);
    auto quad = [&](std::array<std::array<float,3>,4> p, std::array<float,3> n, int face) {
        std::array<std::array<float,2>,4> uv{{{0,0},{2,0},{2,2},{0,2}}};
        appendQuad(scene, glassMaterial, seed ^ static_cast<std::uint32_t>(face * 0x9e3779b9u), p, uv, n);
    };
    quad({{{x1,y0,z0},{x1,y0,z1},{x1,y1,z1},{x1,y1,z0}}}, {1,0,0}, 0);
    quad({{{x0,y0,z1},{x0,y0,z0},{x0,y1,z0},{x0,y1,z1}}}, {-1,0,0}, 1);
    quad({{{x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1}}}, {0,1,0}, 2);
    quad({{{x0,y0,z1},{x1,y0,z1},{x1,y0,z0},{x0,y0,z0}}}, {0,-1,0}, 3);
    quad({{{x1,y0,z1},{x0,y0,z1},{x0,y1,z1},{x1,y1,z1}}}, {0,0,1}, 4);
    quad({{{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0}}}, {0,0,-1}, 5);
}

} // namespace

Scene loadScene(const std::filesystem::path& materialsPath)
{
    Json root;
    try {
        std::ifstream input(materialsPath, std::ios::binary);
        if (!input) throw std::runtime_error("无法打开材料配置文件");
        input >> root;
    } catch (const std::exception& e) {
        throw std::runtime_error("读取材料配置失败 " + materialsPath.string() + ": " + e.what());
    }
    if (!root.is_object()) throw std::runtime_error("materials.json 根节点必须为对象");
    if (!root.contains("schemaVersion") || !root.at("schemaVersion").is_number_integer() || root.at("schemaVersion").get<int>() != 1)
        throw std::runtime_error("materials.json schemaVersion 必须为 1");
    if (!root.contains("textureSize") || !root.at("textureSize").is_number_integer()) throw std::runtime_error("materials.json 缺少整数 textureSize");
    const auto textureSize = root.at("textureSize").get<std::int64_t>();
    if (textureSize < 16 || textureSize > 1024 || (textureSize & (textureSize - 1)) != 0)
        throw std::runtime_error("textureSize 必须为 16 至 1024 范围内的 2 的整数次幂");
    if (!root.contains("materials") || !root.at("materials").is_array()) throw std::runtime_error("materials.json 缺少 materials 数组");

    Scene scene;
    scene.textureSize = static_cast<std::uint32_t>(textureSize);
    for (std::uint32_t n = scene.textureSize; n > 1; n >>= 1) ++scene.mipCount;
    ++scene.mipCount;
    std::unordered_map<std::string, std::uint32_t> materialIds;
    for (const auto& entry : root.at("materials")) {
        Material m = parseMaterial(entry);
        if (!materialIds.emplace(m.id, static_cast<std::uint32_t>(scene.materials.size())).second)
            throw std::runtime_error("材质 id 重复: " + m.id);
        scene.materials.push_back(std::move(m));
    }
    const std::array<const char*, 8> roleNames{"stone","wood","red","blue","rough_metal","mirror","glass","glow"};
    for (std::size_t i = 0; i < roleNames.size(); ++i) {
        auto it = materialIds.find(roleNames[i]);
        if (it == materialIds.end()) throw std::runtime_error(std::string("场景无法解析必需材质 ID: ") + roleNames[i]);
        scene.materialByRole[i] = it->second;
    }
    scene.textures.reserve(scene.materials.size() * scene.mipCount);
    for (const Material& material : scene.materials) {
        auto mips = generateMips(material, scene.textureSize, scene.mipCount);
        for (auto& mip : mips) scene.textures.push_back(std::move(mip));
    }

    Grid grid;
    const auto stone = scene.materialByRole[0], wood = scene.materialByRole[1], red = scene.materialByRole[2];
    const auto blue = scene.materialByRole[3], metal = scene.materialByRole[4], mirror = scene.materialByRole[5];
    const auto glow = scene.materialByRole[7];
    for (int z = 0; z <= 15; ++z) for (int x = 0; x <= 15; ++x) grid.set(x, -1, z, stone);
    for (int y = 0; y <= 5; ++y) for (int z = 0; z <= 15; ++z) { grid.set(0,y,z,red); grid.set(15,y,z,blue); }
    for (int y = 0; y <= 5; ++y) for (int x = 1; x <= 14; ++x) grid.set(x,y,0,stone);
    for (int x = 0; x <= 15; ++x) for (int z = 0; z <= 15; ++z)
        if (!(x >= 6 && x <= 9 && z >= 6 && z <= 9)) grid.set(x,6,z,stone);
    for (int x = 5; x <= 10; ++x) for (int y = 1; y <= 4; ++y) grid.set(x,y,1,mirror);
    for (int z = 4; z <= 7; ++z) for (int y = 1; y <= 4; ++y) grid.set(1,y,z,mirror);
    for (int y = 0; y <= 2; ++y) grid.set(3,y,5,red);
    for (int y = 0; y <= 2; ++y) grid.set(12,y,6,blue);
    grid.set(4,0,10,wood); grid.set(4,1,10,wood); grid.set(5,0,10,wood);
    grid.set(10,0,10,metal); grid.set(10,1,10,metal);
    grid.set(4,5,4,glow); grid.set(11,5,4,glow); grid.set(4,5,11,glow); grid.set(11,5,11,glow);
    buildOpaqueMesh(scene, grid);
    appendGlass(scene, scene.materialByRole[6]);

    for (std::uint32_t ti = 0; ti < scene.triangles.size(); ++ti) {
        const auto& tri = scene.triangles[ti];
        if (scene.materials[tri.material].type != MaterialType::emissive) continue;
        const auto& a = scene.vertices[tri.vertex[0]].position;
        const auto& b = scene.vertices[tri.vertex[1]].position;
        const auto& c = scene.vertices[tri.vertex[2]].position;
        const float ax=b[0]-a[0], ay=b[1]-a[1], az=b[2]-a[2];
        const float bx=c[0]-a[0], by=c[1]-a[1], bz=c[2]-a[2];
        const float cx=ay*bz-az*by, cy=az*bx-ax*bz, cz=ax*by-ay*bx;
        const float area=0.5f*std::sqrt(cx*cx+cy*cy+cz*cz);
        const auto& e=scene.materials[tri.material].emission;
        const float power=area*(0.2126f*e[0]+0.7152f*e[1]+0.0722f*e[2]);
        if (power > 0) { scene.totalLightPower += power; scene.lights.push_back({ti,area,power,scene.totalLightPower}); }
    }
    if (scene.lights.empty() || !(scene.totalLightPower > 0.0f)) throw std::runtime_error("场景必须包含功率大于零的暴露发光三角形");
    for (auto& light : scene.lights) light.cdf /= scene.totalLightPower;
    return scene;
}

} // namespace voxel
