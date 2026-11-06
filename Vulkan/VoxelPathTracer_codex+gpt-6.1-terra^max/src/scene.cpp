#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace arena {
namespace {
using Json = nlohmann::json;

[[noreturn]] void invalid(const std::string& field, const std::string& reason)
{
    throw std::runtime_error("materials.json: " + field + ": " + reason);
}
float number(const Json& object, const char* key, float minimum, float maximum)
{
    const auto& value = object.at(key);
    if (!value.is_number()) invalid(key, "expected number");
    const double v = value.get<double>();
    if (!std::isfinite(v) || v < minimum || v > maximum) invalid(key, "out of range");
    return static_cast<float>(v);
}
Color color(const Json& object, const char* key, float maximum)
{
    const auto& value = object.at(key);
    if (!value.is_array() || value.size() != 3) invalid(key, "expected three numbers");
    Color result{};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!value[i].is_number()) invalid(key, "expected three numbers");
        const double v = value[i].get<double>();
        if (!std::isfinite(v) || v < 0 || v > maximum) invalid(key, "out of range");
        result[i] = static_cast<float>(v);
    }
    return result;
}
std::uint32_t integer(const Json& object, const char* key)
{
    const auto& value = object.at(key);
    if (!value.is_number_integer()) invalid(key, "expected nonnegative integer");
    if (value.is_number_unsigned()) {
        const auto n = value.get<std::uint64_t>();
        if (n > std::numeric_limits<std::uint32_t>::max()) invalid(key, "out of uint32 range");
        return static_cast<std::uint32_t>(n);
    }
    const auto n = value.get<std::int64_t>();
    if (n < 0 || static_cast<std::uint64_t>(n) > std::numeric_limits<std::uint32_t>::max())
        invalid(key, "out of uint32 range");
    return static_cast<std::uint32_t>(n);
}
std::string string(const Json& object, const char* key)
{
    const auto& value = object.at(key);
    if (!value.is_string()) invalid(key, "expected string");
    return value.get<std::string>();
}
std::uint32_t hash(std::uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float noise(int x, int y, int frequency, std::uint32_t seed)
{
    x = (x % frequency + frequency) % frequency;
    y = (y % frequency + frequency) % frequency;
    return static_cast<float>(hash(seed ^ hash(static_cast<std::uint32_t>(x)) ^
        hash(static_cast<std::uint32_t>(y) + 0x9e3779b9u)) >> 8) / 16777215.0f;
}
float valueNoise(float u, float v, int frequency, std::uint32_t seed)
{
    const float x = u * static_cast<float>(frequency), y = v * static_cast<float>(frequency);
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float fx = x - std::floor(x), fy = y - std::floor(y);
    fx = fx * fx * (3.0f - 2.0f * fx); fy = fy * fy * (3.0f - 2.0f * fy);
    const float a = std::lerp(noise(ix, iy, frequency, seed), noise(ix + 1, iy, frequency, seed), fx);
    const float b = std::lerp(noise(ix, iy + 1, frequency, seed), noise(ix + 1, iy + 1, frequency, seed), fx);
    return std::lerp(a, b, fy);
}
float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linearToSrgb(float c)
{
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
std::uint8_t unorm(float value)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}
void textures(Material& material, std::uint32_t size)
{
    MipLevel base{size, std::vector<std::uint8_t>(static_cast<std::size_t>(size) * size * 4),
        std::vector<std::uint8_t>(static_cast<std::size_t>(size) * size)};
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            float modulation = 0;
            if (material.pattern == Pattern::stoneNoise) {
                modulation = 2.0f * (0.55f * valueNoise(u, v, 4, material.seed) +
                    0.30f * valueNoise(u, v, 8, material.seed + 1) +
                    0.15f * valueNoise(u, v, 16, material.seed + 2)) - 1.0f;
            } else if (material.pattern == Pattern::woodGrain) {
                const float warp = valueNoise(u, v, 4, material.seed) - 0.5f;
                modulation = std::sin(2.0f * std::numbers::pi_v<float> * (12.0f * v + warp));
            }
            const std::size_t index = static_cast<std::size_t>(y) * size + x;
            for (std::size_t c = 0; c < 3; ++c)
                base.color[index * 4 + c] = unorm(material.baseColor[c] + material.colorVariation * modulation);
            base.color[index * 4 + 3] = 255;
            float roughness = std::clamp(material.roughness + material.roughnessVariation * modulation, 0.0f, 1.0f);
            if (material.type == MaterialType::roughMetal) roughness = std::max(roughness, 1.0f / 255.0f);
            base.roughness[index] = unorm(roughness);
        }
    }
    material.mip.push_back(std::move(base));
    while (size > 1) {
        const auto& previous = material.mip.back();
        const auto nextSize = size / 2;
        MipLevel next{nextSize, std::vector<std::uint8_t>(static_cast<std::size_t>(nextSize) * nextSize * 4),
            std::vector<std::uint8_t>(static_cast<std::size_t>(nextSize) * nextSize)};
        for (std::uint32_t y = 0; y < nextSize; ++y) {
            for (std::uint32_t x = 0; x < nextSize; ++x) {
                Color sum{}; float rough = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy) for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::size_t src = static_cast<std::size_t>(y * 2 + dy) * size + x * 2 + dx;
                    for (std::size_t c = 0; c < 3; ++c)
                        sum[c] += srgbToLinear(static_cast<float>(previous.color[src * 4 + c]) / 255.0f);
                    rough += static_cast<float>(previous.roughness[src]) / 255.0f;
                }
                const std::size_t dst = static_cast<std::size_t>(y) * nextSize + x;
                for (std::size_t c = 0; c < 3; ++c) next.color[dst * 4 + c] = unorm(linearToSrgb(sum[c] * 0.25f));
                next.color[dst * 4 + 3] = 255;
                next.roughness[dst] = unorm(rough * 0.25f);
            }
        }
        material.mip.push_back(std::move(next)); size = nextSize;
    }
}
Material parseMaterial(const Json& j)
{
    if (!j.is_object()) invalid("materials", "each material must be an object");
    Material m{};
    m.id = string(j, "id");
    if (m.id.empty()) invalid("id", "must not be empty");
    const auto type = string(j, "type");
    if (type == "diffuse") m.type = MaterialType::diffuse;
    else if (type == "rough_metal") m.type = MaterialType::roughMetal;
    else if (type == "mirror") m.type = MaterialType::mirror;
    else if (type == "dielectric") m.type = MaterialType::dielectric;
    else if (type == "emissive") m.type = MaterialType::emissive;
    else invalid("type", "unknown value " + type);
    m.baseColor = color(j, "baseColorSRGB", 1);
    m.metallic = number(j, "metallic", 0, 1);
    m.roughness = number(j, "roughness", 0, 1);
    m.ior = number(j, "ior", 1, 3);
    m.absorption = color(j, "absorption", std::numeric_limits<float>::max());
    m.emission = color(j, "emission", std::numeric_limits<float>::max());
    const auto pattern = string(j, "pattern");
    if (pattern == "solid") m.pattern = Pattern::solid;
    else if (pattern == "stone_noise") m.pattern = Pattern::stoneNoise;
    else if (pattern == "wood_grain") m.pattern = Pattern::woodGrain;
    else invalid("pattern", "unknown value " + pattern);
    m.seed = integer(j, "textureSeed");
    m.colorVariation = number(j, "baseColorVariation", 0, 1);
    m.roughnessVariation = number(j, "roughnessVariation", 0, 1);
    // 先按输入的 double 判断语义，再转换为 GPU float；避免小非零值被舍入为零后误通过约束。
    const double metallic = j.at("metallic").get<double>(), roughness = j.at("roughness").get<double>(), ior = j.at("ior").get<double>();
    const auto zero = [&](const char* key) {
        const auto& values = j.at(key);
        return values[0].get<double>() == 0 && values[1].get<double>() == 0 && values[2].get<double>() == 0;
    };
    if ((m.type == MaterialType::diffuse && metallic != 0) ||
        (m.type == MaterialType::roughMetal && (metallic != 1 || roughness <= 0)) ||
        (m.type == MaterialType::mirror && (metallic != 1 || roughness != 0)) ||
        (m.type == MaterialType::dielectric && (metallic != 0 || roughness != 0)))
        invalid(m.id, "type, metallic and roughness are inconsistent");
    if ((m.type == MaterialType::dielectric && ior <= 1) ||
        (m.type != MaterialType::dielectric && ior != 1)) invalid(m.id, "invalid ior for material type");
    if (m.type != MaterialType::dielectric && !zero("absorption")) invalid(m.id, "non-dielectric absorption must be zero");
    if (m.type != MaterialType::emissive && !zero("emission")) invalid(m.id, "non-emissive emission must be zero");
    if ((m.type == MaterialType::mirror || m.type == MaterialType::dielectric || m.type == MaterialType::emissive) &&
        (m.pattern != Pattern::solid || j.at("baseColorVariation").get<double>() != 0 || j.at("roughnessVariation").get<double>() != 0))
        invalid(m.id, "delta and emissive materials require a solid pattern and zero variation");
    return m;
}

