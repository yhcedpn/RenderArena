// materials.json 解析、程序化纹理生成、体素场景提取与 greedy meshing。
#pragma once

#include "rae_common.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace rae {

inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

struct MaterialDesc {
    std::string id;
    uint32_t type = kMatDiffuse;
    Vec3 baseColorSRGB{0.5f, 0.5f, 0.5f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    float ior = 1.0f;
    Vec3 absorption{0.0f, 0.0f, 0.0f};
    Vec3 emission{0.0f, 0.0f, 0.0f};
    uint32_t pattern = kPatternSolid;
    uint32_t textureSeed = 0;
    float baseColorVariation = 0.0f;
    float roughnessVariation = 0.0f;
};

struct TextureLayer {
    // [mip]，每个 mip 是 size*size 的 RGBA8（base）或 R8（roughness）。
    std::vector<std::vector<uint8_t>> base;
    std::vector<std::vector<uint8_t>> rough;
};

struct CpuGroup {
    uint32_t material = 0;
    bool glassBody = false;
    std::vector<Vec3> vertices;
    std::vector<uint32_t> indices;
    // 每个三角形对应的外法线；由提取方向决定，不依赖 winding。
    std::vector<Vec3> triangleNormals;
};

struct SceneData {
    std::vector<MaterialDesc> materials;
    std::vector<GpuMaterial> gpuMaterials;
    std::vector<TextureLayer> textures;
    uint32_t textureSize = 0;
    uint32_t mipLevels = 0;

    std::vector<CpuGroup> groups;
    std::vector<uint32_t> groupTriOffset;
    std::vector<GpuTriangle> triangles;
    std::vector<GpuLightEntry> lights;
    float totalLightPower = 0.0f;

    Vec3 glassMin{0.0f};
    Vec3 glassMax{0.0f};
    uint32_t glassMaterial = 0;

    std::string error;

    bool load(const std::filesystem::path& materialsPath);

private:
    bool parseMaterials(const std::filesystem::path& materialsPath);
    bool generateTextures();
    bool buildSceneMesh();
};

namespace detail {

inline uint32_t hashU32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

inline uint32_t hashCombine(uint32_t a, uint32_t b) {
    return hashU32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2)));
}

inline float hash01(int x, int y, uint32_t seed) {
    const uint32_t h = hashU32(static_cast<uint32_t>(x) * 0x8da6b343u ^
                               static_cast<uint32_t>(y) * 0xd8163841u ^ seed);
    return static_cast<float>(h & 0x00ffffffu) / 16777215.0f;
}

inline float smoothStep01(float t) { return t * t * (3.0f - 2.0f * t); }

inline float valueNoiseTileable(float u, float v, int freq, uint32_t seed) {
    const float x = u * static_cast<float>(freq);
    const float y = v * static_cast<float>(freq);
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = smoothStep01(x - static_cast<float>(x0));
    const float fy = smoothStep01(y - static_cast<float>(y0));
    const int x0m = ((x0 % freq) + freq) % freq;
    const int y0m = ((y0 % freq) + freq) % freq;
    const int x1m = (x0m + 1) % freq;
    const int y1m = (y0m + 1) % freq;
    const float n00 = hash01(x0m, y0m, seed);
    const float n10 = hash01(x1m, y0m, seed);
    const float n01 = hash01(x0m, y1m, seed);
    const float n11 = hash01(x1m, y1m, seed);
    const float nx0 = n00 + (n10 - n00) * fx;
    const float nx1 = n01 + (n11 - n01) * fx;
    return nx0 + (nx1 - nx0) * fy;
}

inline float fbmTileable(float u, float v, uint32_t seed, int octaves) {
    float sum = 0.0f;
    float amp = 0.5f;
    float total = 0.0f;
    int freq = 4;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoiseTileable(u, v, freq, seed + static_cast<uint32_t>(o) * 0x9e3779b9u);
        total += amp;
        amp *= 0.5f;
        freq *= 2;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

inline bool finite3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline std::string formatTypeName(uint32_t type) {
    switch (type) {
        case kMatDiffuse: return "diffuse";
        case kMatRoughMetal: return "rough_metal";
        case kMatMirror: return "mirror";
        case kMatDielectric: return "dielectric";
        case kMatEmissive: return "emissive";
        default: return "unknown";
    }
}

} // namespace detail

