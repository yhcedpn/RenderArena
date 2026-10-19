#include "scene.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace arena {
namespace {

constexpr int sizeX = 16;
constexpr int sizeY = 8;
constexpr int sizeZ = 16;
constexpr int empty = -1;

struct Voxels {
    std::array<int, sizeX * sizeY * sizeZ> cells{};

    Voxels() { cells.fill(empty); }

    int& at(int x, int y, int z) {
        return cells[static_cast<std::size_t>((y * sizeZ + z) * sizeX + x)];
    }

    int at(int x, int y, int z) const {
        if (x < 0 || x >= sizeX || y < 0 || y >= sizeY || z < 0 || z >= sizeZ) return empty;
        return cells[static_cast<std::size_t>((y * sizeZ + z) * sizeX + x)];
    }
};

std::uint32_t materialIndex(const MaterialLibrary& materials, const std::string& id) {
    const auto found = materials.indexById.find(id);
    if (found == materials.indexById.end()) throw std::runtime_error("场景引用了缺失材质：" + id);
    return found->second;
}

Vec3 toWorld(const std::array<int, 3>& p) {
    return {static_cast<float>(p[0]), static_cast<float>(p[1] - 1), static_cast<float>(p[2])};
}

std::array<float, 2> uvFor(Vec3 p, int normalAxis) {
    if (normalAxis == 0) return {p.z, p.y};
    if (normalAxis == 1) return {p.x, p.z};
    return {p.x, p.y};
}

void appendQuad(SceneMesh& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 normal, std::uint32_t material, std::uint32_t surface) {
    const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
    const int axis = std::abs(normal.x) > 0.5F ? 0 : (std::abs(normal.y) > 0.5F ? 1 : 2);
    for (Vec3 p : {a, b, c, d}) {
        const auto uv = uvFor(p, axis);
        mesh.vertices.push_back({{p.x, p.y, p.z, 1.0F}, {normal.x, normal.y, normal.z, uv[0]}, {uv[1], static_cast<float>(material), static_cast<float>(surface), 0.0F}});
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1U, base + 2U, base, base + 2U, base + 3U});
}

void fillScene(Voxels& voxels, const MaterialLibrary& materials) {
    const int stone = static_cast<int>(materialIndex(materials, "stone"));
    const int red = static_cast<int>(materialIndex(materials, "red"));
    const int blue = static_cast<int>(materialIndex(materials, "blue"));
    const int mirror = static_cast<int>(materialIndex(materials, "mirror"));
    const int wood = static_cast<int>(materialIndex(materials, "wood"));
    const int metal = static_cast<int>(materialIndex(materials, "rough_metal"));
    const int glow = static_cast<int>(materialIndex(materials, "glow"));

    for (int x = 0; x < 16; ++x) for (int z = 0; z < 16; ++z) voxels.at(x, 0, z) = stone;
    for (int y = 0; y <= 5; ++y) for (int z = 0; z < 16; ++z) voxels.at(0, y + 1, z) = red;
    for (int y = 0; y <= 5; ++y) for (int z = 0; z < 16; ++z) voxels.at(15, y + 1, z) = blue;
    for (int x = 1; x <= 14; ++x) for (int y = 0; y <= 5; ++y) voxels.at(x, y + 1, 0) = stone;
    for (int x = 0; x < 16; ++x) for (int z = 0; z < 16; ++z) {
        if (!(x >= 6 && x <= 9 && z >= 6 && z <= 9)) voxels.at(x, 7, z) = stone;
    }
    for (int x = 5; x <= 10; ++x) for (int y = 1; y <= 4; ++y) voxels.at(x, y + 1, 1) = mirror;
    for (int y = 1; y <= 4; ++y) for (int z = 4; z <= 7; ++z) voxels.at(1, y + 1, z) = mirror;
    for (int y = 0; y <= 2; ++y) voxels.at(3, y + 1, 5) = red;
    for (int y = 0; y <= 2; ++y) voxels.at(12, y + 1, 6) = blue;
    voxels.at(4, 1, 10) = wood;
    voxels.at(4, 2, 10) = wood;
    voxels.at(5, 1, 10) = wood;
    voxels.at(10, 1, 10) = metal;
    voxels.at(10, 2, 10) = metal;
    for (const auto& p : {std::array{4, 6, 4}, std::array{11, 6, 4}, std::array{4, 6, 11}, std::array{11, 6, 11}}) voxels.at(p[0], p[1], p[2]) = glow;
}

