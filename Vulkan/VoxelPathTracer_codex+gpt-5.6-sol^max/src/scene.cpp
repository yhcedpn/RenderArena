#include "scene.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vpt {
namespace {

constexpr std::array<int, 3> kDimensions{16, 8, 16};
constexpr int kMinY = -1;

struct Quad {
    std::array<Vec3, 4> positions{};
    Vec3 normal{};
    std::uint32_t material{};
    std::uint32_t surface{};
};

std::size_t cellIndex(int x, int yIndex, int z) {
    return (static_cast<std::size_t>(yIndex) * kDimensions[2] +
            static_cast<std::size_t>(z)) *
               kDimensions[0] +
           static_cast<std::size_t>(x);
}

Vec3 gridPoint(const std::array<float, 3>& point) {
    return {point[0], point[1] + static_cast<float>(kMinY), point[2]};
}

void addQuad(std::vector<Quad>& quads, int axis, int sign, float plane, float u0, float v0,
             float u1, float v1, std::uint32_t material, std::uint32_t& nextSurface) {
    const int uAxis = (axis + 1) % 3;
    const int vAxis = (axis + 2) % 3;
    std::array<float, 3> a{};
    std::array<float, 3> b{};
    std::array<float, 3> c{};
    std::array<float, 3> d{};
    a[axis] = b[axis] = c[axis] = d[axis] = plane;
    a[uAxis] = u0;
    a[vAxis] = v0;
    b[uAxis] = u1;
    b[vAxis] = v0;
    c[uAxis] = u1;
    c[vAxis] = v1;
    d[uAxis] = u0;
    d[vAxis] = v1;

    Quad quad{};
    quad.material = material;
    quad.surface = nextSurface++;
    if (sign > 0) {
        quad.positions = {gridPoint(a), gridPoint(b), gridPoint(c), gridPoint(d)};
    } else {
        quad.positions = {gridPoint(a), gridPoint(d), gridPoint(c), gridPoint(b)};
    }
    float* normalComponent = axis == 0 ? &quad.normal.x : (axis == 1 ? &quad.normal.y : &quad.normal.z);
    *normalComponent = static_cast<float>(sign);
    quads.push_back(quad);
}

void greedyOpaqueMesh(const std::vector<std::int32_t>& cells, std::vector<Quad>& quads,
                      std::uint32_t& nextSurface) {
    for (int axis = 0; axis < 3; ++axis) {
        const int uAxis = (axis + 1) % 3;
        const int vAxis = (axis + 2) % 3;
        const int width = kDimensions[uAxis];
        const int height = kDimensions[vAxis];
        for (int sign : {-1, 1}) {
            for (int slice = 0; slice < kDimensions[axis]; ++slice) {
                std::vector<std::int32_t> mask(static_cast<std::size_t>(width * height), -1);
                for (int v = 0; v < height; ++v) {
                    for (int u = 0; u < width; ++u) {
                        std::array<int, 3> cell{};
                        cell[axis] = slice;
                        cell[uAxis] = u;
                        cell[vAxis] = v;
                        const std::int32_t material = cells[cellIndex(cell[0], cell[1], cell[2])];
                        if (material < 0) continue;
                        std::array<int, 3> neighbor = cell;
                        neighbor[axis] += sign;
                        bool neighborFilled = false;
                        if (neighbor[axis] >= 0 && neighbor[axis] < kDimensions[axis]) {
                            neighborFilled = cells[cellIndex(neighbor[0], neighbor[1], neighbor[2])] >= 0;
                        }
                        if (!neighborFilled) {
                            mask[static_cast<std::size_t>(v * width + u)] = material;
                        }
                    }
                }

                // Deterministic row-major greedy expansion. Width is maximized first,
                // then height is extended while every cell of the complete edge matches.
                for (int v = 0; v < height; ++v) {
                    for (int u = 0; u < width;) {
                        const std::int32_t material = mask[static_cast<std::size_t>(v * width + u)];
                        if (material < 0) {
                            ++u;
                            continue;
                        }
                        int rectangleWidth = 1;
                        while (u + rectangleWidth < width &&
                               mask[static_cast<std::size_t>(v * width + u + rectangleWidth)] ==
                                   material) {
                            ++rectangleWidth;
                        }
                        int rectangleHeight = 1;
                        bool canExtend = true;
                        while (v + rectangleHeight < height && canExtend) {
                            for (int x = 0; x < rectangleWidth; ++x) {
                                if (mask[static_cast<std::size_t>((v + rectangleHeight) * width +
                                                                 u + x)] != material) {
                                    canExtend = false;
                                    break;
                                }
                            }
                            if (canExtend) ++rectangleHeight;
                        }
                        for (int y = 0; y < rectangleHeight; ++y) {
                            for (int x = 0; x < rectangleWidth; ++x) {
                                mask[static_cast<std::size_t>((v + y) * width + u + x)] = -1;
                            }
                        }
                        const float plane = static_cast<float>(slice + (sign > 0 ? 1 : 0));
                        addQuad(quads, axis, sign, plane, static_cast<float>(u),
                                static_cast<float>(v), static_cast<float>(u + rectangleWidth),
                                static_cast<float>(v + rectangleHeight),
                                static_cast<std::uint32_t>(material), nextSurface);
                        u += rectangleWidth;
                    }
                }
            }
        }
    }
}

void addGlassBox(std::vector<Quad>& quads, std::uint32_t material,
                 std::uint32_t& nextSurface) {
    // addQuad applies kMinY to its Y coordinate. Supply Y values as grid-space
    // coordinates so the resulting bounds are exactly [0.125, 2.125].
    constexpr float minX = 7.0F;
    constexpr float maxX = 9.0F;
    constexpr float minYGrid = 1.125F;
    constexpr float maxYGrid = 3.125F;
    constexpr float minZ = 7.0F;
    constexpr float maxZ = 9.0F;
    // axis X => (u,v)=(Y,Z)
    addQuad(quads, 0, -1, minX, minYGrid, minZ, maxYGrid, maxZ, material, nextSurface);
    addQuad(quads, 0, 1, maxX, minYGrid, minZ, maxYGrid, maxZ, material, nextSurface);
    // axis Y => (u,v)=(Z,X)
    addQuad(quads, 1, -1, minYGrid, minZ, minX, maxZ, maxX, material, nextSurface);
    addQuad(quads, 1, 1, maxYGrid, minZ, minX, maxZ, maxX, material, nextSurface);
    // axis Z => (u,v)=(X,Y)
    addQuad(quads, 2, -1, minZ, minX, minYGrid, maxX, maxYGrid, material, nextSurface);
    addQuad(quads, 2, 1, maxZ, minX, minYGrid, maxX, maxYGrid, material, nextSurface);
}

float floatFromUint(std::uint32_t value) { return std::bit_cast<float>(value); }

} // namespace