inline bool SceneData::parseMaterials(const std::filesystem::path& materialsPath) {
    std::ifstream file(materialsPath, std::ios::binary);
    if (!file) {
        error = "无法打开材质配置: " + materialsPath.string();
        return false;
    }
    nlohmann::json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        error = "材质配置解析失败: " + std::string(e.what());
        return false;
    }
    if (!root.is_object()) {
        error = "材质配置根节点必须是对象";
        return false;
    }
    try {
        const int schemaVersion = root.at("schemaVersion").get<int>();
        if (schemaVersion != 1) {
            error = "不支持的 schemaVersion: " + std::to_string(schemaVersion);
            return false;
        }
        const int64_t texSize = root.at("textureSize").get<int64_t>();
        if (texSize < 16 || texSize > 1024 || (texSize & (texSize - 1)) != 0) {
            error = "textureSize 必须是 [16,1024] 内的 2 的幂";
            return false;
        }
        textureSize = static_cast<uint32_t>(texSize);
        mipLevels = 1;
        while ((1u << mipLevels) <= textureSize) {
            ++mipLevels;
        }
        // mipLevels：128 -> 8（log2+1）。
        mipLevels = static_cast<uint32_t>(std::log2(static_cast<double>(textureSize))) + 1u;

        const auto& mats = root.at("materials");
        if (!mats.is_array() || mats.empty()) {
            error = "materials 必须是非空数组";
            return false;
        }
        std::map<std::string, uint32_t> ids;
        for (const auto& node : mats) {
            if (!node.is_object()) {
                error = "materials 中存在非对象元素";
                return false;
            }
            MaterialDesc m;
            m.id = node.at("id").get<std::string>();
            if (m.id.empty()) {
                error = "材质 id 不能为空";
                return false;
            }
            if (ids.count(m.id) != 0) {
                error = "材质 id 重复: " + m.id;
                return false;
            }
            const std::string type = node.at("type").get<std::string>();
            if (type == "diffuse") {
                m.type = kMatDiffuse;
            } else if (type == "rough_metal") {
                m.type = kMatRoughMetal;
            } else if (type == "mirror") {
                m.type = kMatMirror;
            } else if (type == "dielectric") {
                m.type = kMatDielectric;
            } else if (type == "emissive") {
                m.type = kMatEmissive;
            } else {
                error = "未知材质类型: " + type + " (id=" + m.id + ")";
                return false;
            }
            const auto base = node.at("baseColorSRGB");
            if (!base.is_array() || base.size() != 3) {
                error = "baseColorSRGB 必须是 3 个数的数组 (id=" + m.id + ")";
                return false;
            }
            m.baseColorSRGB = {base[0].get<float>(), base[1].get<float>(), base[2].get<float>()};
            m.metallic = node.at("metallic").get<float>();
            m.roughness = node.at("roughness").get<float>();
            m.ior = node.at("ior").get<float>();
            const auto abs = node.at("absorption");
            if (!abs.is_array() || abs.size() != 3) {
                error = "absorption 必须是 3 个数的数组 (id=" + m.id + ")";
                return false;
            }
            m.absorption = {abs[0].get<float>(), abs[1].get<float>(), abs[2].get<float>()};
            const auto emi = node.at("emission");
            if (!emi.is_array() || emi.size() != 3) {
                error = "emission 必须是 3 个数的数组 (id=" + m.id + ")";
                return false;
            }
            m.emission = {emi[0].get<float>(), emi[1].get<float>(), emi[2].get<float>()};
            const std::string pattern = node.at("pattern").get<std::string>();
            if (pattern == "solid") {
                m.pattern = kPatternSolid;
            } else if (pattern == "stone_noise") {
                m.pattern = kPatternStone;
            } else if (pattern == "wood_grain") {
                m.pattern = kPatternWood;
            } else {
                error = "未知 pattern: " + pattern + " (id=" + m.id + ")";
                return false;
            }
            const int64_t seed = node.at("textureSeed").get<int64_t>();
            if (seed < 0 || seed > 0xffffffffLL) {
                error = "textureSeed 超出 uint32 范围 (id=" + m.id + ")";
                return false;
            }
            m.textureSeed = static_cast<uint32_t>(seed);
            m.baseColorVariation = node.at("baseColorVariation").get<float>();
            m.roughnessVariation = node.at("roughnessVariation").get<float>();

            // 数值范围与类型语义校验。
            auto in01 = [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; };
            if (!detail::finite3(m.baseColorSRGB) || !in01(m.baseColorSRGB.x) ||
                !in01(m.baseColorSRGB.y) || !in01(m.baseColorSRGB.z)) {
                error = "baseColorSRGB 分量必须是 [0,1] 内有限数 (id=" + m.id + ")";
                return false;
            }
            if (!std::isfinite(m.metallic) || !in01(m.metallic)) {
                error = "metallic 必须是 [0,1] 内有限数 (id=" + m.id + ")";
                return false;
            }
            if (!std::isfinite(m.roughness) || !in01(m.roughness)) {
                error = "roughness 必须是 [0,1] 内有限数 (id=" + m.id + ")";
                return false;
            }
            if (!std::isfinite(m.ior)) {
                error = "ior 必须是有限数 (id=" + m.id + ")";
                return false;
            }
            if (!detail::finite3(m.absorption) || m.absorption.x < 0.0f || m.absorption.y < 0.0f ||
                m.absorption.z < 0.0f) {
                error = "absorption 必须是非负有限数 (id=" + m.id + ")";
                return false;
            }
            if (!detail::finite3(m.emission) || m.emission.x < 0.0f || m.emission.y < 0.0f ||
                m.emission.z < 0.0f) {
                error = "emission 必须是非负有限数 (id=" + m.id + ")";
                return false;
            }
            if (!std::isfinite(m.baseColorVariation) || !in01(m.baseColorVariation) ||
                !std::isfinite(m.roughnessVariation) || !in01(m.roughnessVariation)) {
                error = "variation 必须是 [0,1] 内有限数 (id=" + m.id + ")";
                return false;
            }
            const bool zeroAbs = std::abs(m.absorption.x) <= 1e-6f &&
                                 std::abs(m.absorption.y) <= 1e-6f &&
                                 std::abs(m.absorption.z) <= 1e-6f;
            const bool zeroEmi = std::abs(m.emission.x) <= 1e-6f &&
                                 std::abs(m.emission.y) <= 1e-6f &&
                                 std::abs(m.emission.z) <= 1e-6f;
            switch (m.type) {
                case kMatDiffuse:
                    if (std::abs(m.metallic) > 1e-6f) {
                        error = "diffuse 要求 metallic=0 (id=" + m.id + ")";
                        return false;
                    }
                    break;
                case kMatRoughMetal:
                    if (std::abs(m.metallic - 1.0f) > 1e-6f || m.roughness <= 0.0f) {
                        error = "rough_metal 要求 metallic=1 且 roughness>0 (id=" + m.id + ")";
                        return false;
                    }
                    break;
                case kMatMirror:
                    if (std::abs(m.metallic - 1.0f) > 1e-6f || m.roughness > 1e-6f) {
                        error = "mirror 要求 metallic=1 且 roughness=0 (id=" + m.id + ")";
                        return false;
                    }
                    if (m.pattern != kPatternSolid || m.baseColorVariation > 1e-6f ||
                        m.roughnessVariation > 1e-6f) {
                        error = "mirror 要求 solid pattern 且 variation=0 (id=" + m.id + ")";
                        return false;
                    }
                    break;
                case kMatDielectric:
                    if (std::abs(m.metallic) > 1e-6f || m.roughness > 1e-6f) {
                        error = "dielectric 要求 metallic=0 且 roughness=0 (id=" + m.id + ")";
                        return false;
                    }
                    if (!(m.ior > 1.0f && m.ior <= 3.0f)) {
                        error = "dielectric 要求 ior 在 (1,3] (id=" + m.id + ")";
                        return false;
                    }
                    if (m.pattern != kPatternSolid || m.baseColorVariation > 1e-6f ||
                        m.roughnessVariation > 1e-6f) {
                        error = "dielectric 要求 solid pattern 且 variation=0 (id=" + m.id + ")";
                        return false;
                    }
                    break;
                case kMatEmissive:
                    if (m.pattern != kPatternSolid || m.baseColorVariation > 1e-6f ||
                        m.roughnessVariation > 1e-6f) {
                        error = "emissive 要求 solid pattern 且 variation=0 (id=" + m.id + ")";
                        return false;
                    }
                    break;
                default: break;
            }
            if (m.type != kMatDielectric && std::abs(m.ior - 1.0f) > 1e-6f) {
                error = "非 dielectric 要求 ior=1 (id=" + m.id + ")";
                return false;
            }
            if (m.type != kMatDielectric && !zeroAbs) {
                error = "非 dielectric 要求 absorption=0 (id=" + m.id + ")";
                return false;
            }
            if (m.type != kMatEmissive && !zeroEmi) {
                error = "非 emissive 要求 emission=0 (id=" + m.id + ")";
                return false;
            }

            ids.emplace(m.id, static_cast<uint32_t>(materials.size()));
            materials.push_back(m);
        }

        // 场景引用的材质 ID 必须存在。
        static const char* kSceneIds[] = {"stone", "wood", "red", "blue", "rough_metal",
                                          "mirror", "glass", "glow"};
        for (const char* id : kSceneIds) {
            if (ids.count(id) == 0) {
                error = std::string("场景引用的材质缺失: ") + id;
                return false;
            }
        }
        glassMaterial = ids.at("glass");
    } catch (const std::exception& e) {
        error = "材质字段缺失或类型错误: " + std::string(e.what());
        return false;
    }
    return true;
}

