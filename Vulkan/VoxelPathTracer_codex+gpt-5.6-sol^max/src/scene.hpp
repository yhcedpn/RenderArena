#pragma once

#include "materials.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace arena {

struct alignas(16) GpuVertex {
    std::array<float, 4> position{};
    std::array<float, 4> normal{};
    std::array<float, 4> uv{};
};

struct alignas(16) GpuTriangle {
    std::array<std::uint32_t, 4> data{};
};

struct alignas(16) GpuLight {
    std::array<float, 4> aArea{};
    std::array<float, 4> bCdf{};
    std::array<float, 4> cProbability{};
    std::array<std::uint32_t, 4> data{};
};

struct GeometryGroup {
    std::uint32_t material{};
    std::uint32_t firstTriangle{};
    std::uint32_t triangleCount{};
};

struct SceneMesh {
    std::vector<GpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GpuTriangle> triangles;
    std::vector<GpuLight> lights;
    std::vector<GeometryGroup> groups;
    std::uint32_t greedyRectangleCount{};
};

[[nodiscard]] SceneMesh buildShowroom(const MaterialLibrary& materials);

} // namespace arena
