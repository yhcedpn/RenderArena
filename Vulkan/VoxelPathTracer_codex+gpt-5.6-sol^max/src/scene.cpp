#include "scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace arena {
namespace {

constexpr std::array<int, 3> kGridOrigin{0, -1, 0};
constexpr std::array<int, 3> kGridSize{16, 8, 16};

struct Cell {
    int material{-1};
};

struct FaceKey {
    int axis{};
    int sign{};
    int plane{};

    [[nodiscard]] auto tie() const noexcept { return std::tie(axis, sign, plane); }
    [[nodiscard]] bool operator<(const FaceKey& other) const noexcept { return tie() < other.tie(); }
};

struct FaceMask {
    int uAxis{};
    int vAxis{};
    int width{};
    int height{};
    std::vector<int> material;
};

struct GroupData {
    std::vector<VertexGpu> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<TriangleGpu> triangles;
};

std::size_t cellOffset(int x, int y, int z) noexcept {
    return (static_cast<std::size_t>(y + 1) * 16U + static_cast<std::size_t>(z)) * 16U +
           static_cast<std::size_t>(x);
}

bool inside(int x, int y, int z) noexcept {
    return x >= 0 && x < 16 && y >= -1 && y <= 6 && z >= 0 && z < 16;
}

int getCell(const std::vector<Cell>& cells, int x, int y, int z) noexcept {
    return inside(x, y, z) ? cells[cellOffset(x, y, z)].material : -1;
}

void setCell(std::vector<Cell>& cells, int x, int y, int z, int material) {
    if (!inside(x, y, z)) throw std::logic_error("场景格坐标越界");
    cells[cellOffset(x, y, z)].material = material;
}

std::uint32_t requireMaterial(const MaterialSet& materials, const char* id) {
    const auto iterator = materials.indexById.find(id);
    if (iterator == materials.indexById.end()) {
        throw std::runtime_error(std::string("场景引用的材质 id 无法解析: ") + id);
    }
    return iterator->second;
}

std::pair<int, int> faceAxes(int axis) noexcept {
    if (axis == 0) return {2, 1};
    if (axis == 1) return {0, 2};
    return {0, 1};
}

Vec3 arrayToVec(const std::array<float, 3>& value) noexcept { return {value[0], value[1], value[2]}; }

void appendRectangle(GroupData& group, std::uint32_t material, std::uint32_t groupIndex,
                     std::uint32_t surfaceId, int axis, int sign, float plane,
                     int uAxis, int vAxis, float u0, float v0, float u1, float v1) {
    std::array<std::array<float, 3>, 4> coordinates{};
    for (auto& coordinate : coordinates) coordinate[static_cast<std::size_t>(axis)] = plane;
    coordinates[0][static_cast<std::size_t>(uAxis)] = u0;
    coordinates[0][static_cast<std::size_t>(vAxis)] = v0;
    coordinates[1][static_cast<std::size_t>(uAxis)] = u1;
    coordinates[1][static_cast<std::size_t>(vAxis)] = v0;
    coordinates[2][static_cast<std::size_t>(uAxis)] = u1;
    coordinates[2][static_cast<std::size_t>(vAxis)] = v1;
    coordinates[3][static_cast<std::size_t>(uAxis)] = u0;
    coordinates[3][static_cast<std::size_t>(vAxis)] = v1;

    const Vec3 normal = axis == 0 ? Vec3{static_cast<float>(sign), 0.0F, 0.0F}
                       : axis == 1 ? Vec3{0.0F, static_cast<float>(sign), 0.0F}
                                   : Vec3{0.0F, 0.0F, static_cast<float>(sign)};
    const Vec3 c0 = arrayToVec(coordinates[0]);
    const Vec3 c1 = arrayToVec(coordinates[1]);
    const Vec3 c2 = arrayToVec(coordinates[2]);
    if (dot(cross(c1 - c0, c2 - c0), normal) < 0.0F) std::swap(coordinates[1], coordinates[3]);

    const std::uint32_t firstVertex = static_cast<std::uint32_t>(group.vertices.size());
    for (const auto& coordinate : coordinates) {
        const float u = coordinate[static_cast<std::size_t>(uAxis)];
        const float v = coordinate[static_cast<std::size_t>(vAxis)];
        group.vertices.push_back({
            {coordinate[0], coordinate[1], coordinate[2], u},
            {normal.x, normal.y, normal.z, v},
            {material, surfaceId, 0U, 0U},
        });
    }
    group.indices.insert(group.indices.end(), {firstVertex, firstVertex + 1U, firstVertex + 2U,
                                                firstVertex, firstVertex + 2U, firstVertex + 3U});
    const float area = (u1 - u0) * (v1 - v0) * 0.5F;
    const TriangleGpu triangle{{surfaceId, material, kInvalidIndex, groupIndex}, {area, 0.0F, 0.0F, 0.0F}};
    group.triangles.push_back(triangle);
    group.triangles.push_back(triangle);
}

float luminance(Vec3 value) noexcept { return dot(value, Vec3{0.2126F, 0.7152F, 0.0722F}); }

} // namespace

