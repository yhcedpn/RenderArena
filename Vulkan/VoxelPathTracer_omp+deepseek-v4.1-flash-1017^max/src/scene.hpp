#pragma once

#include "math.hpp"
#include "shared.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vpt {

struct SceneMaterial {
    std::string id;
    std::uint32_t type = 0;
    Vec3 baseColorSRGB;
    float metallic = 0.0f;
    float roughness = 0.0f;
    float ior = 1.0f;
    Vec3 absorption;
    Vec3 emission;
    std::string pattern;
    std::uint32_t textureSeed = 0;
    float baseColorVariation = 0.0f;
    float roughnessVariation = 0.0f;
    std::uint32_t layer = 0;
};

struct TextureLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // 按 layer 连续的 RGBA8 或 R8 数据。
    std::vector<std::uint8_t> pixels;
};

struct SceneData {
    std::vector<SceneMaterial> materials;
    std::uint32_t textureSize = 0;
    std::uint32_t mipLevels = 0;
    std::vector<TextureLevel> baseColorLevels;
    std::vector<TextureLevel> roughnessLevels;

    std::vector<MaterialGpu> gpuMaterials;
    std::vector<TriangleGpu> triangles;
    std::vector<InstanceDescGpu> instances;
    std::vector<LightGpu> lights;
    std::vector<PlaneGpu> planes;
    Vec3 glassMin;
    Vec3 glassMax;

    // 每个材质组（实例顺序）的 BLAS 输入。
    std::vector<std::vector<Vec3>> groupPositions;
    std::vector<std::vector<std::uint32_t>> groupIndices;

    std::uint32_t materialByType(std::uint32_t type) const;
    std::uint32_t materialId(const char* id) const;
};

// 从外部 JSON 读取并校验材质，生成纹理、体素网格、greedy 矩形、光源表。
// 任何错误以 std::runtime_error 报告，消息包含具体路径/字段。
SceneData loadScene(const std::filesystem::path& materialsPath);

} // namespace vpt
