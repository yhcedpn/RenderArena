#include <SDL3/SDL.h>
#include <volk.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using Vec3 = std::array<float, 3>;

struct Material {
    std::string id, type, pattern;
    Vec3 color{}, absorption{}, emission{};
    float metallic{}, roughness{}, ior{}, colorVariation{}, roughnessVariation{};
    uint32_t seed{};
};
struct Level { int size; std::vector<std::array<float, 4>> pixels; };
struct Vertex { Vec3 position, normal; std::array<float, 2> uv; uint32_t material; };
struct Scene { std::vector<Vertex> vertices; std::vector<uint32_t> indices; };

[[noreturn]] static void fail(const std::string& message) { throw std::runtime_error(message); }
static float number(const json& j, const char* key, float min, float max) {
    const auto& v = j.at(key);
    if (!v.is_number()) fail(std::string(key) + " must be a number");
    const double n = v.get<double>();
    if (!std::isfinite(n) || n < min || n > max) fail(std::string(key) + " is outside its range");
    return static_cast<float>(n);
}
static Vec3 triple(const json& j, const char* key, float max) {
    const auto& v = j.at(key);
    if (!v.is_array() || v.size() != 3) fail(std::string(key) + " must contain three numbers");
    Vec3 result{};
    for (size_t i = 0; i < 3; ++i) {
        if (!v[i].is_number()) fail(std::string(key) + " must contain numbers");
        const double n = v[i].get<double>();
        if (!std::isfinite(n) || n < 0 || n > max) fail(std::string(key) + " is outside its range");
        result[i] = static_cast<float>(n);
    }
    return result;
}
static std::vector<Material> loadMaterials(const std::filesystem::path& path, int& size) {
    std::ifstream in(path);
    if (!in) fail("Cannot open materials file: " + path.string());
    try {
        json root = json::parse(in);
        if (!root.at("schemaVersion").is_number_integer() || root.at("schemaVersion").get<int>() != 1)
            fail("Unsupported schemaVersion");
        if (!root.at("textureSize").is_number_integer()) fail("textureSize must be an integer");
        size = root.at("textureSize").get<int>();
        if (size < 16 || size > 1024 || (size & (size - 1)) != 0) fail("Invalid textureSize");
        const auto& entries = root.at("materials");
        if (!entries.is_array()) fail("materials must be an array");
        std::vector<Material> result;
        std::set<std::string> ids;
        for (const auto& j : entries) {
            Material m;
            m.id = j.at("id").get<std::string>();
            m.type = j.at("type").get<std::string>();
            m.pattern = j.at("pattern").get<std::string>();
            if (m.id.empty() || !ids.insert(m.id).second) fail("Empty or duplicate material id: " + m.id);
            if (m.type != "diffuse" && m.type != "rough_metal" && m.type != "mirror" && m.type != "dielectric" && m.type != "emissive") fail("Invalid material type: " + m.id);
            if (m.pattern != "solid" && m.pattern != "stone_noise" && m.pattern != "wood_grain") fail("Invalid pattern: " + m.id);
            m.color = triple(j, "baseColorSRGB", 1);
            m.absorption = triple(j, "absorption", 3.4028234e38f);
            m.emission = triple(j, "emission", 3.4028234e38f);
            m.metallic = number(j, "metallic", 0, 1);
            m.roughness = number(j, "roughness", 0, 1);
            m.ior = number(j, "ior", 1, 3);
            m.colorVariation = number(j, "baseColorVariation", 0, 1);
            m.roughnessVariation = number(j, "roughnessVariation", 0, 1);
            const auto& seed = j.at("textureSeed");
            if (!seed.is_number_integer() || (seed.is_number_unsigned() ? seed.get<uint64_t>() > UINT32_MAX : (seed.get<int64_t>() < 0 || static_cast<uint64_t>(seed.get<int64_t>()) > UINT32_MAX))) fail("Invalid textureSeed: " + m.id);
            m.seed = seed.get<uint32_t>();
            const bool delta = m.type == "mirror" || m.type == "dielectric";
            if ((m.type == "diffuse" && m.metallic != 0) || (m.type == "rough_metal" && (m.metallic != 1 || m.roughness <= 0)) || (m.type == "mirror" && (m.metallic != 1 || m.roughness != 0)) || (m.type == "dielectric" && (m.metallic != 0 || m.roughness != 0 || m.ior <= 1)) || (m.type != "dielectric" && m.ior != 1)) fail("Invalid material model: " + m.id);
            if ((delta || m.type == "emissive") && (m.pattern != "solid" || m.colorVariation != 0 || m.roughnessVariation != 0)) fail("Invalid delta/emissive pattern: " + m.id);
            if (m.type != "dielectric" && m.absorption != Vec3{}) fail("Unexpected absorption: " + m.id);
            if (m.type != "emissive" && m.emission != Vec3{}) fail("Unexpected emission: " + m.id);
            result.push_back(m);
        }
        for (const char* id : {"stone", "wood", "red", "blue", "rough_metal", "mirror", "glass", "glow"})
            if (!ids.contains(id)) fail(std::string("Scene material missing: ") + id);
        return result;
    } catch (const std::exception& e) { fail("Invalid materials file " + path.string() + ": " + e.what()); }
}

