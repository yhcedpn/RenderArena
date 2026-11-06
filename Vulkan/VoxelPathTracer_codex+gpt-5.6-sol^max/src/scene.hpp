#pragma once

#include "materials.hpp"

#include <cstdint>
#include <vector>

namespace arena {

struct MeshGroup {
    std::uint32_t materialIndex{};
    std::uint32_t firstIndex{};
    std::uint32_t indexCount{};
    std::uint32_t triangleBase{};
    std::uint32_t triangleCount{};
};

struct SceneGeometry {
    std::vector<VertexGpu> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TriangleGpu> triangles;
    std::vector<MeshGroup> groups;
    std::vector<LightGpu> lights;
    std::uint32_t rectangleCount{};
};

[[nodiscard]] SceneGeometry buildScene(const MaterialSet& materials);

} // namespace arena