inline bool SceneData::generateTextures() {
    textures.resize(materials.size());
    const uint32_t size = textureSize;
    for (size_t mi = 0; mi < materials.size(); ++mi) {
        const MaterialDesc& m = materials[mi];
        TextureLayer& layer = textures[mi];
        layer.base.resize(mipLevels);
        layer.rough.resize(mipLevels);
        const size_t level0Count = static_cast<size_t>(size) * size;
        layer.base[0].resize(level0Count * 4);
        layer.rough[0].resize(level0Count);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                float colorFactor = 1.0f;
                float roughFactor = 0.0f; // [-1,1] 噪声
                if (m.pattern == kPatternStone || m.pattern == kPatternWood) {
                    const float n = detail::fbmTileable(u, v, m.textureSeed, 4);
                    if (m.pattern == kPatternStone) {
                        colorFactor = 1.0f + m.baseColorVariation * (2.0f * n - 1.0f);
                        roughFactor = 2.0f * n - 1.0f;
                    } else {
                        // 条纹沿纹理 U 方向延伸：变化主要发生在 V；用低频噪声扰动相位。
                        const float low = detail::fbmTileable(u, v, m.textureSeed + 977u, 3);
                        const float stripe =
                            0.5f + 0.5f * std::sin(6.28318530718f *
                                                  (v * 8.0f + 0.35f * low + 0.08f * n));
                        colorFactor = 1.0f + m.baseColorVariation *
                                                  (2.0f * stripe - 1.0f) * (0.7f + 0.3f * low);
                        roughFactor = 2.0f * n - 1.0f;
                    }
                }
                const float rough =
                    std::clamp(m.roughness + roughFactor * m.roughnessVariation, 0.0f, 1.0f);
                const size_t idx = static_cast<size_t>(y) * size + x;
                const float rgb[3] = {std::clamp(m.baseColorSRGB.x * colorFactor, 0.0f, 1.0f),
                                      std::clamp(m.baseColorSRGB.y * colorFactor, 0.0f, 1.0f),
                                      std::clamp(m.baseColorSRGB.z * colorFactor, 0.0f, 1.0f)};
                for (int c = 0; c < 3; ++c) {
                    layer.base[0][idx * 4 + static_cast<size_t>(c)] =
                        static_cast<uint8_t>(std::lround(rgb[c] * 255.0f));
                }
                layer.base[0][idx * 4 + 3] = 255;
                layer.rough[0][idx] = static_cast<uint8_t>(std::lround(rough * 255.0f));
            }
        }
        // 基础色 mip：线性域平均后重新编码；粗糙度：线性标量域平均。
        uint32_t srcSize = size;
        for (uint32_t level = 1; level < mipLevels; ++level) {
            const uint32_t dstSize = srcSize / 2;
            layer.base[level].resize(static_cast<size_t>(dstSize) * dstSize * 4);
            layer.rough[level].resize(static_cast<size_t>(dstSize) * dstSize);
            const std::vector<uint8_t>& srcBase = layer.base[level - 1];
            const std::vector<uint8_t>& srcRough = layer.rough[level - 1];
            for (uint32_t y = 0; y < dstSize; ++y) {
                for (uint32_t x = 0; x < dstSize; ++x) {
                    float lin[3] = {0.0f, 0.0f, 0.0f};
                    float rough = 0.0f;
                    for (uint32_t oy = 0; oy < 2; ++oy) {
                        for (uint32_t ox = 0; ox < 2; ++ox) {
                            const uint32_t sx = x * 2 + ox;
                            const uint32_t sy = y * 2 + oy;
                            const size_t si = static_cast<size_t>(sy) * srcSize + sx;
                            for (int c = 0; c < 3; ++c) {
                                lin[c] += srgbToLinear(
                                    static_cast<float>(srcBase[si * 4 + static_cast<size_t>(c)]) /
                                    255.0f);
                            }
                            rough += static_cast<float>(srcRough[si]) / 255.0f;
                        }
                    }
                    const size_t di = static_cast<size_t>(y) * dstSize + x;
                    for (int c = 0; c < 3; ++c) {
                        layer.base[level][di * 4 + static_cast<size_t>(c)] =
                            static_cast<uint8_t>(
                                std::lround(std::clamp(linearToSrgb(lin[c] * 0.25f), 0.0f, 1.0f) *
                                            255.0f));
                    }
                    layer.base[level][di * 4 + 3] = 255;
                    layer.rough[level][di] = static_cast<uint8_t>(
                        std::lround(std::clamp(rough * 0.25f, 0.0f, 1.0f) * 255.0f));
                }
            }
            srcSize = dstSize;
        }
    }

    gpuMaterials.resize(materials.size());
    for (size_t i = 0; i < materials.size(); ++i) {
        const MaterialDesc& m = materials[i];
        GpuMaterial& g = gpuMaterials[i];
        g.baseColor[0] = srgbToLinear(m.baseColorSRGB.x);
        g.baseColor[1] = srgbToLinear(m.baseColorSRGB.y);
        g.baseColor[2] = srgbToLinear(m.baseColorSRGB.z);
        g.baseColor[3] = static_cast<float>(m.type);
        g.params[0] = m.metallic;
        g.params[1] = m.roughness;
        g.params[2] = m.ior;
        g.params[3] = static_cast<float>(i);
        g.absorption[0] = m.absorption.x;
        g.absorption[1] = m.absorption.y;
        g.absorption[2] = m.absorption.z;
        g.absorption[3] = static_cast<float>(i);
        g.emission[0] = m.emission.x;
        g.emission[1] = m.emission.y;
        g.emission[2] = m.emission.z;
        g.emission[3] = static_cast<float>(m.pattern);
    }
    return true;
}