using Grid = std::array<int, 16 * 8 * 16>;
int& cell(Grid& grid, int x, int y, int z) { return grid[static_cast<std::size_t>((y + 1) * 256 + z * 16 + x)]; }
int at(const Grid& grid, int x, int y, int z)
{
    if (x < 0 || x >= 16 || y < -1 || y > 6 || z < 0 || z >= 16) return -1;
    return grid[static_cast<std::size_t>((y + 1) * 256 + z * 16 + x)];
}
Vec3 vector(const std::array<float, 3>& p) { return {p[0], p[1], p[2]}; }
void rectangle(Scene& scene, int axis, int sign, float plane, float u0, float v0,
    float u1, float v1, std::uint32_t material, std::uint32_t surface)
{
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    std::array<float, 3> p{}; p[axis] = plane; p[u] = u0; p[v] = v0;
    auto p10 = p, p01 = p, p11 = p; p10[u] = u1; p01[v] = v1; p11[u] = u1; p11[v] = v1;
    std::array<float, 3> n{}; n[axis] = static_cast<float>(sign);
    const Vertex a{vector(p), vector(n), {u0, v0}}, b{vector(p10), vector(n), {u1, v0}},
        c{vector(p11), vector(n), {u1, v1}}, d{vector(p01), vector(n), {u0, v1}};
    if (sign > 0) {
        scene.triangles.push_back({{a, b, c}, material, surface});
        scene.triangles.push_back({{a, c, d}, material, surface});
    } else {
        scene.triangles.push_back({{a, c, b}, material, surface});
        scene.triangles.push_back({{a, d, c}, material, surface});
    }
}
void mesh(Scene& scene, const Grid& grid, std::uint32_t glass)
{
    const std::array<int, 3> lower{0, -1, 0}, extent{16, 8, 16};
    std::uint32_t surface = 0;
    for (int axis = 0; axis < 3; ++axis) for (int sign : {-1, 1}) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        for (int slice = 0; slice < extent[axis]; ++slice) {
            const int width = extent[u], height = extent[v];
            std::vector<int> mask(static_cast<std::size_t>(width * height), -1);
            for (int j = 0; j < height; ++j) for (int i = 0; i < width; ++i) {
                std::array<int, 3> p = lower; p[axis] += slice; p[u] += i; p[v] += j;
                const int mat = at(grid, p[0], p[1], p[2]);
                auto neighbour = p; neighbour[axis] += sign;
                if (mat >= 0 && at(grid, neighbour[0], neighbour[1], neighbour[2]) < 0)
                    mask[static_cast<std::size_t>(j * width + i)] = mat;
            }
            for (int j = 0; j < height; ++j) for (int i = 0; i < width; ++i) {
                const int mat = mask[static_cast<std::size_t>(j * width + i)];
                if (mat < 0) continue;
                int w = 1, h = 1;
                while (i + w < width && mask[static_cast<std::size_t>(j * width + i + w)] == mat) ++w;
                while (j + h < height) {
                    bool row = true;
                    for (int x = 0; x < w; ++x) if (mask[static_cast<std::size_t>((j + h) * width + i + x)] != mat) row = false;
                    if (!row) break;
                    ++h;
                }
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                    mask[static_cast<std::size_t>((j + y) * width + i + x)] = -1;
                rectangle(scene, axis, sign, static_cast<float>(lower[axis] + slice + (sign > 0 ? 1 : 0)),
                    static_cast<float>(lower[u] + i), static_cast<float>(lower[v] + j),
                    static_cast<float>(lower[u] + i + w), static_cast<float>(lower[v] + j + h),
                    static_cast<std::uint32_t>(mat), surface++);
            }
        }
    }
    // 玻璃先合并成闭合体，再上移；不影响不透明地板的暴露面。
    const std::array<float, 3> lo{7.0f, 0.125f, 7.0f}, hi{9.0f, 2.125f, 9.0f};
    for (int axis = 0; axis < 3; ++axis) for (int sign : {-1, 1}) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        rectangle(scene, axis, sign, sign > 0 ? hi[axis] : lo[axis], lo[u], lo[v], hi[u], hi[v], glass, surface++);
    }
}
void lights(Scene& scene)
{
    for (std::size_t i = 0; i < scene.triangles.size(); ++i) {
        const auto& t = scene.triangles[i];
        const auto& m = scene.materials[t.material];
        if (m.type != MaterialType::emissive) continue;
        const auto& a = t.vertices[0].position; const auto& b = t.vertices[1].position; const auto& c = t.vertices[2].position;
        const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z}, ac{c.x - a.x, c.y - a.y, c.z - a.z};
        const Vec3 cross{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x};
        const float area = 0.5f * std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);
        const float power = area * (0.2126f * m.emission[0] + 0.7152f * m.emission[1] + 0.0722f * m.emission[2]);
        if (power > 0) { scene.lightPower += power; scene.lights.push_back({static_cast<std::uint32_t>(i), area, scene.lightPower}); }
    }
}
}

