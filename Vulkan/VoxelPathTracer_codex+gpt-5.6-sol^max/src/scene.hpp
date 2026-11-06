#pragma once

#include "materials.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace vpt {

struct alignas(16) GpuVertex {
    std::array<float, 4> positionU{};
    std::array<float, 4> normalV{};
};

struct alignas(16) GpuTriangle {
    // xyz: vertex indices, w: material index
    std::array<std::uint32_t, 4> indicesMaterial{};
    // x: stable surface ID, y: global triangle ID, z/w reserved
    std::array<std::uint32_t, 4> identity{};
};

struct alignas(16) GpuMaterial {
    std::array<float, 4> baseColor{};
    std::array<float, 4> emission{};
    std::array<float, 4> absorption{};
    // x: type encoded as float bits by GLSL uintBitsToFloat inverse,
    // y: metallic, z: roughness, w: ior
    std::array<float, 4> params{};
};

struct alignas(16) GpuLight {
    std::array<float, 4> p0Area{};
    std::array<float, 4> p1Cdf{};
    std::array<float, 4> p2SelectionPdf{};
    // xyz: outward normal, w: material index encoded as float
    std::array<float, 4> normalMaterial{};
};

struct GeometryGroup {
    std::uint32_t material{};
    std::uint32_t firstVertex{};
    std::uint32_t vertexCount{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::uint32_t triangleBase{};
};

struct SceneData {
    std::vector<GpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GpuTriangle> triangles;
    std::vector<GpuMaterial> gpuMaterials;
    std::vector<GpuLight> lights;
    std::vector<GeometryGroup> groups;
};

SceneData buildScene(const MaterialSet& materials);

} // namespace vpt