SceneData buildScene(const MaterialSet& materials) {
    const std::uint32_t stone = materials.require("stone");
    const std::uint32_t wood = materials.require("wood");
    const std::uint32_t red = materials.require("red");
    const std::uint32_t blue = materials.require("blue");
    const std::uint32_t roughMetal = materials.require("rough_metal");
    const std::uint32_t mirror = materials.require("mirror");
    const std::uint32_t glass = materials.require("glass");
    const std::uint32_t glow = materials.require("glow");

    std::vector<std::int32_t> cells(
        static_cast<std::size_t>(kDimensions[0] * kDimensions[1] * kDimensions[2]), -1);
    auto setCell = [&](int x, int y, int z, std::uint32_t material) {
        const int yi = y - kMinY;
        if (x < 0 || x >= kDimensions[0] || yi < 0 || yi >= kDimensions[1] || z < 0 ||
            z >= kDimensions[2]) {
            throw std::runtime_error("内部场景坐标越界");
        }
        cells[cellIndex(x, yi, z)] = static_cast<std::int32_t>(material);
    };

    for (int x = 0; x <= 15; ++x)
        for (int z = 0; z <= 15; ++z) setCell(x, -1, z, stone);
    for (int y = 0; y <= 5; ++y)
        for (int z = 0; z <= 15; ++z) setCell(0, y, z, red);
    for (int y = 0; y <= 5; ++y)
        for (int z = 0; z <= 15; ++z) setCell(15, y, z, blue);
    for (int x = 1; x <= 14; ++x)
        for (int y = 0; y <= 5; ++y) setCell(x, y, 0, stone);
    for (int x = 0; x <= 15; ++x) {
        for (int z = 0; z <= 15; ++z) {
            if (!(x >= 6 && x <= 9 && z >= 6 && z <= 9)) setCell(x, 6, z, stone);
        }
    }
    for (int x = 5; x <= 10; ++x)
        for (int y = 1; y <= 4; ++y) setCell(x, y, 1, mirror);
    for (int y = 1; y <= 4; ++y)
        for (int z = 4; z <= 7; ++z) setCell(1, y, z, mirror);
    for (int y = 0; y <= 2; ++y) setCell(3, y, 5, red);
    for (int y = 0; y <= 2; ++y) setCell(12, y, 6, blue);
    setCell(4, 0, 10, wood);
    setCell(4, 1, 10, wood);
    setCell(5, 0, 10, wood);
    setCell(10, 0, 10, roughMetal);
    setCell(10, 1, 10, roughMetal);
    setCell(4, 5, 4, glow);
    setCell(11, 5, 4, glow);
    setCell(4, 5, 11, glow);
    setCell(11, 5, 11, glow);

    std::vector<Quad> quads;
    std::uint32_t nextSurface = 1;
    greedyOpaqueMesh(cells, quads, nextSurface);
    addGlassBox(quads, glass, nextSurface);

    SceneData result{};
    result.gpuMaterials.reserve(materials.materials.size());
    for (const Material& material : materials.materials) {
        GpuMaterial gpu{};
        gpu.baseColor = {material.baseColorSrgb.x, material.baseColorSrgb.y,
                         material.baseColorSrgb.z, 0.0F};
        gpu.emission = {material.emission.x, material.emission.y, material.emission.z, 0.0F};
        gpu.absorption = {material.absorption.x, material.absorption.y, material.absorption.z,
                          0.0F};
        gpu.params = {floatFromUint(static_cast<std::uint32_t>(material.type)),
                      material.metallic, material.roughness, material.ior};
        result.gpuMaterials.push_back(gpu);
    }

    std::uint32_t triangleId = 0;
    for (std::uint32_t materialIndex = 0;
         materialIndex < static_cast<std::uint32_t>(materials.materials.size()); ++materialIndex) {
        GeometryGroup group{};
        group.material = materialIndex;
        group.firstVertex = static_cast<std::uint32_t>(result.vertices.size());
        group.firstIndex = static_cast<std::uint32_t>(result.indices.size());
        group.triangleBase = static_cast<std::uint32_t>(result.triangles.size());
        for (const Quad& quad : quads) {
            if (quad.material != materialIndex) continue;
            const std::uint32_t first = static_cast<std::uint32_t>(result.vertices.size());
            const int axis = std::abs(quad.normal.x) > 0.5F ? 0
                             : std::abs(quad.normal.y) > 0.5F ? 1
                                                               : 2;
            const int uAxis = (axis + 1) % 3;
            const int vAxis = (axis + 2) % 3;
            auto component = [](Vec3 p, int a) { return a == 0 ? p.x : (a == 1 ? p.y : p.z); };
            for (const Vec3 position : quad.positions) {
                GpuVertex vertex{};
                vertex.positionU = {position.x, position.y, position.z,
                                    component(position, uAxis)};
                vertex.normalV = {quad.normal.x, quad.normal.y, quad.normal.z,
                                  component(position, vAxis)};
                result.vertices.push_back(vertex);
            }
            constexpr std::array<std::uint32_t, 6> localIndices{0, 1, 2, 0, 2, 3};
            for (std::uint32_t local : localIndices) result.indices.push_back(first + local);
            for (std::uint32_t triangle = 0; triangle < 2U; ++triangle) {
                GpuTriangle gpuTriangle{};
                gpuTriangle.indicesMaterial = {
                    first + localIndices[triangle * 3U], first + localIndices[triangle * 3U + 1U],
                    first + localIndices[triangle * 3U + 2U], materialIndex};
                gpuTriangle.identity = {quad.surface, triangleId, 0U, 0U};
                result.triangles.push_back(gpuTriangle);
                ++triangleId;
            }
        }
        group.vertexCount = static_cast<std::uint32_t>(result.vertices.size()) - group.firstVertex;
        group.indexCount = static_cast<std::uint32_t>(result.indices.size()) - group.firstIndex;
        if (group.indexCount > 0U) result.groups.push_back(group);
    }

    struct PendingLight {
        GpuLight light{};
        float power{};
    };
    std::vector<PendingLight> pending;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(result.triangles.size()); ++i) {
        const GpuTriangle& triangle = result.triangles[i];
        const std::uint32_t materialIndex = triangle.indicesMaterial[3];
        const Material& material = materials.materials[materialIndex];
        if (material.type != MaterialType::Emissive) continue;
        const Vec3 p0{result.vertices[triangle.indicesMaterial[0]].positionU[0],
                      result.vertices[triangle.indicesMaterial[0]].positionU[1],
                      result.vertices[triangle.indicesMaterial[0]].positionU[2]};
        const Vec3 p1{result.vertices[triangle.indicesMaterial[1]].positionU[0],
                      result.vertices[triangle.indicesMaterial[1]].positionU[1],
                      result.vertices[triangle.indicesMaterial[1]].positionU[2]};
        const Vec3 p2{result.vertices[triangle.indicesMaterial[2]].positionU[0],
                      result.vertices[triangle.indicesMaterial[2]].positionU[1],
                      result.vertices[triangle.indicesMaterial[2]].positionU[2]};
        const Vec3 crossValue = cross(p1 - p0, p2 - p0);
        const float area = 0.5F * length(crossValue);
        const Vec3 normal = normalize(crossValue);
        PendingLight item{};
        item.light.p0Area = {p0.x, p0.y, p0.z, area};
        item.light.p1Cdf = {p1.x, p1.y, p1.z, 0.0F};
        item.light.p2SelectionPdf = {p2.x, p2.y, p2.z, 0.0F};
        item.light.normalMaterial = {normal.x, normal.y, normal.z,
                                     floatFromUint(materialIndex)};
        item.power = area * luminance(material.emission);
        if (item.power > 0.0F) pending.push_back(item);
    }
    float totalPower = 0.0F;
    for (const PendingLight& item : pending) totalPower += item.power;
    if (pending.empty() || totalPower <= 0.0F) {
        throw std::runtime_error("场景没有可采样的暴露发光三角形");
    }
    float cumulative = 0.0F;
    result.lights.reserve(pending.size());
    for (PendingLight& item : pending) {
        const float probability = item.power / totalPower;
        cumulative += probability;
        item.light.p1Cdf[3] = cumulative;
        item.light.p2SelectionPdf[3] = probability;
        result.lights.push_back(item.light);
    }
    result.lights.back().p1Cdf[3] = 1.0F;
    return result;
}

} // namespace vpt
