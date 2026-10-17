#include "scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <tuple>

namespace vpt {
namespace {

using json = nlohmann::json;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

float readNumber(const json& obj, const char* field, const std::string& context) {
    if (!obj.contains(field)) fail(context + ": 缺少字段 '" + field + "'");
    const json& v = obj.at(field);
    if (!v.is_number()) fail(context + ": 字段 '" + field + "' 必须是数值");
    const double d = v.get<double>();
    if (!std::isfinite(d)) fail(context + ": 字段 '" + field + "' 必须是有限数");
    return static_cast<float>(d);
}

Vec3 readVec3(const json& obj, const char* field, const std::string& context) {
    if (!obj.contains(field)) fail(context + ": 缺少字段 '" + field + "'");
    const json& v = obj.at(field);
    if (!v.is_array() || v.size() != 3) fail(context + ": 字段 '" + field + "' 必须是 3 元数组");
    Vec3 out;
    float comps[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 3; ++i) {
        if (!v.at(static_cast<size_t>(i)).is_number()) {
            fail(context + ": 字段 '" + field + "' 的分量必须是数值");
        }
        const double d = v.at(static_cast<size_t>(i)).get<double>();
        if (!std::isfinite(d)) fail(context + ": 字段 '" + field + "' 的分量必须是有限数");
        comps[i] = static_cast<float>(d);
    }
    out = Vec3(comps[0], comps[1], comps[2]);
    return out;
}

std::string readString(const json& obj, const char* field, const std::string& context) {
    if (!obj.contains(field)) fail(context + ": 缺少字段 '" + field + "'");
    if (!obj.at(field).is_string()) fail(context + ": 字段 '" + field + "' 必须是字符串");
    return obj.at(field).get<std::string>();
}

std::uint32_t readSeed(const json& obj, const char* field, const std::string& context) {
    if (!obj.contains(field)) fail(context + ": 缺少字段 '" + field + "'");
    const json& v = obj.at(field);
    if (!v.is_number_integer()) fail(context + ": 字段 '" + field + "' 必须是整数");
    if (v.is_number_unsigned()) {
        const std::uint64_t u = v.get<std::uint64_t>();
        if (u > 0xFFFFFFFFull) fail(context + ": 字段 '" + field + "' 超出 uint32 范围");
        return static_cast<std::uint32_t>(u);
    }
    const std::int64_t s = v.get<std::int64_t>();
    if (s < 0 || s > 0xFFFFFFFFll) fail(context + ": 字段 '" + field + "' 超出 uint32 范围");
    return static_cast<std::uint32_t>(s);
}

bool inRange(float v, float lo, float hi) { return v >= lo && v <= hi; }

std::uint32_t hashU32(std::uint32_t x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float hash2f(std::int32_t x, std::int32_t y, std::uint32_t seed) {
    const std::uint32_t h = hashU32(static_cast<std::uint32_t>(x) * 374761393u ^
                                    static_cast<std::uint32_t>(y) * 668265263u ^ seed);
    return static_cast<float>(h) * (1.0f / 4294967296.0f);
}

float smoothstep01(float t) { return t * t * (3.0f - 2.0f * t); }

int wrapInt(int v, int period) {
    int r = v % period;
    if (r < 0) r += period;
    return r;
}

float valueNoise(float x, float y, int period, std::uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float u = smoothstep01(fx);
    const float v = smoothstep01(fy);
    const float n00 = hash2f(wrapInt(x0, period), wrapInt(y0, period), seed);
    const float n10 = hash2f(wrapInt(x0 + 1, period), wrapInt(y0, period), seed);
    const float n01 = hash2f(wrapInt(x0, period), wrapInt(y0 + 1, period), seed);
    const float n11 = hash2f(wrapInt(x0 + 1, period), wrapInt(y0 + 1, period), seed);
    const float nx0 = n00 + (n10 - n00) * u;
    const float nx1 = n01 + (n11 - n01) * u;
    return nx0 + (nx1 - nx0) * v;
}

float fbm(float u, float v, std::uint32_t seed) {
    float sum = 0.0f;
    float norm = 0.0f;
    int period = 4;
    float amp = 0.5f;
    for (int octave = 0; octave < 5; ++octave) {
        sum += amp * valueNoise(u * static_cast<float>(period), v * static_cast<float>(period),
                                period, seed + static_cast<std::uint32_t>(octave) * 0x9E3779B9u);
        norm += amp;
        period *= 2;
        amp *= 0.5f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

std::uint8_t toByte(float v) {
    const float c = std::clamp(v, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(c * 255.0f + 0.5f);
}

void generateTextures(SceneData& scene) {
    const std::uint32_t size = scene.textureSize;
    const std::uint32_t layers = static_cast<std::uint32_t>(scene.materials.size());
    std::uint32_t levels = 1;
    for (std::uint32_t s = size; s > 1; s >>= 1u) ++levels;
    scene.mipLevels = levels;

    // 生成 level 0。
    TextureLevel base0;
    base0.width = size;
    base0.height = size;
    base0.pixels.resize(static_cast<size_t>(layers) * size * size * 4u);
    TextureLevel rough0;
    rough0.width = size;
    rough0.height = size;
    rough0.pixels.resize(static_cast<size_t>(layers) * size * size);

    for (std::uint32_t layer = 0; layer < layers; ++layer) {
        const SceneMaterial& mat = scene.materials[layer];
        const std::uint32_t layerBase = layer * size * size;
        for (std::uint32_t y = 0; y < size; ++y) {
            for (std::uint32_t x = 0; x < size; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                float baseMod = 1.0f;
                float roughMod = 1.0f;
                if (mat.pattern == "stone_noise") {
                    const float n = fbm(u, v, mat.textureSeed);
                    baseMod = 1.0f + mat.baseColorVariation * (2.0f * n - 1.0f);
                    const float rn = fbm(u, v, mat.textureSeed ^ 0x51ED270Bu);
                    roughMod = 1.0f + mat.roughnessVariation * (2.0f * rn - 1.0f);
                } else if (mat.pattern == "wood_grain") {
                    const float n = fbm(u * 0.7f, v * 0.7f, mat.textureSeed);
                    // 条纹沿 U 延伸：相位主要随 V 变化，低频噪声扰动。
                    const float stripe = 0.5f + 0.5f * std::sin(
                        6.28318530718f * (v * 8.0f + n * 1.6f));
                    baseMod = 1.0f + mat.baseColorVariation * (2.0f * stripe - 1.0f);
                    const float rn = fbm(u, v, mat.textureSeed ^ 0xA511E9B3u);
                    roughMod = 1.0f + mat.roughnessVariation * (2.0f * rn - 1.0f);
                } else { // solid
                    baseMod = 1.0f;
                    roughMod = 1.0f;
                }
                const Vec3 base = clampColor(mat.baseColorSRGB * baseMod);
                const size_t o = (static_cast<size_t>(layerBase) +
                                  static_cast<size_t>(y) * size + x) * 4u;
                base0.pixels[o + 0] = toByte(base.x);
                base0.pixels[o + 1] = toByte(base.y);
                base0.pixels[o + 2] = toByte(base.z);
                base0.pixels[o + 3] = 255u;
                const float rough = std::clamp(mat.roughness * roughMod, 0.0f, 1.0f);
                rough0.pixels[static_cast<size_t>(layerBase) +
                              static_cast<size_t>(y) * size + x] = toByte(rough);
            }
        }
    }

    scene.baseColorLevels.clear();
    scene.roughnessLevels.clear();
    scene.baseColorLevels.push_back(std::move(base0));
    scene.roughnessLevels.push_back(std::move(rough0));

    // 逐层生成完整 mip 链：基础色在线性空间平均后重新编码，粗糙度在线性标量域平均。
    for (std::uint32_t level = 1; level < levels; ++level) {
        const TextureLevel& prevBase = scene.baseColorLevels[level - 1];
        const TextureLevel& prevRough = scene.roughnessLevels[level - 1];
        const std::uint32_t w = std::max(1u, prevBase.width / 2u);
        const std::uint32_t h = std::max(1u, prevBase.height / 2u);
        TextureLevel baseLevel;
        baseLevel.width = w;
        baseLevel.height = h;
        baseLevel.pixels.resize(static_cast<size_t>(layers) * w * h * 4u);
        TextureLevel roughLevel;
        roughLevel.width = w;
        roughLevel.height = h;
        roughLevel.pixels.resize(static_cast<size_t>(layers) * w * h);

        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            const std::uint32_t layerPrev = layer * prevBase.width * prevBase.height;
            const std::uint32_t layerDst = layer * w * h;
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    Vec3 lin(0.0f, 0.0f, 0.0f);
                    float rough = 0.0f;
                    for (std::uint32_t dy = 0; dy < 2; ++dy) {
                        for (std::uint32_t dx = 0; dx < 2; ++dx) {
                            const std::uint32_t sx = std::min(prevBase.width - 1u, x * 2u + dx);
                            const std::uint32_t sy = std::min(prevBase.height - 1u, y * 2u + dy);
                            const size_t so = (static_cast<size_t>(layerPrev) +
                                               static_cast<size_t>(sy) * prevBase.width + sx) * 4u;
                            const Vec3 srgb(static_cast<float>(prevBase.pixels[so + 0]) / 255.0f,
                                            static_cast<float>(prevBase.pixels[so + 1]) / 255.0f,
                                            static_cast<float>(prevBase.pixels[so + 2]) / 255.0f);
                            lin += srgbToLinear(srgb);
                            const size_t ro = static_cast<size_t>(layerPrev) +
                                static_cast<size_t>(sy) * prevBase.width + sx;
                            rough += static_cast<float>(prevRough.pixels[ro]) / 255.0f;
                        }
                    }
                    lin = lin * 0.25f;
                    rough *= 0.25f;
                    const Vec3 enc = linearToSrgb(lin);
                    const size_t o = (static_cast<size_t>(layerDst) +
                                      static_cast<size_t>(y) * w + x) * 4u;
                    baseLevel.pixels[o + 0] = toByte(enc.x);
                    baseLevel.pixels[o + 1] = toByte(enc.y);
                    baseLevel.pixels[o + 2] = toByte(enc.z);
                    baseLevel.pixels[o + 3] = 255u;
                    roughLevel.pixels[static_cast<size_t>(layerDst) +
                                      static_cast<size_t>(y) * w + x] = toByte(rough);
                }
            }
        }
        scene.baseColorLevels.push_back(std::move(baseLevel));
        scene.roughnessLevels.push_back(std::move(roughLevel));
    }
}

struct FaceMask {
    int planeCount = 0;
    int uCount = 0;
    int vCount = 0;
    std::vector<std::uint32_t> cells; // 0 空，否则 material + 1
};

int faceDirectionIndex(int dx, int dy, int dz) {
    if (dx > 0) return 0; // +X
    if (dx < 0) return 1; // -X
    if (dy > 0) return 2; // +Y
    if (dy < 0) return 3; // -Y
    if (dz > 0) return 4; // +Z
    return 5;             // -Z
}

Vec3 directionNormal(int dir) {
    switch (dir) {
    case 0: return Vec3(1.0f, 0.0f, 0.0f);
    case 1: return Vec3(-1.0f, 0.0f, 0.0f);
    case 2: return Vec3(0.0f, 1.0f, 0.0f);
    case 3: return Vec3(0.0f, -1.0f, 0.0f);
    case 4: return Vec3(0.0f, 0.0f, 1.0f);
    default: return Vec3(0.0f, 0.0f, -1.0f);
    }
}

std::uint32_t addPlane(std::vector<PlaneGpu>& planes, Vec3 n, Vec3 p) {
    const float d = dot(n, p);
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(planes.size()); ++i) {
        const PlaneGpu& plane = planes[i];
        if (std::abs(plane.n[0] - n.x) < 1e-5f && std::abs(plane.n[1] - n.y) < 1e-5f &&
            std::abs(plane.n[2] - n.z) < 1e-5f && std::abs(plane.n[3] - d) < 1e-4f) {
            return i;
        }
    }
    PlaneGpu plane{};
    plane.n[0] = n.x;
    plane.n[1] = n.y;
    plane.n[2] = n.z;
    plane.n[3] = d;
    planes.push_back(plane);
    return static_cast<std::uint32_t>(planes.size() - 1u);
}

void addTriangle(std::vector<TriangleGpu>& tris, Vec3 a, Vec3 b, Vec3 c, Vec3 n,
                 std::uint32_t material, bool needsPlane, std::vector<PlaneGpu>& planes) {
    // 让三角形绕序与面法线一致；求交本身不依赖绕序，但保持数据自洽。
    const Vec3 cr = cross(b - a, c - a);
    if (dot(cr, n) < 0.0f) std::swap(b, c);
    // 仅镜面/玻璃平面进入反射链平面表，保证索引可打包为 uint8。
    const std::uint32_t plane = needsPlane ? addPlane(planes, n, a) : 0u;
    TriangleGpu tri{};
    tri.p0[0] = a.x; tri.p0[1] = a.y; tri.p0[2] = a.z; tri.p0[3] = 0.0f;
    tri.p1[0] = b.x; tri.p1[1] = b.y; tri.p1[2] = b.z; tri.p1[3] = 0.0f;
    tri.p2[0] = c.x; tri.p2[1] = c.y; tri.p2[2] = c.z; tri.p2[3] = 0.0f;
    tri.n[0] = n.x; tri.n[1] = n.y; tri.n[2] = n.z; tri.n[3] = 0.0f;
    tri.extra[0] = material;
    tri.extra[1] = plane;
    tri.extra[2] = 0xFFFFFFFFu;
    tri.extra[3] = 0;
    tris.push_back(tri);
}

} // namespace

std::uint32_t SceneData::materialByType(std::uint32_t type) const {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(materials.size()); ++i) {
        if (materials[i].type == type) return i;
    }
    fail("场景缺少所需类型的材质");
}

