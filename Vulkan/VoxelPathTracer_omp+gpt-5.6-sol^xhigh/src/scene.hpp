#pragma once

#include "materials.hpp"

#include <cstdint>
#include <vector>

namespace arena {

struct alignas(16) Vertex {
    Vec4 position{};
    Vec4 normalU{};
    Vec4 vMaterialSurface{};
};

struct alignas(16) GpuLight {
    Vec4 p0Area{};
    Vec4 p1Cdf{};
    Vec4 p2Pad{};
    Vec4 normalPad{};
    Vec4 emissionPower{};
    std::uint32_t triangleId{};
    std::uint32_t pad0{};
    std::uint32_t pad1{};
    std::uint32_t pad2{};
};

struct SceneMesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GpuLight> lights;
    std::uint32_t glassMaterial{};
};

SceneMesh buildScene(const MaterialLibrary& materials);

} // 命名空间 arena
