#include "scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace arena {
namespace {

constexpr std::array<std::int32_t, 3> dimensions{16, 8, 16};
constexpr std::int32_t emptyVoxel = -1;

struct MaskCell {
    std::int32_t material{emptyVoxel};
    std::int32_t sign{};

    [[nodiscard]] bool operator==(const MaskCell&) const noexcept = default;
};

struct Quad {
    std::uint32_t material{};
    std::uint32_t surface{};
    std::array<Vec3, 4> positions{};
    std::array<Vec2, 4> uvs{};
    Vec3 normal{};
};

[[nodiscard]] std::size_t voxelOffset(const std::int32_t x, const std::int32_t yIndex,
                                      const std::int32_t z) noexcept
{
    return static_cast<std::size_t>(z * dimensions[0] * dimensions[1] +
                                    yIndex * dimensions[0] + x);
}

[[nodiscard]] std::int32_t getVoxel(const std::vector<std::int32_t>& voxels,
                                    const std::array<std::int32_t, 3>& coordinate) noexcept
{
    if (coordinate[0] < 0 || coordinate[0] >= dimensions[0] || coordinate[1] < 0 ||
        coordinate[1] >= dimensions[1] || coordinate[2] < 0 || coordinate[2] >= dimensions[2]) {
        return emptyVoxel;
    }
    return voxels[voxelOffset(coordinate[0], coordinate[1], coordinate[2])];
}

void putVoxel(std::vector<std::int32_t>& voxels, const std::int32_t x, const std::int32_t y,
              const std::int32_t z, const std::uint32_t material)
{
    const std::int32_t yIndex = y + 1;
    if (x < 0 || x >= dimensions[0] || yIndex < 0 || yIndex >= dimensions[1] || z < 0 ||
        z >= dimensions[2]) {
        throw Error("场景体素坐标越界");
    }
    voxels[voxelOffset(x, yIndex, z)] = static_cast<std::int32_t>(material);
}

[[nodiscard]] Vec3 gridToWorld(const std::array<std::int32_t, 3>& point) noexcept
{
    return {static_cast<float>(point[0]), static_cast<float>(point[1] - 1),
            static_cast<float>(point[2])};
}

[[nodiscard]] std::uint32_t hashSurface(const std::uint32_t axis, const std::int32_t plane,
                                        const std::int32_t sign, const std::uint32_t material,
                                        const std::int32_t u, const std::int32_t v,
                                        const std::int32_t width, const std::int32_t height) noexcept
{
    std::uint32_t hash = 2166136261U;
    const auto mix = [&hash](const std::uint32_t value) {
        hash ^= value;
        hash *= 16777619U;
    };
    mix(axis);
    mix(static_cast<std::uint32_t>(plane));
    mix(static_cast<std::uint32_t>(sign));
    mix(material);
    mix(static_cast<std::uint32_t>(u));
    mix(static_cast<std::uint32_t>(v));
    mix(static_cast<std::uint32_t>(width));
    mix(static_cast<std::uint32_t>(height));
    return hash == 0U ? 1U : hash;
}

void appendGreedyQuads(const std::vector<std::int32_t>& voxels, std::vector<Quad>& quads)
{
    for (std::uint32_t axis = 0; axis < 3U; ++axis) {
        const std::uint32_t uAxis = (axis + 1U) % 3U;
        const std::uint32_t vAxis = (axis + 2U) % 3U;
        const std::int32_t uSize = dimensions[uAxis];
        const std::int32_t vSize = dimensions[vAxis];
        std::vector<MaskCell> mask(static_cast<std::size_t>(uSize * vSize));

        for (std::int32_t plane = 0; plane <= dimensions[axis]; ++plane) {
            for (std::int32_t v = 0; v < vSize; ++v) {
                for (std::int32_t u = 0; u < uSize; ++u) {
                    std::array<std::int32_t, 3> before{};
                    std::array<std::int32_t, 3> after{};
                    before[axis] = plane - 1;
                    after[axis] = plane;
                    before[uAxis] = after[uAxis] = u;
                    before[vAxis] = after[vAxis] = v;
                    const std::int32_t a = getVoxel(voxels, before);
                    const std::int32_t b = getVoxel(voxels, after);
                    MaskCell cell;
                    if (a != emptyVoxel && b == emptyVoxel) {
                        cell = {a, 1};
                    } else if (a == emptyVoxel && b != emptyVoxel) {
                        cell = {b, -1};
                    }
                    mask[static_cast<std::size_t>(v * uSize + u)] = cell;
                }
            }

            for (std::int32_t v = 0; v < vSize; ++v) {
                for (std::int32_t u = 0; u < uSize;) {
                    const std::size_t offset = static_cast<std::size_t>(v * uSize + u);
                    const MaskCell cell = mask[offset];
                    if (cell.material == emptyVoxel) {
                        ++u;
                        continue;
                    }

                    std::int32_t width = 1;
                    while (u + width < uSize &&
                           mask[static_cast<std::size_t>(v * uSize + u + width)] == cell) {
                        ++width;
                    }
                    std::int32_t height = 1;
                    bool canExtend = true;
                    while (v + height < vSize && canExtend) {
                        for (std::int32_t x = 0; x < width; ++x) {
                            if (!(mask[static_cast<std::size_t>((v + height) * uSize + u + x)] == cell)) {
                                canExtend = false;
                                break;
                            }
                        }
                        if (canExtend) {
                            ++height;
                        }
                    }

                    std::array<std::int32_t, 3> base{};
                    std::array<std::int32_t, 3> du{};
                    std::array<std::int32_t, 3> dv{};
                    base[axis] = plane;
                    base[uAxis] = u;
                    base[vAxis] = v;
                    du[uAxis] = width;
                    dv[vAxis] = height;
                    const auto add = [](const std::array<std::int32_t, 3>& lhs,
                                        const std::array<std::int32_t, 3>& rhs) {
                        return std::array<std::int32_t, 3>{lhs[0] + rhs[0], lhs[1] + rhs[1],
                                                          lhs[2] + rhs[2]};
                    };
                    const auto baseDu = add(base, du);
                    const auto baseDv = add(base, dv);
                    const auto baseDuDv = add(baseDu, dv);

                    Quad quad;
                    quad.material = static_cast<std::uint32_t>(cell.material);
                    quad.surface = hashSurface(axis, plane, cell.sign, quad.material, u, v, width, height);
                    quad.normal = {};
                    if (axis == 0U) {
                        quad.normal.x = static_cast<float>(cell.sign);
                    } else if (axis == 1U) {
                        quad.normal.y = static_cast<float>(cell.sign);
                    } else {
                        quad.normal.z = static_cast<float>(cell.sign);
                    }
                    if (cell.sign > 0) {
                        quad.positions = {gridToWorld(base), gridToWorld(baseDu), gridToWorld(baseDuDv),
                                          gridToWorld(baseDv)};
                    } else {
                        quad.positions = {gridToWorld(base), gridToWorld(baseDv), gridToWorld(baseDuDv),
                                          gridToWorld(baseDu)};
                    }
                    quad.uvs = {Vec2{0.0F, 0.0F}, Vec2{static_cast<float>(width), 0.0F},
                                Vec2{static_cast<float>(width), static_cast<float>(height)},
                                Vec2{0.0F, static_cast<float>(height)}};
                    quads.push_back(quad);

                    for (std::int32_t y = 0; y < height; ++y) {
                        for (std::int32_t x = 0; x < width; ++x) {
                            mask[static_cast<std::size_t>((v + y) * uSize + u + x)] = {};
                        }
                    }
                    u += width;
                }
            }
        }
    }
}

void appendBoxQuad(std::vector<Quad>& quads, const std::uint32_t material,
                   const std::uint32_t surface, const Vec3 normal,
                   const std::array<Vec3, 4>& positions, const float width, const float height)
{
    Quad quad;
    quad.material = material;
    quad.surface = surface;
    quad.positions = positions;
    quad.uvs = {Vec2{0.0F, 0.0F}, Vec2{width, 0.0F}, Vec2{width, height}, Vec2{0.0F, height}};
    quad.normal = normal;
    quads.push_back(quad);
}

void appendGlassBox(std::vector<Quad>& quads, const std::uint32_t material)
{
    constexpr float x0 = 7.0F;
    constexpr float x1 = 9.0F;
    constexpr float y0 = 0.125F;
    constexpr float y1 = 2.125F;
    constexpr float z0 = 7.0F;
    constexpr float z1 = 9.0F;
    const std::uint32_t seed = fnv1a("glass-box");
    appendBoxQuad(quads, material, seed ^ 1U, {-1.0F, 0.0F, 0.0F},
                  {{{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}}}, 2.0F, 2.0F);
    appendBoxQuad(quads, material, seed ^ 2U, {1.0F, 0.0F, 0.0F},
                  {{{x1, y0, z1}, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}}}, 2.0F, 2.0F);
    appendBoxQuad(quads, material, seed ^ 3U, {0.0F, -1.0F, 0.0F},
                  {{{x0, y0, z1}, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}}}, 2.0F, 2.0F);
    appendBoxQuad(quads, material, seed ^ 4U, {0.0F, 1.0F, 0.0F},
                  {{{x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}}}, 2.0F, 2.0F);
    appendBoxQuad(quads, material, seed ^ 5U, {0.0F, 0.0F, -1.0F},
                  {{{x1, y0, z0}, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}}}, 2.0F, 2.0F);
    appendBoxQuad(quads, material, seed ^ 6U, {0.0F, 0.0F, 1.0F},
                  {{{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}}}, 2.0F, 2.0F);
}