static float linear(float s) { return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); }
static float srgb(float l) { return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f; }
static uint32_t hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; return x ^ (x >> 16); }
static float noise(float x, float y, uint32_t seed, int period) {
    int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    auto value = [&](int a, int b) { return static_cast<float>(hash(seed ^ hash(static_cast<uint32_t>(a % period)) ^ hash(static_cast<uint32_t>(b % period) + 12345U)) & 0xffffU) / 65535.0f; };
    const float a = std::lerp(value(ix, iy), value(ix + 1, iy), fx);
    const float b = std::lerp(value(ix, iy + 1), value(ix + 1, iy + 1), fx);
    return std::lerp(a, b, fy);
}
static std::vector<Level> makeTexture(const Material& m, int size) {
    std::vector<Level> levels;
    Level base{size, std::vector<std::array<float, 4>>(static_cast<size_t>(size) * size)};
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        float modulation = 0;
        if (m.pattern == "stone_noise") modulation = 2 * (0.6f * noise(u * 4, v * 4, m.seed, 4) + 0.3f * noise(u * 8, v * 8, m.seed + 1, 8) + 0.1f * noise(u * 16, v * 16, m.seed + 2, 16)) - 1;
        if (m.pattern == "wood_grain") modulation = std::sin(6.2831853f * (v * 8 + 0.4f * (noise(u * 2, v * 2, m.seed, 2) - 0.5f)));
        auto& p = base.pixels[static_cast<size_t>(y) * size + x];
        for (int c = 0; c < 3; ++c) p[c] = std::clamp(m.color[c] + m.colorVariation * modulation, 0.0f, 1.0f);
        p[3] = std::clamp(m.roughness + m.roughnessVariation * modulation, m.type == "rough_metal" ? 0.001f : 0.0f, 1.0f);
    }
    levels.push_back(std::move(base));
    while (levels.back().size > 1) {
        const Level& src = levels.back();
        Level dst{src.size / 2, std::vector<std::array<float, 4>>(static_cast<size_t>(src.size / 2) * (src.size / 2))};
        for (int y = 0; y < dst.size; ++y) for (int x = 0; x < dst.size; ++x) {
            auto& out = dst.pixels[static_cast<size_t>(y) * dst.size + x];
            for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                const auto& p = src.pixels[static_cast<size_t>(2 * y + dy) * src.size + 2 * x + dx];
                for (int c = 0; c < 3; ++c) out[c] += linear(p[c]) * 0.25f;
                out[3] += p[3] * 0.25f;
            }
            for (int c = 0; c < 3; ++c) out[c] = srgb(out[c]);
        }
        levels.push_back(std::move(dst));
    }
    return levels;
}