void greedyMesh(const Voxels& voxels, SceneMesh& mesh) {
    struct Cell {
        int material{-1};
        int sign{};
        bool operator==(const Cell&) const = default;
    };
    std::vector<Cell> mask(static_cast<std::size_t>(std::max({sizeX * sizeY, sizeY * sizeZ, sizeZ * sizeX})));
    const std::array<int, 3> dimensions{sizeX, sizeY, sizeZ};
    std::uint32_t surface = 1U;

    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;
        const int maskWidth = dimensions[u];
        const int maskHeight = dimensions[v];
        for (int slice = -1; slice < dimensions[d]; ++slice) {
            for (int j = 0; j < maskHeight; ++j) {
                for (int i = 0; i < maskWidth; ++i) {
                    std::array<int, 3> a{};
                    std::array<int, 3> b{};
                    a[d] = slice;
                    b[d] = slice + 1;
                    a[u] = b[u] = i;
                    a[v] = b[v] = j;
                    const int ma = voxels.at(a[0], a[1], a[2]);
                    const int mb = voxels.at(b[0], b[1], b[2]);
                    Cell cell{};
                    if (ma != empty && mb == empty) cell = {ma, 1};
                    else if (ma == empty && mb != empty) cell = {mb, -1};
                    mask[static_cast<std::size_t>(j * maskWidth + i)] = cell;
                }
            }

            for (int j = 0; j < maskHeight; ++j) {
                for (int i = 0; i < maskWidth;) {
                    const Cell cell = mask[static_cast<std::size_t>(j * maskWidth + i)];
                    if (cell.material == empty) {
                        ++i;
                        continue;
                    }
                    int width = 1;
                    while (i + width < maskWidth && mask[static_cast<std::size_t>(j * maskWidth + i + width)] == cell) ++width;
                    int height = 1;
                    bool expandable = true;
                    while (j + height < maskHeight && expandable) {
                        for (int x = 0; x < width; ++x) {
                            if (!(mask[static_cast<std::size_t>((j + height) * maskWidth + i + x)] == cell)) {
                                expandable = false;
                                break;
                            }
                        }
                        if (expandable) ++height;
                    }
                    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) mask[static_cast<std::size_t>((j + y) * maskWidth + i + x)] = {};

                    std::array<int, 3> p{};
                    std::array<int, 3> du{};
                    std::array<int, 3> dv{};
                    p[d] = slice + 1;
                    p[u] = i;
                    p[v] = j;
                    du[u] = width;
                    dv[v] = height;
                    auto add = [](std::array<int, 3> lhs, const std::array<int, 3>& rhs) {
                        for (int axis = 0; axis < 3; ++axis) lhs[axis] += rhs[axis];
                        return lhs;
                    };
                    const Vec3 p0 = toWorld(p);
                    const Vec3 pU = toWorld(add(p, du));
                    const Vec3 pV = toWorld(add(p, dv));
                    const Vec3 pUV = toWorld(add(add(p, du), dv));
                    Vec3 normal{};
                    if (d == 0) normal.x = static_cast<float>(cell.sign);
                    if (d == 1) normal.y = static_cast<float>(cell.sign);
                    if (d == 2) normal.z = static_cast<float>(cell.sign);
                    if (cell.sign > 0) appendQuad(mesh, p0, pU, pUV, pV, normal, static_cast<std::uint32_t>(cell.material), surface++);
                    else appendQuad(mesh, p0, pV, pUV, pU, normal, static_cast<std::uint32_t>(cell.material), surface++);
                    i += width;
                }
            }
        }
    }
}

void appendGlass(SceneMesh& mesh, std::uint32_t material) {
    const float x0 = 7.0F;
    const float x1 = 9.0F;
    const float y0 = 0.125F;
    const float y1 = 2.125F;
    const float z0 = 7.0F;
    const float z1 = 9.0F;
    std::uint32_t surface = 100000U;
    appendQuad(mesh, {x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, {1, 0, 0}, material, surface++);
    appendQuad(mesh, {x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {-1, 0, 0}, material, surface++);
    appendQuad(mesh, {x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {0, 1, 0}, material, surface++);
    appendQuad(mesh, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {0, -1, 0}, material, surface++);
    appendQuad(mesh, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {0, 0, 1}, material, surface++);
    appendQuad(mesh, {x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, {0, 0, -1}, material, surface++);
}

void buildLights(SceneMesh& mesh, const MaterialLibrary& materials) {
    float totalPower = 0.0F;
    for (std::size_t triangle = 0; triangle < mesh.indices.size() / 3U; ++triangle) {
        const Vertex& a = mesh.vertices[mesh.indices[triangle * 3U + 0U]];
        const Vertex& b = mesh.vertices[mesh.indices[triangle * 3U + 1U]];
        const Vertex& c = mesh.vertices[mesh.indices[triangle * 3U + 2U]];
        const auto materialIndexValue = static_cast<std::uint32_t>(a.vMaterialSurface.y);
        const Material& material = materials.materials.at(materialIndexValue);
        if (material.type != MaterialType::Emissive) continue;
        const Vec3 p0{a.position.x, a.position.y, a.position.z};
        const Vec3 p1{b.position.x, b.position.y, b.position.z};
        const Vec3 p2{c.position.x, c.position.y, c.position.z};
        const Vec3 n = normalize(cross(p1 - p0, p2 - p0));
        const float area = 0.5F * length(cross(p1 - p0, p2 - p0));
        const float luminance = material.emission.x * 0.2126F + material.emission.y * 0.7152F + material.emission.z * 0.0722F;
        const float power = area * luminance;
        totalPower += power;
        GpuLight light{};
        light.p0Area = {p0.x, p0.y, p0.z, area};
        light.p1Cdf = {p1.x, p1.y, p1.z, totalPower};
        light.p2Pad = {p2.x, p2.y, p2.z, 0.0F};
        light.normalPad = {n.x, n.y, n.z, 0.0F};
        light.emissionPower = {material.emission.x, material.emission.y, material.emission.z, power};
        light.triangleId = static_cast<std::uint32_t>(triangle);
        mesh.lights.push_back(light);
    }
    if (mesh.lights.empty() || totalPower <= 0.0F) throw std::runtime_error("场景没有非零功率的暴露发光三角形");
    for (GpuLight& light : mesh.lights) light.p1Cdf.w /= totalPower;
}

} // 匿名命名空间

SceneMesh buildScene(const MaterialLibrary& materials) {
    SceneMesh mesh{};
    Voxels voxels{};
    fillScene(voxels, materials);
    greedyMesh(voxels, mesh);
    mesh.glassMaterial = materialIndex(materials, "glass");
    appendGlass(mesh, mesh.glassMaterial);
    buildLights(mesh, materials);
    return mesh;
}

} // 命名空间 arena