Scene loadScene(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open material file: " + path.string());
    Json root;
    try { file >> root; }
    catch (const Json::exception& e) { throw std::runtime_error("cannot parse " + path.string() + ": " + e.what()); }
    try {
        if (!root.is_object()) invalid("root", "expected object");
        if (integer(root, "schemaVersion") != 1) invalid("schemaVersion", "unsupported version");
        Scene scene{}; scene.textureSize = integer(root, "textureSize");
        if (scene.textureSize < 16 || scene.textureSize > 1024 || (scene.textureSize & (scene.textureSize - 1)) != 0)
            invalid("textureSize", "must be a power of two between 16 and 1024");
        const auto& materials = root.at("materials");
        if (!materials.is_array()) invalid("materials", "expected array");
        std::unordered_map<std::string, int> ids;
        for (const auto& j : materials) {
            auto m = parseMaterial(j);
            if (ids.contains(m.id)) invalid("id", "duplicate " + m.id);
            ids.emplace(m.id, static_cast<int>(scene.materials.size()));
            textures(m, scene.textureSize); scene.materials.push_back(std::move(m));
        }
        const auto id = [&](const char* name) {
            const auto it = ids.find(name);
            if (it == ids.end()) invalid("id", std::string("scene material not found: ") + name);
            return it->second;
        };
        const int stone = id("stone"), red = id("red"), blue = id("blue"), mirror = id("mirror"),
            wood = id("wood"), metal = id("rough_metal"), glass = id("glass"), glow = id("glow");
        Grid grid; grid.fill(-1);
        for (int x = 0; x < 16; ++x) for (int z = 0; z < 16; ++z) cell(grid, x, -1, z) = stone;
        for (int y = 0; y <= 5; ++y) for (int z = 0; z < 16; ++z) { cell(grid, 0, y, z) = red; cell(grid, 15, y, z) = blue; }
        for (int x = 1; x <= 14; ++x) for (int y = 0; y <= 5; ++y) cell(grid, x, y, 0) = stone;
        for (int x = 0; x < 16; ++x) for (int z = 0; z < 16; ++z)
            if (!(x >= 6 && x <= 9 && z >= 6 && z <= 9)) cell(grid, x, 6, z) = stone;
        for (int x = 5; x <= 10; ++x) for (int y = 1; y <= 4; ++y) cell(grid, x, y, 1) = mirror;
        for (int y = 1; y <= 4; ++y) for (int z = 4; z <= 7; ++z) cell(grid, 1, y, z) = mirror;
        for (int y = 0; y <= 2; ++y) { cell(grid, 3, y, 5) = red; cell(grid, 12, y, 6) = blue; }
        cell(grid, 4, 0, 10) = wood; cell(grid, 4, 1, 10) = wood; cell(grid, 5, 0, 10) = wood;
        cell(grid, 10, 0, 10) = metal; cell(grid, 10, 1, 10) = metal;
        for (int x : {4, 11}) for (int z : {4, 11}) cell(grid, x, 5, z) = glow;
        mesh(scene, grid, static_cast<std::uint32_t>(glass)); lights(scene);
        return scene;
    } catch (const Json::exception& e) { throw std::runtime_error("invalid " + path.string() + ": " + e.what()); }
}
}