static void quad(Scene& scene, int axis, int sign, float plane, float u0, float v0, float u1, float v1, uint32_t material) {
    // Cyclic axes ensure cross(U,V) points along the positive normal.
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    Vec3 normal{}; normal[axis] = static_cast<float>(sign);
    Vec3 p{}; p[axis] = plane;
    const uint32_t first = static_cast<uint32_t>(scene.vertices.size());
    for (const auto& uv : {std::array<float, 2>{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}) {
        p[u] = uv[0]; p[v] = uv[1]; scene.vertices.push_back({p, normal, uv, material});
    }
    if (sign > 0) for (uint32_t i : {0U, 1U, 2U, 0U, 2U, 3U}) scene.indices.push_back(first + i);
    else for (uint32_t i : {0U, 2U, 1U, 0U, 3U, 2U}) scene.indices.push_back(first + i);
}
static Scene makeScene(const std::vector<Material>& materials) {
    std::map<std::string, int> ids;
    for (size_t i = 0; i < materials.size(); ++i) ids[materials[i].id] = static_cast<int>(i);
    constexpr int dims[3] = {16, 8, 16};
    std::vector<int> cells(16 * 8 * 16, -1);
    auto index = [](int x, int y, int z) { return static_cast<size_t>((y * 16 + z) * 16 + x); };
    auto put = [&](int x, int y, int z, const char* id) { cells[index(x, y + 1, z)] = ids.at(id); };
    auto fill = [&](int x0, int x1, int y0, int y1, int z0, int z1, const char* id) { for (int x=x0;x<=x1;++x) for (int y=y0;y<=y1;++y) for (int z=z0;z<=z1;++z) put(x,y,z,id); };
    fill(0,15,-1,-1,0,15,"stone"); fill(0,0,0,5,0,15,"red"); fill(15,15,0,5,0,15,"blue"); fill(1,14,0,5,0,0,"stone");
    for (int x=0;x<16;++x) for (int z=0;z<16;++z) if (!(x>=6 && x<=9 && z>=6 && z<=9)) put(x,6,z,"stone");
    fill(5,10,1,4,1,1,"mirror"); fill(1,1,1,4,4,7,"mirror"); fill(3,3,0,2,5,5,"red"); fill(12,12,0,2,6,6,"blue"); fill(4,4,0,1,10,10,"wood"); put(5,0,10,"wood"); fill(10,10,0,1,10,10,"rough_metal");
    for (int x : {4,11}) for (int z : {4,11}) put(x,5,z,"glow");
    Scene scene;
    auto get = [&](const std::array<int,3>& p) { if (p[0]<0 || p[0]>=dims[0] || p[1]<0 || p[1]>=dims[1] || p[2]<0 || p[2]>=dims[2]) return -1; return cells[index(p[0],p[1],p[2])]; };
    for (int axis=0;axis<3;++axis) for (int sign : {-1,1}) {
        int u=(axis+1)%3, v=(axis+2)%3;
        for (int d=0;d<dims[axis];++d) {
            std::vector<int> mask(static_cast<size_t>(dims[u])*dims[v], -1);
            for (int b=0;b<dims[v];++b) for (int a=0;a<dims[u];++a) {
                std::array<int,3> p{}; p[axis]=d; p[u]=a; p[v]=b;
                int mat=get(p); p[axis]+=sign;
                if (mat >= 0 && get(p) < 0) mask[static_cast<size_t>(b)*dims[u]+a]=mat;
            }
            for (int b=0;b<dims[v];++b) for (int a=0;a<dims[u];++a) {
                int mat=mask[static_cast<size_t>(b)*dims[u]+a]; if (mat<0) continue;
                int w=1, h=1; while (a+w<dims[u] && mask[static_cast<size_t>(b)*dims[u]+a+w]==mat) ++w;
                while (b+h<dims[v]) { bool same=true; for(int i=0;i<w;++i) if(mask[static_cast<size_t>(b+h)*dims[u]+a+i]!=mat) same=false; if(!same) break; ++h; }
                for(int j=0;j<h;++j) for(int i=0;i<w;++i) mask[static_cast<size_t>(b+j)*dims[u]+a+i]=-1;
                const float offsetU = u==1 ? -1.0f : 0.0f, offsetV = v==1 ? -1.0f : 0.0f;
                quad(scene,axis,sign,static_cast<float>(d+(sign>0 ? 1 : 0))+(axis==1 ? -1.0f : 0.0f),static_cast<float>(a)+offsetU,static_cast<float>(b)+offsetV,static_cast<float>(a+w)+offsetU,static_cast<float>(b+h)+offsetV,static_cast<uint32_t>(mat));
            }
        }
    }
    // Glass is separate after translation, so its bottom and the floor remain closed.
    const float lo[3]={7,0.125f,7}, hi[3]={9,2.125f,9};
    for(int axis=0;axis<3;++axis) for(int sign : {-1,1}) { int u=(axis+1)%3,v=(axis+2)%3; quad(scene,axis,sign,sign<0?lo[axis]:hi[axis],lo[u],lo[v],hi[u],hi[v],static_cast<uint32_t>(ids.at("glass"))); }
    return scene;
}

static void inspectVulkan() {
    if (volkInitialize() != VK_SUCCESS) fail("Vulkan loader unavailable");
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion && vkEnumerateInstanceVersion(&version) != VK_SUCCESS) fail("Cannot query Vulkan loader version");
    if (version < VK_API_VERSION_1_4) fail("Vulkan loader does not support Vulkan 1.4");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VoxelPathTracer"; app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&info, nullptr, &instance);
    if (result != VK_SUCCESS) fail("vkCreateInstance failed: " + std::to_string(result));
    volkLoadInstance(instance);
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) { vkDestroyInstance(instance, nullptr); fail("No Vulkan physical device available"); }
    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (result != VK_SUCCESS) { vkDestroyInstance(instance, nullptr); fail("Cannot enumerate physical devices"); }
    bool suitable = false;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(device, &properties);
        std::vector<std::string> missing;
        if (properties.apiVersion < VK_API_VERSION_1_4) missing.emplace_back("Vulkan 1.4");
        uint32_t extensionCount = 0;
        result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        if (result != VK_SUCCESS) { missing.emplace_back("device extension enumeration"); }
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (result == VK_SUCCESS) result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
        if (result != VK_SUCCESS) missing.emplace_back("device extension enumeration");
        for (const char* name : {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, VK_EXT_SHADER_OBJECT_EXTENSION_NAME}) {
            if (std::none_of(extensions.begin(), extensions.end(), [&](const auto& e) { return std::string(e.extensionName) == name; })) missing.emplace_back(name);
        }
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceDescriptorBufferFeaturesEXT db{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        VkPhysicalDeviceShaderObjectFeaturesEXT so{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext=&f12; f12.pNext=&f13; f13.pNext=&as; as.pNext=&rt; rt.pNext=&db; db.pNext=&so;
        vkGetPhysicalDeviceFeatures2(device, &features);
        auto require = [&](VkBool32 value, const char* name) { if (!value) missing.emplace_back(name); };
        require(f12.timelineSemaphore,"timelineSemaphore"); require(f12.bufferDeviceAddress,"bufferDeviceAddress"); require(f12.scalarBlockLayout,"scalarBlockLayout");
        require(f13.dynamicRendering,"dynamicRendering"); require(f13.synchronization2,"synchronization2");
        require(as.accelerationStructure,"accelerationStructure"); require(rt.rayTracingPipeline,"rayTracingPipeline"); require(rt.rayTracingPipelineTraceRaysIndirect,"rayTracingPipelineTraceRaysIndirect"); require(db.descriptorBuffer,"descriptorBuffer"); require(so.shaderObject,"shaderObject");
        std::cout << "Physical device " << properties.deviceName << ":";
        if (missing.empty()) { std::cout << " listed capability checks passed (surface and limits not checked)\n"; suitable = true; }
        else { std::cout << " missing"; for (const auto& name : missing) std::cout << " " << name; std::cout << '\n'; }
    }
    vkDestroyInstance(instance, nullptr);
    if (!suitable) fail("No device satisfies the checked Vulkan requirements");
}

int main() {
    try {
        const char* base = SDL_GetBasePath();
        if (!base) fail(std::string("SDL_GetBasePath failed: ") + SDL_GetError());
        int textureSize = 0;
        const auto materials = loadMaterials(std::filesystem::path(base) / "materials.json", textureSize);
        std::vector<std::vector<Level>> textures;
        for (const auto& material : materials) textures.push_back(makeTexture(material, textureSize));
        const Scene scene = makeScene(materials);
        std::cout << "Loaded " << materials.size() << " materials, generated " << textures.front().size() << " mip levels per material and " << scene.indices.size()/3 << " scene triangles.\n";
        inspectVulkan();
        fail("Renderer is not implemented: no Vulkan device, acceleration structures, ray tracing, denoising or presentation are available.");
    } catch (const std::exception& e) { std::cerr << "VoxelPathTracer: " << e.what() << '\n'; return 1; }
}