[[nodiscard]] float luminance(const std::array<float, 3>& value) noexcept
{
    return 0.2126F * value[0] + 0.7152F * value[1] + 0.0722F * value[2];
}

} // namespace

SceneMesh buildShowroom(const MaterialLibrary& materials)
{
    const std::uint32_t stone = materials.indexOf("stone");
    const std::uint32_t wood = materials.indexOf("wood");
    const std::uint32_t red = materials.indexOf("red");
    const std::uint32_t blue = materials.indexOf("blue");
    const std::uint32_t roughMetal = materials.indexOf("rough_metal");
    const std::uint32_t mirror = materials.indexOf("mirror");
    const std::uint32_t glass = materials.indexOf("glass");
    const std::uint32_t glow = materials.indexOf("glow");

    std::vector<std::int32_t> voxels(static_cast<std::size_t>(dimensions[0] * dimensions[1] *
                                                              dimensions[2]),
                                     emptyVoxel);
    for (std::int32_t x = 0; x <= 15; ++x) {
        for (std::int32_t z = 0; z <= 15; ++z) {
            putVoxel(voxels, x, -1, z, stone);
        }
    }
    for (std::int32_t y = 0; y <= 5; ++y) {
        for (std::int32_t z = 0; z <= 15; ++z) {
            putVoxel(voxels, 0, y, z, red);
            putVoxel(voxels, 15, y, z, blue);
        }
        for (std::int32_t x = 1; x <= 14; ++x) {
            putVoxel(voxels, x, y, 0, stone);
        }
    }
    for (std::int32_t x = 0; x <= 15; ++x) {
        for (std::int32_t z = 0; z <= 15; ++z) {
            if (!(x >= 6 && x <= 9 && z >= 6 && z <= 9)) {
                putVoxel(voxels, x, 6, z, stone);
            }
        }
    }
    for (std::int32_t x = 5; x <= 10; ++x) {
        for (std::int32_t y = 1; y <= 4; ++y) {
            putVoxel(voxels, x, y, 1, mirror);
        }
    }
    for (std::int32_t y = 1; y <= 4; ++y) {
        for (std::int32_t z = 4; z <= 7; ++z) {
            putVoxel(voxels, 1, y, z, mirror);
        }
    }
    for (std::int32_t y = 0; y <= 2; ++y) {
        putVoxel(voxels, 3, y, 5, red);
        putVoxel(voxels, 12, y, 6, blue);
    }
    putVoxel(voxels, 4, 0, 10, wood);
    putVoxel(voxels, 4, 1, 10, wood);
    putVoxel(voxels, 5, 0, 10, wood);
    putVoxel(voxels, 10, 0, 10, roughMetal);
    putVoxel(voxels, 10, 1, 10, roughMetal);
    putVoxel(voxels, 4, 5, 4, glow);
    putVoxel(voxels, 11, 5, 4, glow);
    putVoxel(voxels, 4, 5, 11, glow);
    putVoxel(voxels, 11, 5, 11, glow);

    std::vector<Quad> quads;
    appendGreedyQuads(voxels, quads);
    const std::uint32_t greedyCount = static_cast<std::uint32_t>(quads.size());
    appendGlassBox(quads, glass);
    std::stable_sort(quads.begin(), quads.end(), [](const Quad& lhs, const Quad& rhs) {
        return lhs.material < rhs.material;
    });

    SceneMesh mesh;
    mesh.greedyRectangleCount = greedyCount;
    std::uint32_t activeMaterial = std::numeric_limits<std::uint32_t>::max();
    for (const Quad& quad : quads) {
        if (quad.material != activeMaterial) {
            activeMaterial = quad.material;
            mesh.groups.push_back({activeMaterial, static_cast<std::uint32_t>(mesh.triangles.size()), 0U});
        }
        const std::uint32_t vertexBase = static_cast<std::uint32_t>(mesh.vertices.size());
        for (std::size_t vertex = 0; vertex < 4U; ++vertex) {
            mesh.vertices.push_back({{quad.positions[vertex].x, quad.positions[vertex].y,
                                      quad.positions[vertex].z, 1.0F},
                                     {quad.normal.x, quad.normal.y, quad.normal.z, 0.0F},
                                     {quad.uvs[vertex].x, quad.uvs[vertex].y, 0.0F, 0.0F}});
        }
        mesh.indices.insert(mesh.indices.end(), {vertexBase, vertexBase + 1U, vertexBase + 2U,
                                                 vertexBase, vertexBase + 2U, vertexBase + 3U});
        std::uint32_t flags = 0U;
        const MaterialType type = materials.at(quad.material).type;
        if (type == MaterialType::dielectric) {
            flags |= 1U;
        }
        if (type == MaterialType::emissive) {
            flags |= 2U;
        }
        mesh.triangles.push_back({{quad.material, quad.surface, flags, 0U}});
        mesh.triangles.push_back({{quad.material, quad.surface, flags, 0U}});
        mesh.groups.back().triangleCount += 2U;
    }

    std::vector<float> powers;
    powers.reserve(mesh.triangles.size());
    float totalPower = 0.0F;
    for (std::uint32_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
        GpuTriangle& triangle = mesh.triangles[triangleIndex];
        if ((triangle.data[2] & 2U) == 0U) {
            continue;
        }
        const std::uint32_t i0 = mesh.indices[static_cast<std::size_t>(triangleIndex) * 3U];
        const std::uint32_t i1 = mesh.indices[static_cast<std::size_t>(triangleIndex) * 3U + 1U];
        const std::uint32_t i2 = mesh.indices[static_cast<std::size_t>(triangleIndex) * 3U + 2U];
        const Vec3 a{mesh.vertices[i0].position[0], mesh.vertices[i0].position[1],
                     mesh.vertices[i0].position[2]};
        const Vec3 b{mesh.vertices[i1].position[0], mesh.vertices[i1].position[1],
                     mesh.vertices[i1].position[2]};
        const Vec3 c{mesh.vertices[i2].position[0], mesh.vertices[i2].position[1],
                     mesh.vertices[i2].position[2]};
        const float area = 0.5F * length(cross(b - a, c - a));
        const Material& material = materials.at(triangle.data[0]);
        const float power = area * luminance(material.emission);
        if (!(power > 0.0F)) {
            throw Error("暴露发光三角形的功率必须大于零");
        }
        totalPower += power;
        powers.push_back(power);
        mesh.lights.push_back({{a.x, a.y, a.z, area},
                               {b.x, b.y, b.z, totalPower},
                               {c.x, c.y, c.z, power},
                               {triangleIndex, triangle.data[0], triangle.data[1], 0U}});
        triangle.data[3] = static_cast<std::uint32_t>(mesh.lights.size());
    }
    if (mesh.lights.empty() || !(totalPower > 0.0F)) {
        throw Error("场景没有有效暴露发光三角形");
    }
    for (GpuLight& light : mesh.lights) {
        light.bCdf[3] /= totalPower;
        light.cProbability[3] /= totalPower;
    }

    return mesh;
}

static_assert(sizeof(GpuVertex) == 48U);
static_assert(sizeof(GpuTriangle) == 16U);
static_assert(sizeof(GpuLight) == 64U);

} // namespace arena