std::uint32_t SceneData::materialId(const char* id) const {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(materials.size()); ++i) {
        if (materials[i].id == id) return i;
    }
    fail(std::string("场景材质 ID 无法解析: ") + id);
}

SceneData loadScene(const std::filesystem::path& materialsPath) {
    std::ifstream file(materialsPath);
    if (!file) {
        fail("无法打开材质文件: " + materialsPath.string());
    }
    json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        fail("解析材质文件失败 (" + materialsPath.string() + "): " + e.what());
    }
    if (!root.is_object()) fail("材质文件根对象必须是 JSON object: " + materialsPath.string());

    if (!root.contains("schemaVersion")) fail("材质文件缺少 schemaVersion");
    if (!root.at("schemaVersion").is_number_integer()) fail("schemaVersion 必须是整数");
    if (root.at("schemaVersion").get<std::int64_t>() != 1) {
        fail("不支持的材质 schemaVersion: " + root.at("schemaVersion").dump());
    }
    if (!root.contains("textureSize")) fail("材质文件缺少 textureSize");
    if (!root.at("textureSize").is_number_integer()) fail("textureSize 必须是整数");
    const std::int64_t textureSize64 = root.at("textureSize").get<std::int64_t>();
    if (textureSize64 < 16 || textureSize64 > 1024 ||
        (textureSize64 & (textureSize64 - 1)) != 0) {
        fail("textureSize 必须是 [16,1024] 内的 2 的整数次幂");
    }
    if (!root.contains("materials") || !root.at("materials").is_array()) {
        fail("材质文件缺少 materials 数组");
    }

    SceneData scene;
    scene.textureSize = static_cast<std::uint32_t>(textureSize64);
    const json& mats = root.at("materials");
    if (mats.empty()) fail("materials 不能为空");

    std::vector<std::string> ids;
    for (size_t i = 0; i < mats.size(); ++i) {
        const json& m = mats.at(i);
        const std::string context = "materials[" + std::to_string(i) + "]";
        if (!m.is_object()) fail(context + " 必须是 object");
        SceneMaterial mat;
        mat.id = readString(m, "id", context);
        if (mat.id.empty()) fail(context + ": id 不能为空");
        for (const std::string& existing : ids) {
            if (existing == mat.id) fail(context + ": id 重复: " + mat.id);
        }
        ids.push_back(mat.id);

        const std::string typeName = readString(m, "type", context);
        if (typeName == "diffuse") mat.type = material_type::Diffuse;
        else if (typeName == "rough_metal") mat.type = material_type::RoughMetal;
        else if (typeName == "mirror") mat.type = material_type::Mirror;
        else if (typeName == "dielectric") mat.type = material_type::Dielectric;
        else if (typeName == "emissive") mat.type = material_type::Emissive;
        else fail(context + ": 未知 type: " + typeName);

        mat.baseColorSRGB = readVec3(m, "baseColorSRGB", context);
        if (!inRange(mat.baseColorSRGB.x, 0.0f, 1.0f) ||
            !inRange(mat.baseColorSRGB.y, 0.0f, 1.0f) ||
            !inRange(mat.baseColorSRGB.z, 0.0f, 1.0f)) {
            fail(context + ": baseColorSRGB 分量必须在 [0,1]");
        }
        mat.metallic = readNumber(m, "metallic", context);
        mat.roughness = readNumber(m, "roughness", context);
        mat.ior = readNumber(m, "ior", context);
        mat.absorption = readVec3(m, "absorption", context);
        mat.emission = readVec3(m, "emission", context);
        if (!inRange(mat.metallic, 0.0f, 1.0f)) fail(context + ": metallic 必须在 [0,1]");
        if (!inRange(mat.roughness, 0.0f, 1.0f)) fail(context + ": roughness 必须在 [0,1]");
        if (mat.absorption.x < 0.0f || mat.absorption.y < 0.0f || mat.absorption.z < 0.0f) {
            fail(context + ": absorption 分量必须非负");
        }
        if (mat.emission.x < 0.0f || mat.emission.y < 0.0f || mat.emission.z < 0.0f) {
            fail(context + ": emission 分量必须非负");
        }
        mat.pattern = readString(m, "pattern", context);
        if (mat.pattern != "solid" && mat.pattern != "stone_noise" &&
            mat.pattern != "wood_grain") {
            fail(context + ": 未知 pattern: " + mat.pattern);
        }
        mat.textureSeed = readSeed(m, "textureSeed", context);
        mat.baseColorVariation = readNumber(m, "baseColorVariation", context);
        mat.roughnessVariation = readNumber(m, "roughnessVariation", context);
        if (!inRange(mat.baseColorVariation, 0.0f, 1.0f)) {
            fail(context + ": baseColorVariation 必须在 [0,1]");
        }
        if (!inRange(mat.roughnessVariation, 0.0f, 1.0f)) {
            fail(context + ": roughnessVariation 必须在 [0,1]");
        }

        // 类型语义约束。
        switch (mat.type) {
        case material_type::Diffuse:
            if (mat.metallic != 0.0f) fail(context + ": diffuse 的 metallic 必须为 0");
            break;
        case material_type::RoughMetal:
            if (mat.metallic != 1.0f) fail(context + ": rough_metal 的 metallic 必须为 1");
            if (!(mat.roughness > 0.0f)) fail(context + ": rough_metal 的 roughness 必须大于 0");
            break;
        case material_type::Mirror:
            if (mat.metallic != 1.0f) fail(context + ": mirror 的 metallic 必须为 1");
            if (mat.roughness != 0.0f) fail(context + ": mirror 的 roughness 必须为 0");
            break;
        case material_type::Dielectric:
            if (mat.metallic != 0.0f) fail(context + ": dielectric 的 metallic 必须为 0");
            if (mat.roughness != 0.0f) fail(context + ": dielectric 的 roughness 必须为 0");
            if (!(mat.ior > 1.0f && mat.ior <= 3.0f)) {
                fail(context + ": dielectric 的 ior 必须在 (1,3]");
            }
            break;
        case material_type::Emissive:
            break;
        default:
            fail(context + ": 非法 type");
        }
        if (mat.type != material_type::Dielectric && std::abs(mat.ior - 1.0f) > 1e-6f) {
            fail(context + ": 非 dielectric 的 ior 必须为 1");
        }
        if (mat.type != material_type::Dielectric &&
            (mat.absorption.x != 0.0f || mat.absorption.y != 0.0f || mat.absorption.z != 0.0f)) {
            fail(context + ": 非 dielectric 的 absorption 必须为零");
        }
        if (mat.type != material_type::Emissive &&
            (mat.emission.x != 0.0f || mat.emission.y != 0.0f || mat.emission.z != 0.0f)) {
            fail(context + ": 非 emissive 的 emission 必须为零");
        }
        if ((mat.type == material_type::Mirror || mat.type == material_type::Dielectric ||
             mat.type == material_type::Emissive) &&
            (mat.pattern != "solid" || mat.baseColorVariation != 0.0f ||
             mat.roughnessVariation != 0.0f)) {
            fail(context + ": mirror/dielectric/emissive 必须使用 solid 且 variation 为零");
        }
        mat.layer = static_cast<std::uint32_t>(scene.materials.size());
        scene.materials.push_back(mat);
    }

    // 场景引用的材质 ID 必须全部存在。
    const char* required[] = {"stone", "wood", "red", "blue", "rough_metal", "mirror", "glass", "glow"};
    for (const char* id : required) {
        bool found = false;
        for (const SceneMaterial& m : scene.materials) {
            if (m.id == id) { found = true; break; }
        }
        if (!found) fail(std::string("场景所需材质 ID 缺失: ") + id);
    }

    generateTextures(scene);

    const std::uint32_t stone = scene.materialId("stone");
    const std::uint32_t wood = scene.materialId("wood");
    const std::uint32_t red = scene.materialId("red");
    const std::uint32_t blue = scene.materialId("blue");
    const std::uint32_t roughMetal = scene.materialId("rough_metal");
    const std::uint32_t mirror = scene.materialId("mirror");
    const std::uint32_t glass = scene.materialId("glass");
    const std::uint32_t glow = scene.materialId("glow");

    std::vector<std::uint8_t> isGlass(scene.materials.size(), 0);
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        isGlass[i] = scene.materials[i].type == material_type::Dielectric ? 1u : 0u;
    }

    // grid: 0 空气，否则 material + 1。
    const int kY = kGridYCount;
    std::vector<std::uint32_t> grid(static_cast<size_t>(kGridX) * kY * kGridZ, 0u);
    auto cellIndex = [](int x, int y, int z) {
        return (static_cast<size_t>(x) * kGridYCount + static_cast<size_t>(y - kGridYMin)) *
            kGridZ + static_cast<size_t>(z);
    };
    auto setCell = [&](int x, int y, int z, std::uint32_t mat) {
        if (x < 0 || x >= kGridX || z < 0 || z >= kGridZ || y < kGridYMin || y > kGridYMax) {
            fail("场景填充越界");
        }
        grid[cellIndex(x, y, z)] = mat + 1u;
    };
    auto fillBox = [&](int x0, int x1, int y0, int y1, int z0, int z1, std::uint32_t mat) {
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z) setCell(x, y, z, mat);
    };

    fillBox(0, 15, -1, -1, 0, 15, stone);          // 1 地板
    fillBox(0, 0, 0, 5, 0, 15, red);               // 2 左墙
    fillBox(15, 15, 0, 5, 0, 15, blue);            // 3 右墙
    fillBox(1, 14, 0, 5, 0, 0, stone);             // 4 后墙
    for (int x = 0; x < 16; ++x) {                 // 5 顶板，中央留洞
        for (int z = 0; z < 16; ++z) {
            if (x >= 6 && x <= 9 && z >= 6 && z <= 9) continue;
            setCell(x, 6, z, stone);
        }
    }
    fillBox(5, 10, 1, 4, 1, 1, mirror);            // 6
    fillBox(1, 1, 1, 4, 4, 7, mirror);             // 7
    const int redPillar[][3] = {{3, 0, 5}, {3, 1, 5}, {3, 2, 5}};
    for (const auto& c : redPillar) setCell(c[0], c[1], c[2], red);       // 8
    const int bluePillar[][3] = {{12, 0, 6}, {12, 1, 6}, {12, 2, 6}};
    for (const auto& c : bluePillar) setCell(c[0], c[1], c[2], blue);     // 9
    const int woodSteps[][3] = {{4, 0, 10}, {4, 1, 10}, {5, 0, 10}};
    for (const auto& c : woodSteps) setCell(c[0], c[1], c[2], wood);      // 10
    const int metalPillar[][3] = {{10, 0, 10}, {10, 1, 10}};
    for (const auto& c : metalPillar) setCell(c[0], c[1], c[2], roughMetal); // 11
    fillBox(7, 8, 0, 1, 7, 8, glass);              // 12
    const int glowBlocks[][3] = {{4, 5, 4}, {11, 5, 4}, {4, 5, 11}, {11, 5, 11}};
    for (const auto& c : glowBlocks) setCell(c[0], c[1], c[2], glow);     // 13

    // 每个法线方向独立构建暴露面掩码。
    std::array<FaceMask, 6> masks;
    for (int dir = 0; dir < 6; ++dir) {
        FaceMask& m = masks[static_cast<size_t>(dir)];
        if (dir <= 1) { m.planeCount = 17; m.uCount = kGridZ; m.vCount = kY; }
        else if (dir <= 3) { m.planeCount = 8; m.uCount = kGridX; m.vCount = kGridZ; }
        else { m.planeCount = 17; m.uCount = kGridX; m.vCount = kY; }
        m.cells.assign(static_cast<size_t>(m.planeCount) * m.uCount * m.vCount, 0u);
    }
    auto maskIndex = [](const FaceMask& m, int plane, int u, int v) {
        return (static_cast<size_t>(plane) * static_cast<size_t>(m.uCount) +
                static_cast<size_t>(u)) * static_cast<size_t>(m.vCount) + static_cast<size_t>(v);
    };
    auto gridAt = [&](int x, int y, int z) -> std::uint32_t {
        if (x < 0 || x >= kGridX || z < 0 || z >= kGridZ || y < kGridYMin || y > kGridYMax) {
            return 0u;
        }
        return grid[cellIndex(x, y, z)];
    };

    for (int x = 0; x < kGridX; ++x) {
        for (int yi = 0; yi < kY; ++yi) {
            const int y = yi + kGridYMin;
            for (int z = 0; z < kGridZ; ++z) {
                const std::uint32_t value = grid[cellIndex(x, y, z)];
                if (value == 0u) continue;
                const std::uint32_t mat = value - 1u;
                const bool cellGlass = isGlass[mat] != 0u;
                const int vIndex = yi;
                const int neighbors[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                             {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (int dir = 0; dir < 6; ++dir) {
                    const int nx = x + neighbors[dir][0];
                    const int ny = y + neighbors[dir][1];
                    const int nz = z + neighbors[dir][2];
                    const std::uint32_t nv = gridAt(nx, ny, nz);
                    bool exposed = false;
                    if (cellGlass) {
                        exposed = nv == 0u || !isGlass[nv - 1u];
                    } else {
                        const bool nOpaque = nv != 0u && !isGlass[nv - 1u];
                        exposed = !nOpaque;
                    }
                    if (!exposed) continue;
                    FaceMask& m = masks[static_cast<size_t>(dir)];
                    int plane = 0;
                    int u = 0;
                    int v = 0;
                    if (dir == 0) { plane = x + 1; u = z; v = vIndex; }
                    else if (dir == 1) { plane = x; u = z; v = vIndex; }
                    else if (dir == 2) { plane = y + 1; u = x; v = z; }
                    else if (dir == 3) { plane = y - kGridYMin; u = x; v = z; }
                    else if (dir == 4) { plane = z + 1; u = x; v = vIndex; }
                    else { plane = z; u = x; v = vIndex; }
                    m.cells[maskIndex(m, plane, u, v)] = mat + 1u;
                }
            }
        }
    }

    std::vector<std::vector<TriangleGpu>> perMaterial(scene.materials.size());
    auto emitQuad = [&](int dir, int plane, int u0, int v0, int w, int h, std::uint32_t mat) {
        auto world = [&](int u, int v) -> Vec3 {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            if (dir == 0 || dir == 1) {
                x = static_cast<float>(plane);
                y = static_cast<float>(v + kGridYMin);
                z = static_cast<float>(u);
            } else if (dir == 2) {
                x = static_cast<float>(u);
                y = static_cast<float>(plane);
                z = static_cast<float>(v);
            } else if (dir == 3) {
                x = static_cast<float>(u);
                y = static_cast<float>(plane + kGridYMin);
                z = static_cast<float>(v);
            } else {
                x = static_cast<float>(u);
                y = static_cast<float>(v + kGridYMin);
                z = static_cast<float>(plane);
            }
            if (isGlass[mat] != 0u) y += 0.125f;
            return Vec3(x, y, z);
        };
        const Vec3 c0 = world(u0, v0);
        const Vec3 c1 = world(u0 + w, v0);
        const Vec3 c2 = world(u0 + w, v0 + h);
        const Vec3 c3 = world(u0, v0 + h);
        const Vec3 n = directionNormal(dir);
        const bool needsPlane = isGlass[mat] != 0u ||
            scene.materials[mat].type == material_type::Mirror;
        addTriangle(perMaterial[mat], c0, c1, c2, n, mat, needsPlane, scene.planes);
        addTriangle(perMaterial[mat], c0, c2, c3, n, mat, needsPlane, scene.planes);
    };

    for (int dir = 0; dir < 6; ++dir) {
        FaceMask& m = masks[static_cast<size_t>(dir)];
        for (int plane = 0; plane < m.planeCount; ++plane) {
            for (int u = 0; u < m.uCount; ++u) {
                for (int v = 0; v < m.vCount; ++v) {
                    const std::uint32_t matPlus = m.cells[maskIndex(m, plane, u, v)];
                    if (matPlus == 0u) continue;
                    int w = 1;
                    while (u + w < m.uCount &&
                           m.cells[maskIndex(m, plane, u + w, v)] == matPlus) {
                        ++w;
                    }
                    int h = 1;
                    bool canGrow = true;
                    while (canGrow && v + h < m.vCount) {
                        for (int du = 0; du < w; ++du) {
                            if (m.cells[maskIndex(m, plane, u + du, v + h)] != matPlus) {
                                canGrow = false;
                                break;
                            }
                        }
                        if (canGrow) ++h;
                    }
                    for (int du = 0; du < w; ++du) {
                        for (int dv = 0; dv < h; ++dv) {
                            m.cells[maskIndex(m, plane, u + du, v + dv)] = 0u;
                        }
                    }
                    emitQuad(dir, plane, u, v, w, h, matPlus - 1u);
                }
            }
        }
    }

    // 按材质分组生成实例与全局三角形数组。
    std::uint32_t instanceIndex = 0;
    for (std::uint32_t mat = 0; mat < static_cast<std::uint32_t>(scene.materials.size()); ++mat) {
        std::vector<TriangleGpu>& tris = perMaterial[mat];
        if (tris.empty()) continue;
        InstanceDescGpu desc{};
        desc.triangleOffset = static_cast<std::uint32_t>(scene.triangles.size());
        desc.triangleCount = static_cast<std::uint32_t>(tris.size());
        desc.material = mat;
        scene.instances.push_back(desc);
        std::vector<Vec3> positions;
        std::vector<std::uint32_t> indices;
        positions.reserve(tris.size() * 3u);
        indices.reserve(tris.size() * 3u);
        for (TriangleGpu& tri : tris) {
            tri.extra[3] = instanceIndex;
            const Vec3 p0(tri.p0[0], tri.p0[1], tri.p0[2]);
            const Vec3 p1(tri.p1[0], tri.p1[1], tri.p1[2]);
            const Vec3 p2(tri.p2[0], tri.p2[1], tri.p2[2]);
            const std::uint32_t base = static_cast<std::uint32_t>(positions.size());
            positions.push_back(p0);
            positions.push_back(p1);
            positions.push_back(p2);
            indices.push_back(base);
            indices.push_back(base + 1u);
            indices.push_back(base + 2u);
            scene.triangles.push_back(tri);
        }
        scene.groupPositions.push_back(std::move(positions));
        scene.groupIndices.push_back(std::move(indices));
        ++instanceIndex;
    }

    // 从实际暴露的 emissive 三角形构建唯一光源表。
    double totalPower = 0.0;
    std::vector<double> powers;
    for (std::uint32_t triIndex = 0; triIndex < static_cast<std::uint32_t>(scene.triangles.size());
         ++triIndex) {
        TriangleGpu& tri = scene.triangles[triIndex];
        const std::uint32_t mat = tri.extra[0];
        if (scene.materials[mat].type != material_type::Emissive) continue;
        const Vec3 p0(tri.p0[0], tri.p0[1], tri.p0[2]);
        const Vec3 p1(tri.p1[0], tri.p1[1], tri.p1[2]);
        const Vec3 p2(tri.p2[0], tri.p2[1], tri.p2[2]);
        const double area = 0.5 * static_cast<double>(length(cross(p1 - p0, p2 - p0)));
        if (!(area > 0.0)) continue;
        const Vec3 emission = scene.materials[mat].emission;
        const double power = area * static_cast<double>(luminance(emission));
        if (!(power > 0.0)) continue;
        const std::uint32_t lightIndex = static_cast<std::uint32_t>(scene.lights.size());
        tri.extra[2] = lightIndex;
        LightGpu light{};
        light.ids[0] = tri.extra[3];
        light.ids[1] = triIndex - scene.instances[tri.extra[3]].triangleOffset;
        light.ids[2] = triIndex;
        light.ids[3] = lightIndex;
        light.emissionArea[0] = emission.x;
        light.emissionArea[1] = emission.y;
        light.emissionArea[2] = emission.z;
        light.emissionArea[3] = static_cast<float>(area);
        scene.lights.push_back(light);
        powers.push_back(power);
        totalPower += power;
    }
    if (scene.lights.empty()) fail("场景没有有效发光三角形");
    double cumulative = 0.0;
    for (size_t i = 0; i < scene.lights.size(); ++i) {
        cumulative += powers[i];
        scene.lights[i].cdf[0] = static_cast<float>(cumulative / totalPower);
        scene.lights[i].cdf[1] = static_cast<float>(powers[i] / totalPower);
    }
    scene.lights.back().cdf[0] = 1.0f;

    // 玻璃体 AABB，供相机在玻璃内时初始化介质状态。
    Vec3 glassMin(1e30f, 1e30f, 1e30f);
    Vec3 glassMax(-1e30f, -1e30f, -1e30f);
    for (const TriangleGpu& tri : scene.triangles) {
        if (scene.materials[tri.extra[0]].type != material_type::Dielectric) continue;
        const Vec3 pts[3] = {Vec3(tri.p0[0], tri.p0[1], tri.p0[2]),
                             Vec3(tri.p1[0], tri.p1[1], tri.p1[2]),
                             Vec3(tri.p2[0], tri.p2[1], tri.p2[2])};
        for (const Vec3& p : pts) {
            glassMin.x = std::min(glassMin.x, p.x);
            glassMin.y = std::min(glassMin.y, p.y);
            glassMin.z = std::min(glassMin.z, p.z);
            glassMax.x = std::max(glassMax.x, p.x);
            glassMax.y = std::max(glassMax.y, p.y);
            glassMax.z = std::max(glassMax.z, p.z);
        }
    }
    scene.glassMin = glassMin;
    scene.glassMax = glassMax;
    if (scene.planes.size() > 255u) fail("反射平面数量超过 uint8 打包上限");

    scene.gpuMaterials.resize(scene.materials.size());
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const SceneMaterial& m = scene.materials[i];
        MaterialGpu& g = scene.gpuMaterials[i];
        const Vec3 lin = srgbToLinear(m.baseColorSRGB);
        g.baseColor[0] = lin.x; g.baseColor[1] = lin.y; g.baseColor[2] = lin.z; g.baseColor[3] = 0.0f;
        g.absorption[0] = m.absorption.x; g.absorption[1] = m.absorption.y;
        g.absorption[2] = m.absorption.z; g.absorption[3] = m.ior;
        g.emission[0] = m.emission.x; g.emission[1] = m.emission.y;
        g.emission[2] = m.emission.z; g.emission[3] = m.metallic;
        g.params[0] = m.roughness;
        g.params[1] = static_cast<float>(m.type);
        g.params[2] = static_cast<float>(m.layer);
        g.params[3] = 0.0f;
    }

    return scene;
}

} // namespace vpt