SceneGeometry buildScene(const MaterialSet& materials) {
    const std::uint32_t stone = requireMaterial(materials, "stone");
    const std::uint32_t wood = requireMaterial(materials, "wood");
    const std::uint32_t red = requireMaterial(materials, "red");
    const std::uint32_t blue = requireMaterial(materials, "blue");
    const std::uint32_t roughMetal = requireMaterial(materials, "rough_metal");
    const std::uint32_t mirror = requireMaterial(materials, "mirror");
    const std::uint32_t glass = requireMaterial(materials, "glass");
    const std::uint32_t glow = requireMaterial(materials, "glow");

    std::vector<Cell> cells(16U * 8U * 16U);
    for (int x = 0; x <= 15; ++x) for (int z = 0; z <= 15; ++z) setCell(cells, x, -1, z, static_cast<int>(stone));
    for (int y = 0; y <= 5; ++y) for (int z = 0; z <= 15; ++z) setCell(cells, 0, y, z, static_cast<int>(red));
    for (int y = 0; y <= 5; ++y) for (int z = 0; z <= 15; ++z) setCell(cells, 15, y, z, static_cast<int>(blue));
    for (int x = 1; x <= 14; ++x) for (int y = 0; y <= 5; ++y) setCell(cells, x, y, 0, static_cast<int>(stone));
    for (int x = 0; x <= 15; ++x) {
        for (int z = 0; z <= 15; ++z) {
            if (!(x >= 6 && x <= 9 && z >= 6 && z <= 9)) setCell(cells, x, 6, z, static_cast<int>(stone));
        }
    }
    for (int x = 5; x <= 10; ++x) for (int y = 1; y <= 4; ++y) setCell(cells, x, y, 1, static_cast<int>(mirror));
    for (int y = 1; y <= 4; ++y) for (int z = 4; z <= 7; ++z) setCell(cells, 1, y, z, static_cast<int>(mirror));
    for (int y = 0; y <= 2; ++y) setCell(cells, 3, y, 5, static_cast<int>(red));
    for (int y = 0; y <= 2; ++y) setCell(cells, 12, y, 6, static_cast<int>(blue));
    setCell(cells, 4, 0, 10, static_cast<int>(wood));
    setCell(cells, 4, 1, 10, static_cast<int>(wood));
    setCell(cells, 5, 0, 10, static_cast<int>(wood));
    setCell(cells, 10, 0, 10, static_cast<int>(roughMetal));
    setCell(cells, 10, 1, 10, static_cast<int>(roughMetal));
    for (const auto& point : std::array<std::array<int, 3>, 4>{{{4, 5, 4}, {11, 5, 4}, {4, 5, 11}, {11, 5, 11}}}) {
        setCell(cells, point[0], point[1], point[2], static_cast<int>(glow));
    }

    std::map<FaceKey, FaceMask> masks;
    for (int y = -1; y <= 6; ++y) {
        for (int z = 0; z <= 15; ++z) {
            for (int x = 0; x <= 15; ++x) {
                const int material = getCell(cells, x, y, z);
                if (material < 0) continue;
                const std::array<int, 3> coordinate{x, y, z};
                for (int axis = 0; axis < 3; ++axis) {
                    for (int sign : {-1, 1}) {
                        std::array<int, 3> neighbor = coordinate;
                        neighbor[static_cast<std::size_t>(axis)] += sign;
                        if (getCell(cells, neighbor[0], neighbor[1], neighbor[2]) >= 0) continue;
                        const auto [uAxis, vAxis] = faceAxes(axis);
                        const int plane = coordinate[static_cast<std::size_t>(axis)] - kGridOrigin[static_cast<std::size_t>(axis)] +
                                          (sign > 0 ? 1 : 0);
                        const FaceKey key{axis, sign, plane};
                        auto [iterator, inserted] = masks.try_emplace(key);
                        FaceMask& mask = iterator->second;
                        if (inserted) {
                            mask.uAxis = uAxis;
                            mask.vAxis = vAxis;
                            mask.width = kGridSize[static_cast<std::size_t>(uAxis)];
                            mask.height = kGridSize[static_cast<std::size_t>(vAxis)];
                            mask.material.assign(static_cast<std::size_t>(mask.width * mask.height), -1);
                        }
                        const int u = coordinate[static_cast<std::size_t>(uAxis)] - kGridOrigin[static_cast<std::size_t>(uAxis)];
                        const int v = coordinate[static_cast<std::size_t>(vAxis)] - kGridOrigin[static_cast<std::size_t>(vAxis)];
                        mask.material[static_cast<std::size_t>(v * mask.width + u)] = material;
                    }
                }
            }
        }
    }

    std::vector<GroupData> groupData(materials.materials.size());
    std::uint32_t surfaceId = 1U;
    for (auto& [key, mask] : masks) {
        for (int v = 0; v < mask.height; ++v) {
            for (int u = 0; u < mask.width;) {
                const std::size_t start = static_cast<std::size_t>(v * mask.width + u);
                const int material = mask.material[start];
                if (material < 0) {
                    ++u;
                    continue;
                }
                int rectangleWidth = 1;
                while (u + rectangleWidth < mask.width &&
                       mask.material[static_cast<std::size_t>(v * mask.width + u + rectangleWidth)] == material) {
                    ++rectangleWidth;
                }
                int rectangleHeight = 1;
                bool canGrow = true;
                while (v + rectangleHeight < mask.height && canGrow) {
                    for (int dx = 0; dx < rectangleWidth; ++dx) {
                        if (mask.material[static_cast<std::size_t>((v + rectangleHeight) * mask.width + u + dx)] != material) {
                            canGrow = false;
                            break;
                        }
                    }
                    if (canGrow) ++rectangleHeight;
                }
                for (int dy = 0; dy < rectangleHeight; ++dy) {
                    for (int dx = 0; dx < rectangleWidth; ++dx) {
                        mask.material[static_cast<std::size_t>((v + dy) * mask.width + u + dx)] = -1;
                    }
                }
                const float plane = static_cast<float>(kGridOrigin[static_cast<std::size_t>(key.axis)] + key.plane);
                const float u0 = static_cast<float>(kGridOrigin[static_cast<std::size_t>(mask.uAxis)] + u);
                const float v0 = static_cast<float>(kGridOrigin[static_cast<std::size_t>(mask.vAxis)] + v);
                const float u1 = u0 + static_cast<float>(rectangleWidth);
                const float v1 = v0 + static_cast<float>(rectangleHeight);
                appendRectangle(groupData[static_cast<std::size_t>(material)], static_cast<std::uint32_t>(material),
                                static_cast<std::uint32_t>(material), surfaceId++, key.axis, key.sign, plane,
                                mask.uAxis, mask.vAxis, u0, v0, u1, v1);
                u += rectangleWidth;
            }
        }
    }

    // The glass is one translated 2x2x2 closed solid. Six rectangles are the
    // complete boundary; there are no internal voxel interfaces.
    for (const auto& face : std::array<std::tuple<int, int, float, int, int, float, float, float, float>, 6>{
             std::tuple{0, -1, 7.0F, 2, 1, 7.0F, 0.125F, 9.0F, 2.125F},
             std::tuple{0, 1, 9.0F, 2, 1, 7.0F, 0.125F, 9.0F, 2.125F},
             std::tuple{1, -1, 0.125F, 0, 2, 7.0F, 7.0F, 9.0F, 9.0F},
             std::tuple{1, 1, 2.125F, 0, 2, 7.0F, 7.0F, 9.0F, 9.0F},
             std::tuple{2, -1, 7.0F, 0, 1, 7.0F, 0.125F, 9.0F, 2.125F},
             std::tuple{2, 1, 9.0F, 0, 1, 7.0F, 0.125F, 9.0F, 2.125F},
         }) {
        appendRectangle(groupData[glass], glass, glass, surfaceId++, std::get<0>(face), std::get<1>(face),
                        std::get<2>(face), std::get<3>(face), std::get<4>(face), std::get<5>(face),
                        std::get<6>(face), std::get<7>(face), std::get<8>(face));
    }

    SceneGeometry result{};
    result.rectangleCount = surfaceId - 1U;
    for (std::uint32_t material = 0; material < groupData.size(); ++material) {
        GroupData& source = groupData[material];
        if (source.indices.empty()) continue;
        MeshGroup group{};
        group.materialIndex = material;
        group.firstIndex = static_cast<std::uint32_t>(result.indices.size());
        group.indexCount = static_cast<std::uint32_t>(source.indices.size());
        group.triangleBase = static_cast<std::uint32_t>(result.triangles.size());
        group.triangleCount = static_cast<std::uint32_t>(source.triangles.size());
        const std::uint32_t vertexBase = static_cast<std::uint32_t>(result.vertices.size());
        result.vertices.insert(result.vertices.end(), source.vertices.begin(), source.vertices.end());
        for (std::uint32_t index : source.indices) result.indices.push_back(vertexBase + index);
        const std::uint32_t compactGroupIndex = static_cast<std::uint32_t>(result.groups.size());
        for (TriangleGpu triangle : source.triangles) {
            triangle.metadata.w = compactGroupIndex;
            result.triangles.push_back(triangle);
        }
        result.groups.push_back(group);
    }

    std::vector<float> powers;
    float totalPower = 0.0F;
    for (std::uint32_t triangleIndex = 0; triangleIndex < result.triangles.size(); ++triangleIndex) {
        const TriangleGpu& triangle = result.triangles[triangleIndex];
        const Material& material = materials.materials[triangle.metadata.y];
        if (material.type != MaterialType::emissive) continue;
        const std::uint32_t i0 = result.indices[static_cast<std::size_t>(triangleIndex) * 3U + 0U];
        const std::uint32_t i1 = result.indices[static_cast<std::size_t>(triangleIndex) * 3U + 1U];
        const std::uint32_t i2 = result.indices[static_cast<std::size_t>(triangleIndex) * 3U + 2U];
        const Vec3 p0 = xyz(result.vertices[i0].positionU);
        const Vec3 p1 = xyz(result.vertices[i1].positionU);
        const Vec3 p2 = xyz(result.vertices[i2].positionU);
        const float area = 0.5F * length(cross(p1 - p0, p2 - p0));
        const float power = area * luminance(material.emission);
        if (power <= 0.0F) continue;
        powers.push_back(power);
        totalPower += power;
        result.lights.push_back({
            makeVec4(p0, area),
            makeVec4(p1 - p0, 0.0F),
            makeVec4(p2 - p0, 1.0F / area),
            makeVec4(normalize(cross(p1 - p0, p2 - p0)), 0.0F),
            {triangleIndex, triangle.metadata.y, 0U, 0U},
        });
    }
    if (result.lights.empty() || totalPower <= 0.0F) throw std::runtime_error("场景没有可采样的非零功率发光三角形");
    float cumulative = 0.0F;
    for (std::uint32_t lightIndex = 0; lightIndex < result.lights.size(); ++lightIndex) {
        LightGpu& light = result.lights[lightIndex];
        const float selectionPdf = powers[lightIndex] / totalPower;
        cumulative += selectionPdf;
        light.edge1SelectionPdf.w = selectionPdf;
        light.normalCdf.w = lightIndex + 1U == result.lights.size() ? 1.0F : cumulative;
        const std::uint32_t triangleIndex = light.metadata.x;
        result.triangles[triangleIndex].metadata.z = lightIndex;
        result.triangles[triangleIndex].geometry.y = selectionPdf * light.edge2AreaPdf.w;
    }
    return result;
}

} // namespace arena