inline bool SceneData::buildSceneMesh() {
    // 16 x 8 x 16 的格子，Y 索引 0 对应世界 y=-1。
    constexpr int kDimX = 16;
    constexpr int kDimY = 8;
    constexpr int kDimZ = 16;
    std::vector<int32_t> voxels(static_cast<size_t>(kDimX) * kDimY * kDimZ, -1);
    auto voxelIndex = [](int x, int y, int z) {
        return static_cast<size_t>((y + 1) * kDimX * kDimZ + z * kDimX + x);
    };
    auto setVoxel = [&](int x, int y, int z, uint32_t mat) {
        if (x >= 0 && x < kDimX && y >= -1 && y <= 6 && z >= 0 && z < kDimZ) {
            voxels[voxelIndex(x, y, z)] = static_cast<int32_t>(mat);
        }
    };
    auto matOf = [&](const char* name) {
        for (size_t i = 0; i < materials.size(); ++i) {
            if (materials[i].id == name) {
                return static_cast<uint32_t>(i);
            }
        }
        return 0u;
    };
    const uint32_t stone = matOf("stone");
    const uint32_t wood = matOf("wood");
    const uint32_t red = matOf("red");
    const uint32_t blue = matOf("blue");
    const uint32_t roughMetal = matOf("rough_metal");
    const uint32_t mirror = matOf("mirror");
    const uint32_t glow = matOf("glow");

    // 1. 地板。
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            setVoxel(x, -1, z, stone);
        }
    }
    // 2/3. 左右墙。
    for (int y = 0; y <= 5; ++y) {
        for (int z = 0; z < 16; ++z) {
            setVoxel(0, y, z, red);
            setVoxel(15, y, z, blue);
        }
    }
    // 4. 后墙。
    for (int x = 1; x <= 14; ++x) {
        for (int y = 0; y <= 5; ++y) {
            setVoxel(x, y, 0, stone);
        }
    }
    // 5. 顶板，中央留 4x4 开口。
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            const bool opening = (x >= 6 && x <= 9 && z >= 6 && z <= 9);
            if (!opening) {
                setVoxel(x, 6, z, stone);
            }
        }
    }
    // 6. 后方镜面方块墙。
    for (int x = 5; x <= 10; ++x) {
        for (int y = 1; y <= 4; ++y) {
            setVoxel(x, y, 1, mirror);
        }
    }
    // 7. 左侧镜面方块墙。
    for (int y = 1; y <= 4; ++y) {
        for (int z = 4; z <= 7; ++z) {
            setVoxel(1, y, z, mirror);
        }
    }
    // 8. 红柱。
    for (int y = 0; y <= 2; ++y) {
        setVoxel(3, y, 5, red);
    }
    // 9. 蓝柱。
    for (int y = 0; y <= 2; ++y) {
        setVoxel(12, y, 6, blue);
    }
    // 10. 木阶梯。
    setVoxel(4, 0, 10, wood);
    setVoxel(4, 1, 10, wood);
    setVoxel(5, 0, 10, wood);
    // 11. 粗糙金属柱。
    setVoxel(10, 0, 10, roughMetal);
    setVoxel(10, 1, 10, roughMetal);
    // 12. 玻璃实心体（8 格）。
    for (int x = 7; x <= 8; ++x) {
        for (int y = 0; y <= 1; ++y) {
            for (int z = 7; z <= 8; ++z) {
                setVoxel(x, y, z, glassMaterial);
            }
        }
    }
    // 13. 四个发光块。
    const int glowPos[4][3] = {{4, 5, 4}, {11, 5, 4}, {4, 5, 11}, {11, 5, 11}};
    for (const auto& p : glowPos) {
        setVoxel(p[0], p[1], p[2], glow);
    }

    auto cellMat = [&](int x, int y, int z) -> int32_t {
        if (x < 0 || x >= kDimX || y < -1 || y > 6 || z < 0 || z >= kDimZ) {
            return -1;
        }
        return voxels[voxelIndex(x, y, z)];
    };
    auto isGlassCell = [&](int32_t m) { return m == static_cast<int32_t>(glassMaterial); };
    auto isOpaqueCell = [&](int32_t m) { return m >= 0 && !isGlassCell(m); };

    struct PendingQuad {
        uint32_t material = 0;
        bool glass = false;
        Vec3 normal;
        Vec3 v[4];
    };
    std::vector<PendingQuad> quads;
    quads.reserve(2048);

    const int dims[3] = {kDimX, kDimY, kDimZ};
    const int uAxis[3] = {2, 0, 0};
    const int vAxis[3] = {1, 2, 1};
    for (int glassPass = 0; glassPass < 2; ++glassPass) {
        const bool wantGlass = (glassPass == 1);
        for (int axis = 0; axis < 3; ++axis) {
            const int ua = uAxis[axis];
            const int va = vAxis[axis];
            const int w = dims[ua];
            const int h = dims[va];
            for (int sign = -1; sign <= 1; sign += 2) {
                for (int slice = 0; slice < dims[axis]; ++slice) {
                    std::vector<int32_t> mask(static_cast<size_t>(w) * h, -1);
                    for (int vi = 0; vi < h; ++vi) {
                        for (int ui = 0; ui < w; ++ui) {
                            int coord[3] = {0, 0, 0};
                            coord[axis] = slice;
                            coord[ua] = ui;
                            coord[va] = vi;
                            const int x = coord[0];
                            const int yi = coord[1];
                            const int z = coord[2];
                            const int32_t m = cellMat(x, yi, z);
                            if (m < 0) {
                                continue;
                            }
                            const bool isGlass = isGlassCell(m);
                            if (isGlass != wantGlass) {
                                continue;
                            }
                            const int nx = x + (axis == 0 ? sign : 0);
                            const int ny = yi + (axis == 1 ? sign : 0);
                            const int nz = z + (axis == 2 ? sign : 0);
                            const int32_t nm = cellMat(nx, ny, nz);
                            const bool exposed =
                                wantGlass ? !isGlassCell(nm) : !isOpaqueCell(nm);
                            if (!exposed) {
                                continue;
                            }
                            mask[static_cast<size_t>(vi) * w + ui] = m;
                        }
                    }
                    std::vector<uint8_t> used(static_cast<size_t>(w) * h, 0);
                    for (int vi = 0; vi < h; ++vi) {
                        for (int ui = 0; ui < w; ++ui) {
                            const size_t base = static_cast<size_t>(vi) * w + ui;
                            if (used[base] != 0 || mask[base] < 0) {
                                continue;
                            }
                            const int32_t mat = mask[base];
                            int u1 = ui + 1;
                            while (u1 < w && used[static_cast<size_t>(vi) * w + u1] == 0 &&
                                   mask[static_cast<size_t>(vi) * w + u1] == mat) {
                                ++u1;
                            }
                            int v1 = vi + 1;
                            while (v1 < h) {
                                bool ok = true;
                                for (int u = ui; u < u1; ++u) {
                                    const size_t k = static_cast<size_t>(v1) * w + u;
                                    if (used[k] != 0 || mask[k] != mat) {
                                        ok = false;
                                        break;
                                    }
                                }
                                if (!ok) {
                                    break;
                                }
                                ++v1;
                            }
                            for (int v = vi; v < v1; ++v) {
                                for (int u = ui; u < u1; ++u) {
                                    used[static_cast<size_t>(v) * w + u] = 1;
                                }
                            }
                            const float offAxis = (axis == 1) ? -1.0f : 0.0f;
                            const float offU = (ua == 1) ? -1.0f : 0.0f;
                            const float offV = (va == 1) ? -1.0f : 0.0f;
                            const float cellMin = static_cast<float>(slice) + offAxis;
                            const float plane = cellMin + (sign > 0 ? 1.0f : 0.0f);
                            const float u0 = static_cast<float>(ui) + offU;
                            const float u1f = static_cast<float>(u1) + offU;
                            const float v0 = static_cast<float>(vi) + offV;
                            const float v1f = static_cast<float>(v1) + offV;
                            PendingQuad q;
                            q.material = static_cast<uint32_t>(mat);
                            q.glass = wantGlass;
                            q.normal = Vec3(0.0f);
                            if (axis == 0) {
                                q.normal.x = static_cast<float>(sign);
                            } else if (axis == 1) {
                                q.normal.y = static_cast<float>(sign);
                            } else {
                                q.normal.z = static_cast<float>(sign);
                            }
                            auto makePoint = [&](float uu, float vv) {
                                Vec3 p;
                                if (axis == 0) {
                                    p = {plane, vv, uu};
                                } else if (axis == 1) {
                                    p = {uu, plane, vv};
                                } else {
                                    p = {uu, vv, plane};
                                }
                                if (wantGlass) {
                                    p.y += 0.125f;
                                }
                                return p;
                            };
                            q.v[0] = makePoint(u0, v0);
                            q.v[1] = makePoint(u1f, v0);
                            q.v[2] = makePoint(u1f, v1f);
                            q.v[3] = makePoint(u0, v1f);
                            quads.push_back(q);
                        }
                    }
                }
            }
        }
    }

    // 按 (材质, 是否玻璃体) 分组。
    groups.clear();
    auto findGroup = [&](uint32_t material, bool glass) -> size_t {
        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].material == material && groups[i].glassBody == glass) {
                return i;
            }
        }
        CpuGroup g;
        g.material = material;
        g.glassBody = glass;
        groups.push_back(std::move(g));
        return groups.size() - 1;
    };
    for (const PendingQuad& q : quads) {
        const size_t gi = findGroup(q.material, q.glass);
        CpuGroup& g = groups[gi];
        const uint32_t base = static_cast<uint32_t>(g.vertices.size());
        for (const Vec3& p : q.v) {
            g.vertices.push_back(p);
        }
        g.indices.push_back(base + 0u);
        g.indices.push_back(base + 1u);
        g.indices.push_back(base + 2u);
        g.indices.push_back(base + 0u);
        g.indices.push_back(base + 2u);
        g.indices.push_back(base + 3u);
        g.triangleNormals.push_back(q.normal);
        g.triangleNormals.push_back(q.normal);
    }

    // 材质组按材质索引排序，保证每次运行全局三角形编号稳定。
    std::sort(groups.begin(), groups.end(), [](const CpuGroup& a, const CpuGroup& b) {
        if (a.material != b.material) {
            return a.material < b.material;
        }
        return static_cast<int>(a.glassBody) < static_cast<int>(b.glassBody);
    });

    triangles.clear();
    groupTriOffset.assign(groups.size(), 0u);
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const CpuGroup& g = groups[gi];
        groupTriOffset[gi] = static_cast<uint32_t>(triangles.size());
        const uint32_t material = g.material;
        for (size_t i = 0; i + 2 < g.indices.size(); i += 3) {
            const Vec3 p0 = g.vertices[g.indices[i + 0]];
            const Vec3 p1 = g.vertices[g.indices[i + 1]];
            const Vec3 p2 = g.vertices[g.indices[i + 2]];
            const Vec3 fn = g.triangleNormals[i / 3];
            GpuTriangle tri;
            tri.p0[0] = p0.x;
            tri.p0[1] = p0.y;
            tri.p0[2] = p0.z;
            tri.p1[0] = p1.x;
            tri.p1[1] = p1.y;
            tri.p1[2] = p1.z;
            tri.p2[0] = p2.x;
            tri.p2[1] = p2.y;
            tri.p2[2] = p2.z;
            tri.nrm[0] = fn.x;
            tri.nrm[1] = fn.y;
            tri.nrm[2] = fn.z;
            tri.nrm[3] = 0.5f * length(cross(p1 - p0, p2 - p0));
            tri.aux[0] = material;
            tri.aux[1] = static_cast<uint32_t>(gi);
            triangles.push_back(tri);
        }
    }

    // 光源表：仅 emissive 材质三角形，功率 = 亮度 * 面积。
    lights.clear();
    totalLightPower = 0.0f;
    for (uint32_t i = 0; i < static_cast<uint32_t>(triangles.size()); ++i) {
        const GpuTriangle& t = triangles[i];
        const uint32_t mat = t.aux[0];
        if (materials[mat].type != kMatEmissive) {
            continue;
        }
        const Vec3 em = materials[mat].emission;
        const float power = luminance(em) * t.nrm[3];
        if (!(power > 0.0f)) {
            continue;
        }
        GpuLightEntry entry;
        entry.area = t.nrm[3];
        entry.power = power;
        entry.triangle = i;
        totalLightPower += power;
        entry.cumulative = totalLightPower;
        triangles[i].aux[2] = static_cast<uint32_t>(lights.size());
        lights.push_back(entry);
    }

    // 玻璃体世界包围盒，用于相机介质初始化。
    bool anyGlass = false;
    Vec3 mn(0.0f);
    Vec3 mx(0.0f);
    for (const CpuGroup& g : groups) {
        if (!g.glassBody) {
            continue;
        }
        for (const Vec3& p : g.vertices) {
            if (!anyGlass) {
                mn = p;
                mx = p;
                anyGlass = true;
            } else {
                mn = vmin(mn, p);
                mx = vmax(mx, p);
            }
        }
    }
    if (!anyGlass) {
        mn = {0.0f, 0.0f, 0.0f};
        mx = {0.0f, 0.0f, 0.0f};
    }
    glassMin = mn;
    glassMax = mx;
    return true;
}

inline bool SceneData::load(const std::filesystem::path& materialsPath) {
    materials.clear();
    if (!parseMaterials(materialsPath)) {
        return false;
    }
    if (!generateTextures()) {
        return false;
    }
    if (!buildSceneMesh()) {
        return false;
    }
    return true;
}

} // namespace rae
