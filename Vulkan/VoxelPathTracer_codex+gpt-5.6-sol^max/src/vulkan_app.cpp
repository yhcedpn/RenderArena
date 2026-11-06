#include "vulkan_app.hpp"

#include "materials.hpp"
#include "math.hpp"
#include "scene.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vpt {
namespace {

constexpr std::uint32_t kFrameSlotCount = 2;
constexpr std::uint32_t kDomainCount = 6;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void checkVk(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        fail(std::string(operation) + " 失败，VkResult=" + std::to_string(result));
    }
}

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

struct UVec4 {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};
    std::uint32_t w{};
};

struct alignas(16) GlobalDataCpu {
    Mat4 currentViewProjection{};
    Mat4 previousViewProjection{};
    Vec4 cameraPositionFrame{};
    Vec4 cameraForwardTanHalfFov{};
    Vec4 cameraRightAspect{};
    Vec4 cameraUpNear{};
    Vec4 jitterCurrentPrevious{};
    UVec4 extentCounts{};
    UVec4 flags{};
    UVec4 sceneIds{};
};

struct alignas(16) PushConstantsCpu {
    UVec4 u0{};
    UVec4 u1{};
    Vec4 f0{};
    Vec4 f1{};
};

struct BufferResource {
    VkBuffer buffer{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};
    VkDeviceSize size{};
    VkDeviceAddress address{};
    void* mapped{};
};

struct ImageResource {
    VkImage image{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    std::uint32_t mipLevels{};
    std::uint32_t layers{};
};

struct AccelerationResource {
    VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
    BufferResource storage{};
    VkDeviceAddress address{};
};

struct FrameSlot {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    VkSemaphore acquired{VK_NULL_HANDLE};
};

struct Camera {
    Vec3 position{8.0F, 2.5F, 20.0F};
    float yaw{};
    float pitch{};

    Camera() { reset(); }

    void reset() {
        position = {8.0F, 2.5F, 20.0F};
        const Vec3 direction = normalize(Vec3{8.0F, 2.0F, 6.0F} - position);
        yaw = std::atan2(direction.x, -direction.z);
        pitch = std::asin(std::clamp(direction.y, -1.0F, 1.0F));
    }

    [[nodiscard]] Vec3 forward() const {
        const float cp = std::cos(pitch);
        return normalize({cp * std::sin(yaw), std::sin(pitch), -cp * std::cos(yaw)});
    }

    [[nodiscard]] Vec3 right() const { return normalize(cross(forward(), {0.0F, 1.0F, 0.0F})); }
};

struct PerPixelResources {
    BufferResource pathsA;
    BufferResource pathsB;
    BufferResource hits;
    BufferResource branchOutputs;
    BufferResource counters;
    BufferResource shadowTasks;
    BufferResource shadowResults;
    BufferResource shadingPoints;
    BufferResource reservoirInitial;
    BufferResource reservoirTemporal;
    BufferResource reservoirSpatial;
    BufferResource reservoirHistory0;
    BufferResource reservoirHistory1;
    BufferResource guidesCurrent;
    BufferResource guidesHistory0;
    BufferResource guidesHistory1;
    BufferResource signalsCurrent;
    BufferResource temporal0;
    BufferResource temporal1;
    BufferResource filterPing;
    BufferResource filterPong;
    BufferResource finalPixels;
    BufferResource indirect;
};

static_assert(sizeof(GlobalDataCpu) == 256);
static_assert(sizeof(PushConstantsCpu) == 64);
static_assert(sizeof(GpuVertex) == 32);
static_assert(sizeof(GpuTriangle) == 32);
static_assert(sizeof(GpuMaterial) == 64);
static_assert(sizeof(GpuLight) == 64);

float halton(std::uint64_t index, std::uint32_t base) {
    float result = 0.0F;
    float fraction = 1.0F;
    while (index > 0U) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

std::vector<std::byte> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) fail("无法打开 shader: " + path.string());
    const std::streamsize size = stream.tellg();
    if (size <= 0 || (size % 4) != 0) fail("shader 文件大小无效: " + path.string());
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(data.data()), size)) {
        fail("读取 shader 失败: " + path.string());
    }
    return data;
}

} // namespace

struct VulkanApp::Impl {
    SDL_Window* window{};
    std::filesystem::path runtimeDirectory;
    bool sdlInitialized{};
    bool running{true};
    bool mouseCaptured{};
    bool focused{true};
    bool minimized{};
    bool framebufferResized{};
    bool cameraCut{true};
    bool historyReset{true};
    bool cameraMovedThisFrame{};

    MaterialSet materialSet;
    SceneData scene;

    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debugMessenger{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queueFamily{};
    VmaAllocator allocator{VK_NULL_HANDLE};
    std::atomic_bool validationFailure{};

    VkPhysicalDeviceProperties2 deviceProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};

    VkCommandPool initializationPool{VK_NULL_HANDLE};
    std::array<FrameSlot, kFrameSlotCount> frameSlots{};
    VkSemaphore timeline{VK_NULL_HANDLE};
    std::uint64_t timelineValue{};

    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFormat swapchainFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    std::vector<VkSemaphore> renderFinishedSemaphores;

    BufferResource globalsBuffer;
    BufferResource vertexBuffer;
    BufferResource indexBuffer;
    BufferResource triangleBuffer;
    BufferResource materialBuffer;
    BufferResource lightBuffer;
    ImageResource baseColorImage;
    ImageResource roughnessImage;
    VkSampler textureSampler{VK_NULL_HANDLE};
    std::vector<AccelerationResource> blas;
    AccelerationResource tlas;
    BufferResource sbtBuffer;
    PerPixelResources pixels;

    VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    BufferResource descriptorBuffer;
    VkDeviceSize descriptorSetSize{};

    VkPipeline rayTracingPipeline{VK_NULL_HANDLE};
    std::array<VkStridedDeviceAddressRegionKHR, 3> raygenRegions{};
    VkStridedDeviceAddressRegionKHR missRegion{};
    VkStridedDeviceAddressRegionKHR hitRegion{};
    VkStridedDeviceAddressRegionKHR callableRegion{};
    std::vector<VkShaderEXT> computeShaders;
    VkShaderEXT vertexShader{VK_NULL_HANDLE};
    VkShaderEXT fragmentShader{VK_NULL_HANDLE};

    enum ComputeShaderIndex : std::size_t {
        Shade,
        MakeIndirect,
        ShadowResolve,
        ResolveBranches,
        RestirInitial,
        RestirTemporal,
        RestirSpatial,
        RestirShadow,
        SvgfTemporal,
        Atrous,
        Compose,
        CommitHistory,
        ComputeShaderCount
    };

    Camera camera;
    Camera previousCamera;
    Mat4 previousViewProjection = Mat4::identity();
    Vec2 previousJitter{};
    std::uint64_t renderedFrame{};
    std::uint32_t historyReadIndex{};
    std::uint32_t historyWriteIndex{};

    std::chrono::steady_clock::time_point lastLoopTime{};
    std::chrono::steady_clock::time_point statisticsStart{};
    std::uint32_t statisticsFrames{};

    ~Impl() { shutdown(); }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData) {
        auto* self = static_cast<Impl*>(userData);
        std::cerr << "Vulkan Validation: "
                  << (callbackData != nullptr && callbackData->pMessage != nullptr
                          ? callbackData->pMessage
                          : "(empty message)")
                  << '\n';
        if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0U) {
            self->validationFailure.store(true);
        }
        return VK_FALSE;
    }

    void initializeWindow() {
        if (!SDL_Init(SDL_INIT_VIDEO)) fail(std::string("SDL_Init 失败: ") + SDL_GetError());
        sdlInitialized = true;
        window = SDL_CreateWindow("VoxelPathTracer — -- FPS, -- ms/frame", 1280, 720,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                      SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window == nullptr) fail(std::string("SDL_CreateWindow 失败: ") + SDL_GetError());
        const char* basePath = SDL_GetBasePath();
        if (basePath == nullptr) fail(std::string("SDL_GetBasePath 失败: ") + SDL_GetError());
        runtimeDirectory = std::filesystem::path(basePath);
        materialSet = loadMaterials(runtimeDirectory / "materials.json");
        scene = buildScene(materialSet);
    }

    void createInstance() {
        checkVk(volkInitialize(), "volkInitialize");
        std::uint32_t loaderVersion = VK_API_VERSION_1_0;
        checkVk(vkEnumerateInstanceVersion(&loaderVersion), "vkEnumerateInstanceVersion");
        if (loaderVersion < VK_API_VERSION_1_4) fail("Vulkan loader 不支持 Vulkan 1.4");

        Uint32 sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
        if (sdlExtensions == nullptr) {
            fail(std::string("SDL_Vulkan_GetInstanceExtensions 失败: ") + SDL_GetError());
        }
        std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);
        std::vector<const char*> layers;

#ifndef NDEBUG
        std::uint32_t layerCount = 0;
        checkVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr),
                "vkEnumerateInstanceLayerProperties(count)");
        std::vector<VkLayerProperties> layerProperties(layerCount);
        checkVk(vkEnumerateInstanceLayerProperties(&layerCount, layerProperties.data()),
                "vkEnumerateInstanceLayerProperties");
        const bool hasValidation = std::ranges::any_of(layerProperties, [](const auto& layer) {
            return std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
        });
        if (!hasValidation) fail("Debug 构建要求 VK_LAYER_KHRONOS_validation，但当前不可用");
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
#endif

        VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        applicationInfo.pApplicationName = "VoxelPathTracer";
        applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        applicationInfo.pEngineName = "RenderArena";
        applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
        applicationInfo.apiVersion = VK_API_VERSION_1_4;

        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &applicationInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        createInfo.ppEnabledLayerNames = layers.data();

#ifndef NDEBUG
        VkValidationFeatureEnableEXT enabledValidationFeatures[] = {
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
        VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validationFeatures.enabledValidationFeatureCount = 1;
        validationFeatures.pEnabledValidationFeatures = enabledValidationFeatures;
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;
        debugCreateInfo.pUserData = this;
        validationFeatures.pNext = &debugCreateInfo;
        createInfo.pNext = &validationFeatures;
#endif

        checkVk(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");
        volkLoadInstanceOnly(instance);
#ifndef NDEBUG
        checkVk(vkCreateDebugUtilsMessengerEXT(instance, &debugCreateInfo, nullptr,
                                               &debugMessenger),
                "vkCreateDebugUtilsMessengerEXT");
#endif
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
            fail(std::string("SDL_Vulkan_CreateSurface 失败: ") + SDL_GetError());
        }
    }

    [[nodiscard]] std::vector<const char*> requiredDeviceExtensions() const {
        return {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
                VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
                VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
                VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME};
    }

    [[nodiscard]] std::optional<std::uint32_t> findQueueFamily(VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, properties.data());
        for (std::uint32_t index = 0; index < count; ++index) {
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &present) !=
                VK_SUCCESS)
                continue;
            if ((properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U && present == VK_TRUE)
                return index;
        }
        return std::nullopt;
    }

    struct FeatureChain {
        VkPhysicalDeviceFeatures2 root{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan14Features v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        VkPhysicalDeviceShaderObjectFeaturesEXT shaderObject{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamic1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamic2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic3{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInput{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};

        FeatureChain() {
            root.pNext = &v12;
            v12.pNext = &v13;
            v13.pNext = &v14;
            v14.pNext = &acceleration;
            acceleration.pNext = &rayTracing;
            rayTracing.pNext = &descriptor;
            descriptor.pNext = &shaderObject;
            shaderObject.pNext = &dynamic1;
            dynamic1.pNext = &dynamic2;
            dynamic2.pNext = &dynamic3;
            dynamic3.pNext = &vertexInput;
        }
    };

    [[nodiscard]] std::vector<std::string> missingCapabilities(
        VkPhysicalDevice candidate) const {
        std::vector<std::string> missing;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_4) missing.emplace_back("Vulkan 1.4");
        std::uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount,
                                             extensions.data());
        std::unordered_set<std::string> names;
        for (const auto& extension : extensions) names.emplace(extension.extensionName);
        for (const char* required : requiredDeviceExtensions()) {
            if (!names.contains(required)) missing.emplace_back(required);
        }
        FeatureChain features;
        vkGetPhysicalDeviceFeatures2(candidate, &features.root);
        const auto require = [&](VkBool32 value, std::string_view name) {
            if (value == VK_FALSE) missing.emplace_back(name);
        };
        require(features.v12.scalarBlockLayout, "scalarBlockLayout");
        require(features.v12.timelineSemaphore, "timelineSemaphore");
        require(features.v12.bufferDeviceAddress, "bufferDeviceAddress");
        require(features.v13.dynamicRendering, "dynamicRendering");
        require(features.v13.synchronization2, "synchronization2");
        require(features.v14.maintenance5, "maintenance5");
        require(features.acceleration.accelerationStructure, "accelerationStructure");
        require(features.rayTracing.rayTracingPipeline, "rayTracingPipeline");
        require(features.rayTracing.rayTracingPipelineTraceRaysIndirect,
                "rayTracingPipelineTraceRaysIndirect");
        require(features.descriptor.descriptorBuffer, "descriptorBuffer");
        require(features.shaderObject.shaderObject, "shaderObject");
        require(features.dynamic1.extendedDynamicState, "extendedDynamicState");
        require(features.dynamic2.extendedDynamicState2, "extendedDynamicState2");
        require(features.dynamic3.extendedDynamicState3PolygonMode,
                "extendedDynamicState3PolygonMode");
        require(features.dynamic3.extendedDynamicState3RasterizationSamples,
                "extendedDynamicState3RasterizationSamples");
        require(features.dynamic3.extendedDynamicState3SampleMask,
                "extendedDynamicState3SampleMask");
        require(features.dynamic3.extendedDynamicState3AlphaToCoverageEnable,
                "extendedDynamicState3AlphaToCoverageEnable");
        require(features.dynamic3.extendedDynamicState3ColorBlendEnable,
                "extendedDynamicState3ColorBlendEnable");
        require(features.dynamic3.extendedDynamicState3ColorBlendEquation,
                "extendedDynamicState3ColorBlendEquation");
        require(features.dynamic3.extendedDynamicState3ColorWriteMask,
                "extendedDynamicState3ColorWriteMask");
        require(features.vertexInput.vertexInputDynamicState, "vertexInputDynamicState");
        if (!findQueueFamily(candidate).has_value()) missing.emplace_back("graphics+present queue");
        return missing;
    }

    void selectPhysicalDevice() {
        std::uint32_t count = 0;
        checkVk(vkEnumeratePhysicalDevices(instance, &count, nullptr),
                "vkEnumeratePhysicalDevices(count)");
        if (count == 0U) fail("未找到 Vulkan 物理设备");
        std::vector<VkPhysicalDevice> candidates(count);
        checkVk(vkEnumeratePhysicalDevices(instance, &count, candidates.data()),
                "vkEnumeratePhysicalDevices");
        std::vector<std::vector<std::string>> reports;
        for (VkPhysicalDevice candidate : candidates) {
            reports.push_back(missingCapabilities(candidate));
            if (!reports.back().empty()) continue;
            const auto family = findQueueFamily(candidate);
            if (!family.has_value()) continue;
            physicalDevice = candidate;
            queueFamily = *family;
            break;
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            std::ostringstream message;
            message << "没有设备满足必需能力";
            for (std::size_t i = 0; i < reports.size(); ++i) {
                message << "；候选 " << (i + 1U) << " 缺少 ";
                for (std::size_t j = 0; j < reports[i].size(); ++j) {
                    if (j != 0U) message << ", ";
                    message << reports[i][j];
                }
            }
            fail(message.str());
        }

        deviceProperties.pNext = &rtProperties;
        rtProperties.pNext = &asProperties;
        asProperties.pNext = &descriptorProperties;
        vkGetPhysicalDeviceProperties2(physicalDevice, &deviceProperties);

        const auto requireSampledFormat = [&](VkFormat format, std::string_view name) {
            VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
            vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &properties);
            if ((properties.formatProperties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U) {
                fail(std::string(name) + " 不支持 sampled image");
            }
        };
        requireSampledFormat(VK_FORMAT_R8G8B8A8_SRGB, "VK_FORMAT_R8G8B8A8_SRGB");
        requireSampledFormat(VK_FORMAT_R8_UNORM, "VK_FORMAT_R8_UNORM");
    }

    void createDevice() {
        FeatureChain enabled;
        enabled.v12.scalarBlockLayout = VK_TRUE;
        enabled.v12.timelineSemaphore = VK_TRUE;
        enabled.v12.bufferDeviceAddress = VK_TRUE;
        enabled.v13.dynamicRendering = VK_TRUE;
        enabled.v13.synchronization2 = VK_TRUE;
        enabled.v14.maintenance5 = VK_TRUE;
        enabled.acceleration.accelerationStructure = VK_TRUE;
        enabled.rayTracing.rayTracingPipeline = VK_TRUE;
        enabled.rayTracing.rayTracingPipelineTraceRaysIndirect = VK_TRUE;
        enabled.descriptor.descriptorBuffer = VK_TRUE;
        enabled.shaderObject.shaderObject = VK_TRUE;
        enabled.dynamic1.extendedDynamicState = VK_TRUE;
        enabled.dynamic2.extendedDynamicState2 = VK_TRUE;
        enabled.dynamic3.extendedDynamicState3PolygonMode = VK_TRUE;
        enabled.dynamic3.extendedDynamicState3RasterizationSamples = VK_TRUE;
        enabled.dynamic3.extendedDynamicState3SampleMask = VK_TRUE;
        enabled.dynamic3.extendedDynamicState3AlphaToCoverageEnable = VK_TRUE;
        enabled.dynamic3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
        enabled.dynamic3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
        enabled.dynamic3.extendedDynamicState3ColorWriteMask = VK_TRUE;
        enabled.vertexInput.vertexInputDynamicState = VK_TRUE;

        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &priority;
        const std::vector<const char*> extensions = requiredDeviceExtensions();
        VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createInfo.pNext = &enabled.root;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        checkVk(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "vkCreateDevice");
        volkLoadDevice(device);
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        allocatorInfo.physicalDevice = physicalDevice;
        allocatorInfo.device = device;
        allocatorInfo.instance = instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
        VmaVulkanFunctions functions{};
        checkVk(vmaImportVulkanFunctionsFromVolk(&allocatorInfo, &functions),
                "vmaImportVulkanFunctionsFromVolk");
        allocatorInfo.pVulkanFunctions = &functions;
        checkVk(vmaCreateAllocator(&allocatorInfo, &allocator), "vmaCreateAllocator");

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        checkVk(vkCreateCommandPool(device, &poolInfo, nullptr, &initializationPool),
                "vkCreateCommandPool(initialization)");
    }

    BufferResource createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                bool hostVisible = false) const {
        BufferResource resource{};
        resource.size = size;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = hostVisible ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                                           : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (hostVisible) {
            allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        VmaAllocationInfo resultInfo{};
        checkVk(vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &resource.buffer,
                                &resource.allocation, &resultInfo),
                "vmaCreateBuffer");
        resource.mapped = resultInfo.pMappedData;
        if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0U) {
            VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            addressInfo.buffer = resource.buffer;
            resource.address = vkGetBufferDeviceAddress(device, &addressInfo);
            if (resource.address == 0U) fail("vkGetBufferDeviceAddress 返回 0");
        }
        return resource;
    }

    void destroyBuffer(BufferResource& resource) const {
        if (resource.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, resource.buffer, resource.allocation);
            resource = {};
        }
    }

    ImageResource createImage(std::uint32_t width, std::uint32_t height,
                              std::uint32_t mipLevels, std::uint32_t layers,
                              VkFormat format, VkImageUsageFlags usage) const {
        ImageResource resource{};
        resource.format = format;
        resource.mipLevels = mipLevels;
        resource.layers = layers;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = layers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        checkVk(vmaCreateImage(allocator, &imageInfo, &allocationInfo, &resource.image,
                               &resource.allocation, nullptr),
                "vmaCreateImage");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = resource.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.layerCount = layers;
        checkVk(vkCreateImageView(device, &viewInfo, nullptr, &resource.view),
                "vkCreateImageView(texture)");
        return resource;
    }

    void destroyImage(ImageResource& resource) const {
        if (resource.view != VK_NULL_HANDLE) vkDestroyImageView(device, resource.view, nullptr);
        if (resource.image != VK_NULL_HANDLE)
            vmaDestroyImage(allocator, resource.image, resource.allocation);
        resource = {};
    }

    void immediateSubmit(const std::function<void(VkCommandBuffer)>& recorder) const {
        VkCommandBufferAllocateInfo allocationInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocationInfo.commandPool = initializationPool;
        allocationInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocationInfo.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        checkVk(vkAllocateCommandBuffers(device, &allocationInfo, &commandBuffer),
                "vkAllocateCommandBuffers(initialization)");
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo),
                "vkBeginCommandBuffer(initialization)");
        recorder(commandBuffer);
        checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(initialization)");

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        checkVk(vkCreateFence(device, &fenceInfo, nullptr, &fence),
                "vkCreateFence(initialization)");
        VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = commandBuffer;
        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandInfo;
        checkVk(vkQueueSubmit2(queue, 1, &submitInfo, fence),
                "vkQueueSubmit2(initialization)");
        checkVk(vkWaitForFences(device, 1, &fence, VK_TRUE,
                                std::numeric_limits<std::uint64_t>::max()),
                "vkWaitForFences(initialization)");
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, initializationPool, 1, &commandBuffer);
    }

    void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 sourceStage,
                       VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                       VkAccessFlags2 destinationAccess) const {
        VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        memory.srcStageMask = sourceStage;
        memory.srcAccessMask = sourceAccess;
        memory.dstStageMask = destinationStage;
        memory.dstAccessMask = destinationAccess;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }

    BufferResource makeStaging(const void* data, VkDeviceSize size) const {
        BufferResource staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, data, static_cast<std::size_t>(size));
        checkVk(vmaFlushAllocation(allocator, staging.allocation, 0, size),
                "vmaFlushAllocation(staging)");
        return staging;
    }

    void createTextureResources(std::vector<BufferResource>& stagingBuffers,
                                std::vector<std::vector<VkBufferImageCopy>>& copyRegions) {
        const std::uint32_t mipLevels =
            static_cast<std::uint32_t>(materialSet.baseColorMips.front().size());
        const std::uint32_t layers = static_cast<std::uint32_t>(materialSet.materials.size());
        baseColorImage = createImage(materialSet.textureSize, materialSet.textureSize, mipLevels,
                                     layers, VK_FORMAT_R8G8B8A8_SRGB,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT);
        roughnessImage = createImage(materialSet.textureSize, materialSet.textureSize, mipLevels,
                                     layers, VK_FORMAT_R8_UNORM,
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT);

        auto buildStaging = [&](const std::vector<std::vector<TextureMip>>& chains) {
            VkDeviceSize totalSize = 0;
            for (const auto& chain : chains)
                for (const TextureMip& mip : chain) totalSize += mip.texels.size();
            std::vector<std::byte> bytes(static_cast<std::size_t>(totalSize));
            std::vector<VkBufferImageCopy> regions;
            VkDeviceSize offset = 0;
            for (std::uint32_t layer = 0; layer < layers; ++layer) {
                for (std::uint32_t mipIndex = 0; mipIndex < mipLevels; ++mipIndex) {
                    const TextureMip& mip = chains[layer][mipIndex];
                    std::memcpy(bytes.data() + offset, mip.texels.data(), mip.texels.size());
                    VkBufferImageCopy region{};
                    region.bufferOffset = offset;
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.mipLevel = mipIndex;
                    region.imageSubresource.baseArrayLayer = layer;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = {mip.width, mip.height, 1};
                    regions.push_back(region);
                    offset += mip.texels.size();
                }
            }
            stagingBuffers.push_back(makeStaging(bytes.data(), totalSize));
            copyRegions.push_back(std::move(regions));
        };
        buildStaging(materialSet.baseColorMips);
        buildStaging(materialSet.roughnessMips);

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.minLod = 0.0F;
        samplerInfo.maxLod = static_cast<float>(mipLevels - 1U);
        samplerInfo.maxAnisotropy = 1.0F;
        checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &textureSampler),
                "vkCreateSampler");
    }

    void createStaticSceneResources() {
        constexpr VkBufferUsageFlags shaderBufferUsage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vertexBuffer = createBuffer(
            scene.vertices.size() * sizeof(GpuVertex),
            shaderBufferUsage | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        indexBuffer = createBuffer(
            scene.indices.size() * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        triangleBuffer =
            createBuffer(scene.triangles.size() * sizeof(GpuTriangle), shaderBufferUsage);
        materialBuffer =
            createBuffer(scene.gpuMaterials.size() * sizeof(GpuMaterial), shaderBufferUsage);
        lightBuffer = createBuffer(scene.lights.size() * sizeof(GpuLight), shaderBufferUsage);
        globalsBuffer = createBuffer(sizeof(GlobalDataCpu),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     true);

        struct UploadPair {
            BufferResource* destination;
            BufferResource staging;
        };
        std::vector<UploadPair> uploads;
        auto addUpload = [&](BufferResource& destination, const void* data, VkDeviceSize size) {
            uploads.push_back({&destination, makeStaging(data, size)});
        };
        addUpload(vertexBuffer, scene.vertices.data(), vertexBuffer.size);
        addUpload(indexBuffer, scene.indices.data(), indexBuffer.size);
        addUpload(triangleBuffer, scene.triangles.data(), triangleBuffer.size);
        addUpload(materialBuffer, scene.gpuMaterials.data(), materialBuffer.size);
        addUpload(lightBuffer, scene.lights.data(), lightBuffer.size);

        std::vector<BufferResource> textureStaging;
        std::vector<std::vector<VkBufferImageCopy>> textureRegions;
        createTextureResources(textureStaging, textureRegions);

        immediateSubmit([&](VkCommandBuffer commandBuffer) {
            for (const UploadPair& upload : uploads) {
                VkBufferCopy copy{0, 0, upload.destination->size};
                vkCmdCopyBuffer(commandBuffer, upload.staging.buffer,
                                upload.destination->buffer, 1, &copy);
            }
            std::array<VkImageMemoryBarrier2, 2> toTransfer{};
            const std::array<ImageResource*, 2> images{&baseColorImage, &roughnessImage};
            for (std::size_t i = 0; i < images.size(); ++i) {
                toTransfer[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toTransfer[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                toTransfer[i].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                toTransfer[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toTransfer[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toTransfer[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toTransfer[i].image = images[i]->image;
                toTransfer[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toTransfer[i].subresourceRange.levelCount = images[i]->mipLevels;
                toTransfer[i].subresourceRange.layerCount = images[i]->layers;
            }
            VkDependencyInfo transferDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            transferDependency.imageMemoryBarrierCount =
                static_cast<std::uint32_t>(toTransfer.size());
            transferDependency.pImageMemoryBarriers = toTransfer.data();
            vkCmdPipelineBarrier2(commandBuffer, &transferDependency);
            vkCmdCopyBufferToImage(commandBuffer, textureStaging[0].buffer,
                                   baseColorImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(textureRegions[0].size()),
                                   textureRegions[0].data());
            vkCmdCopyBufferToImage(commandBuffer, textureStaging[1].buffer,
                                   roughnessImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<std::uint32_t>(textureRegions[1].size()),
                                   textureRegions[1].data());

            std::array<VkImageMemoryBarrier2, 2> toShader{};
            for (std::size_t i = 0; i < images.size(); ++i) {
                toShader[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                toShader[i].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                toShader[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toShader[i].dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                toShader[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                toShader[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toShader[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toShader[i].image = images[i]->image;
                toShader[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toShader[i].subresourceRange.levelCount = images[i]->mipLevels;
                toShader[i].subresourceRange.layerCount = images[i]->layers;
            }
            VkMemoryBarrier2 bufferBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            bufferBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            bufferBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            bufferBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                         VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            bufferBarrier.dstAccessMask =
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            VkDependencyInfo shaderDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            shaderDependency.memoryBarrierCount = 1;
            shaderDependency.pMemoryBarriers = &bufferBarrier;
            shaderDependency.imageMemoryBarrierCount =
                static_cast<std::uint32_t>(toShader.size());
            shaderDependency.pImageMemoryBarriers = toShader.data();
            vkCmdPipelineBarrier2(commandBuffer, &shaderDependency);
        });

        for (UploadPair& upload : uploads) destroyBuffer(upload.staging);
        for (BufferResource& staging : textureStaging) destroyBuffer(staging);
    }

    AccelerationResource createAcceleration(VkAccelerationStructureTypeKHR type,
                                            VkDeviceSize size) const {
        AccelerationResource resource{};
        resource.storage = createBuffer(
            size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        VkAccelerationStructureCreateInfoKHR createInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.buffer = resource.storage.buffer;
        createInfo.size = size;
        createInfo.type = type;
        checkVk(vkCreateAccelerationStructureKHR(device, &createInfo, nullptr, &resource.handle),
                "vkCreateAccelerationStructureKHR");
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addressInfo.accelerationStructure = resource.handle;
        resource.address = vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
        return resource;
    }

    void destroyAcceleration(AccelerationResource& resource) const {
        if (resource.handle != VK_NULL_HANDLE)
            vkDestroyAccelerationStructureKHR(device, resource.handle, nullptr);
        destroyBuffer(resource.storage);
        resource = {};
    }

    void buildAccelerationStructures() {
        struct PendingBuild {
            VkAccelerationStructureGeometryKHR geometry{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            VkAccelerationStructureBuildGeometryInfoKHR info{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            VkAccelerationStructureBuildRangeInfoKHR range{};
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            AccelerationResource uncompacted;
        };
        std::vector<PendingBuild> builds(scene.groups.size());
        VkDeviceSize maximumScratch = 0;
        for (std::size_t i = 0; i < scene.groups.size(); ++i) {
            const GeometryGroup& group = scene.groups[i];
            PendingBuild& build = builds[i];
            build.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            build.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            build.geometry.geometry.triangles.sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            build.geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            build.geometry.geometry.triangles.vertexData.deviceAddress = vertexBuffer.address;
            build.geometry.geometry.triangles.vertexStride = sizeof(GpuVertex);
            build.geometry.geometry.triangles.maxVertex =
                static_cast<std::uint32_t>(scene.vertices.size() - 1U);
            build.geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
            build.geometry.geometry.triangles.indexData.deviceAddress =
                indexBuffer.address + group.firstIndex * sizeof(std::uint32_t);
            build.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                               VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
            build.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.info.geometryCount = 1;
            build.info.pGeometries = &build.geometry;
            build.range.primitiveCount = group.indexCount / 3U;
            const std::uint32_t primitiveCount = build.range.primitiveCount;
            vkGetAccelerationStructureBuildSizesKHR(
                device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build.info,
                &primitiveCount, &build.sizes);
            build.uncompacted = createAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                   build.sizes.accelerationStructureSize);
            build.info.dstAccelerationStructure = build.uncompacted.handle;
            maximumScratch = std::max(maximumScratch, build.sizes.buildScratchSize);
        }

        const VkDeviceSize scratchAlignment =
            asProperties.minAccelerationStructureScratchOffsetAlignment;
        BufferResource scratch = createBuffer(
            maximumScratch + scratchAlignment - 1U,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        const VkDeviceAddress scratchAddress = alignUp(scratch.address, scratchAlignment);
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        queryInfo.queryCount = static_cast<std::uint32_t>(builds.size());
        VkQueryPool queryPool = VK_NULL_HANDLE;
        checkVk(vkCreateQueryPool(device, &queryInfo, nullptr, &queryPool),
                "vkCreateQueryPool(compaction)");

        immediateSubmit([&](VkCommandBuffer commandBuffer) {
            vkCmdResetQueryPool(commandBuffer, queryPool, 0,
                                static_cast<std::uint32_t>(builds.size()));
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(builds.size()); ++i) {
                PendingBuild& build = builds[i];
                build.info.scratchData.deviceAddress = scratchAddress;
                const VkAccelerationStructureBuildRangeInfoKHR* range = &build.range;
                vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &build.info, &range);
                memoryBarrier(commandBuffer,
                              VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                              VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                              VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                              VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                  VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
                vkCmdWriteAccelerationStructuresPropertiesKHR(
                    commandBuffer, 1, &build.uncompacted.handle,
                    VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, queryPool, i);
            }
        });

        std::vector<VkDeviceSize> compactSizes(builds.size());
        checkVk(vkGetQueryPoolResults(device, queryPool, 0,
                                      static_cast<std::uint32_t>(compactSizes.size()),
                                      compactSizes.size() * sizeof(VkDeviceSize),
                                      compactSizes.data(), sizeof(VkDeviceSize),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                "vkGetQueryPoolResults(compaction)");
        blas.resize(builds.size());
        for (std::size_t i = 0; i < builds.size(); ++i) {
            if (compactSizes[i] == 0U) fail("BLAS 压缩查询返回 0");
            blas[i] = createAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                         compactSizes[i]);
        }

        std::vector<VkAccelerationStructureInstanceKHR> instances(builds.size());
        for (std::size_t i = 0; i < builds.size(); ++i) {
            VkAccelerationStructureInstanceKHR instanceData{};
            instanceData.transform.matrix[0][0] = 1.0F;
            instanceData.transform.matrix[1][1] = 1.0F;
            instanceData.transform.matrix[2][2] = 1.0F;
            instanceData.instanceCustomIndex = scene.groups[i].triangleBase;
            instanceData.mask = 0xffU;
            instanceData.instanceShaderBindingTableRecordOffset = 0;
            instanceData.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instanceData.accelerationStructureReference = blas[i].address;
            instances[i] = instanceData;
        }
        BufferResource instanceBuffer = createBuffer(
            instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            true);
        std::memcpy(instanceBuffer.mapped, instances.data(),
                    static_cast<std::size_t>(instanceBuffer.size));
        checkVk(vmaFlushAllocation(allocator, instanceBuffer.allocation, 0, instanceBuffer.size),
                "vmaFlushAllocation(TLAS instances)");

        VkAccelerationStructureGeometryKHR tlasGeometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeometry.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tlasGeometry.geometry.instances.data.deviceAddress = instanceBuffer.address;
        VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuild.geometryCount = 1;
        tlasBuild.pGeometries = &tlasGeometry;
        const std::uint32_t instanceCount = static_cast<std::uint32_t>(instances.size());
        VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(
            device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
            &instanceCount, &tlasSizes);
        tlas = createAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                  tlasSizes.accelerationStructureSize);
        BufferResource tlasScratch = createBuffer(
            tlasSizes.buildScratchSize + scratchAlignment - 1U,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        tlasBuild.dstAccelerationStructure = tlas.handle;
        tlasBuild.scratchData.deviceAddress = alignUp(tlasScratch.address, scratchAlignment);
        VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
        tlasRange.primitiveCount = instanceCount;

        immediateSubmit([&](VkCommandBuffer commandBuffer) {
            for (std::size_t i = 0; i < builds.size(); ++i) {
                VkCopyAccelerationStructureInfoKHR copyInfo{
                    VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
                copyInfo.src = builds[i].uncompacted.handle;
                copyInfo.dst = blas[i].handle;
                copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                vkCmdCopyAccelerationStructureKHR(commandBuffer, &copyInfo);
            }
            memoryBarrier(commandBuffer,
                          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_HOST_BIT,
                          VK_ACCESS_2_HOST_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            const VkAccelerationStructureBuildRangeInfoKHR* range = &tlasRange;
            vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &tlasBuild, &range);
            memoryBarrier(commandBuffer,
                          VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                          VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        });

        for (PendingBuild& build : builds) destroyAcceleration(build.uncompacted);
        destroyBuffer(scratch);
        destroyBuffer(tlasScratch);
        destroyBuffer(instanceBuffer);
        vkDestroyQueryPool(device, queryPool, nullptr);
    }

    void createDescriptorLayout() {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(31);
        for (std::uint32_t binding = 0; binding <= 27; ++binding) {
            VkDescriptorSetLayoutBinding layoutBinding{};
            layoutBinding.binding = binding;
            layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            layoutBinding.descriptorCount = 1;
            layoutBinding.stageFlags = VK_SHADER_STAGE_ALL;
            bindings.push_back(layoutBinding);
        }
        for (std::uint32_t binding : {28U, 29U}) {
            VkDescriptorSetLayoutBinding layoutBinding{};
            layoutBinding.binding = binding;
            layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            layoutBinding.descriptorCount = 1;
            layoutBinding.stageFlags = VK_SHADER_STAGE_ALL;
            bindings.push_back(layoutBinding);
        }
        VkDescriptorSetLayoutBinding accelerationBinding{};
        accelerationBinding.binding = 30;
        accelerationBinding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        accelerationBinding.descriptorCount = 1;
        accelerationBinding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                         VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                         VK_SHADER_STAGE_MISS_BIT_KHR;
        bindings.push_back(accelerationBinding);

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
                                            &descriptorSetLayout),
                "vkCreateDescriptorSetLayout");
        vkGetDescriptorSetLayoutSizeEXT(device, descriptorSetLayout, &descriptorSetSize);
        if (descriptorSetSize == 0U ||
            descriptorSetSize > descriptorProperties.maxResourceDescriptorBufferRange ||
            descriptorSetSize > descriptorProperties.maxSamplerDescriptorBufferRange) {
            fail("Descriptor Buffer layout 大小超出设备容量");
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_ALL;
        pushRange.offset = 0;
        pushRange.size = sizeof(PushConstantsCpu);
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount = 1;
        pipelineInfo.pSetLayouts = &descriptorSetLayout;
        pipelineInfo.pushConstantRangeCount = 1;
        pipelineInfo.pPushConstantRanges = &pushRange;
        checkVk(vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &pipelineLayout),
                "vkCreatePipelineLayout");
    }

    VkShaderModule createShaderModule(const std::vector<std::byte>& code) const {
        VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        checkVk(vkCreateShaderModule(device, &createInfo, nullptr, &module),
                "vkCreateShaderModule");
        return module;
    }

    VkShaderEXT createShaderObject(std::string_view fileName,
                                   VkShaderStageFlagBits stage,
                                   VkShaderStageFlags nextStage = 0,
                                   VkShaderCreateFlagsEXT flags = 0) const {
        const std::vector<std::byte> code =
            readBinaryFile(runtimeDirectory / "shaders" / (std::string(fileName) + ".spv"));
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_ALL;
        pushRange.size = sizeof(PushConstantsCpu);
        VkShaderCreateInfoEXT createInfo{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        createInfo.flags = flags;
        createInfo.stage = stage;
        createInfo.nextStage = nextStage;
        createInfo.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        createInfo.codeSize = code.size();
        createInfo.pCode = code.data();
        createInfo.pName = "main";
        createInfo.setLayoutCount = 1;
        createInfo.pSetLayouts = &descriptorSetLayout;
        createInfo.pushConstantRangeCount = 1;
        createInfo.pPushConstantRanges = &pushRange;
        VkShaderEXT shader = VK_NULL_HANDLE;
        checkVk(vkCreateShadersEXT(device, 1, &createInfo, nullptr, &shader),
                "vkCreateShadersEXT");
        return shader;
    }

    void createShaderObjects() {
        const std::array<std::string_view, ComputeShaderCount> names{
            "shade.comp",          "make_indirect.comp", "shadow_resolve.comp",
            "resolve_branches.comp", "restir_initial.comp", "restir_temporal.comp",
            "restir_spatial.comp", "restir_shadow.comp", "svgf_temporal.comp",
            "atrous.comp",         "compose.comp",       "commit_history.comp"};
        computeShaders.reserve(names.size());
        for (std::string_view name : names) {
            computeShaders.push_back(createShaderObject(name, VK_SHADER_STAGE_COMPUTE_BIT));
        }

        const std::vector<std::byte> vertexCode =
            readBinaryFile(runtimeDirectory / "shaders" / "fullscreen.vert.spv");
        const std::vector<std::byte> fragmentCode =
            readBinaryFile(runtimeDirectory / "shaders" / "display.frag.spv");
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_ALL;
        pushRange.size = sizeof(PushConstantsCpu);
        std::array<VkShaderCreateInfoEXT, 2> createInfos{};
        createInfos[0].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        createInfos[0].flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
        createInfos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        createInfos[0].nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
        createInfos[0].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        createInfos[0].codeSize = vertexCode.size();
        createInfos[0].pCode = vertexCode.data();
        createInfos[0].pName = "main";
        createInfos[0].setLayoutCount = 1;
        createInfos[0].pSetLayouts = &descriptorSetLayout;
        createInfos[0].pushConstantRangeCount = 1;
        createInfos[0].pPushConstantRanges = &pushRange;
        createInfos[1] = createInfos[0];
        createInfos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        createInfos[1].nextStage = 0;
        createInfos[1].codeSize = fragmentCode.size();
        createInfos[1].pCode = fragmentCode.data();
        std::array<VkShaderEXT, 2> shaders{};
        checkVk(vkCreateShadersEXT(device, static_cast<std::uint32_t>(createInfos.size()),
                                   createInfos.data(), nullptr, shaders.data()),
                "vkCreateShadersEXT(graphics)");
        vertexShader = shaders[0];
        fragmentShader = shaders[1];
    }

    void createRayTracingPipeline() {
        const std::array<std::string_view, 6> names{
            "primary.rgen", "continuation.rgen", "shadow.rgen",
            "radiance.rmiss", "shadow.rmiss", "radiance.rchit"};
        const std::array<VkShaderStageFlagBits, 6> stages{
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
            VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
        std::array<VkShaderModule, 6> modules{};
        std::array<VkPipelineShaderStageCreateInfo, 6> stageInfos{};
        for (std::size_t i = 0; i < modules.size(); ++i) {
            modules[i] = createShaderModule(
                readBinaryFile(runtimeDirectory / "shaders" /
                               (std::string(names[i]) + ".spv")));
            stageInfos[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stageInfos[i].stage = stages[i];
            stageInfos[i].module = modules[i];
            stageInfos[i].pName = "main";
        }
        std::array<VkRayTracingShaderGroupCreateInfoKHR, 7> groups{};
        for (auto& group : groups) {
            group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            group.generalShader = VK_SHADER_UNUSED_KHR;
            group.closestHitShader = VK_SHADER_UNUSED_KHR;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        for (std::uint32_t i = 0; i < 5U; ++i) {
            groups[i].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            groups[i].generalShader = i;
        }
        groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[5].closestHitShader = 5;
        // The shadow path skips closest-hit execution, but uses a distinct SBT
        // record so its traceRayEXT record offset is explicit and reviewable.
        groups[6].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[6].closestHitShader = 5;

        VkRayTracingPipelineCreateInfoKHR pipelineInfo{
            VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pipelineInfo.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stageInfos.size());
        pipelineInfo.pStages = stageInfos.data();
        pipelineInfo.groupCount = static_cast<std::uint32_t>(groups.size());
        pipelineInfo.pGroups = groups.data();
        pipelineInfo.maxPipelineRayRecursionDepth = 1;
        pipelineInfo.layout = pipelineLayout;
        checkVk(vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
                                               &pipelineInfo, nullptr,
                                               &rayTracingPipeline),
                "vkCreateRayTracingPipelinesKHR");
        for (VkShaderModule module : modules) vkDestroyShaderModule(device, module, nullptr);

        const VkDeviceSize handleSize = rtProperties.shaderGroupHandleSize;
        const VkDeviceSize stride = alignUp(handleSize, rtProperties.shaderGroupHandleAlignment);
        const VkDeviceSize raygenStride =
            alignUp(handleSize, rtProperties.shaderGroupBaseAlignment);
        if (stride > rtProperties.maxShaderGroupStride ||
            raygenStride > rtProperties.maxShaderGroupStride)
            fail("SBT stride 超出设备限制");
        const VkDeviceSize raygenOffset = 0;
        const VkDeviceSize raygenSize = raygenStride * 3U;
        const VkDeviceSize missOffset = alignUp(raygenSize, rtProperties.shaderGroupBaseAlignment);
        const VkDeviceSize missSize = stride * 2U;
        const VkDeviceSize hitOffset =
            alignUp(missOffset + missSize, rtProperties.shaderGroupBaseAlignment);
        const VkDeviceSize hitSize = stride * 2U;
        const VkDeviceSize totalSize = hitOffset + hitSize;
        sbtBuffer = createBuffer(totalSize + rtProperties.shaderGroupBaseAlignment - 1U,
                                 VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 true);
        const VkDeviceAddress sbtBaseAddress =
            alignUp(sbtBuffer.address, rtProperties.shaderGroupBaseAlignment);
        const VkDeviceSize sbtBaseOffset = sbtBaseAddress - sbtBuffer.address;
        std::vector<std::byte> handles(static_cast<std::size_t>(handleSize * groups.size()));
        checkVk(vkGetRayTracingShaderGroupHandlesKHR(device, rayTracingPipeline, 0,
                                                     static_cast<std::uint32_t>(groups.size()),
                                                     handles.size(), handles.data()),
                "vkGetRayTracingShaderGroupHandlesKHR");
        auto copyHandle = [&](std::uint32_t group, VkDeviceSize offset) {
            std::memcpy(static_cast<std::byte*>(sbtBuffer.mapped) + sbtBaseOffset + offset,
                        handles.data() + group * handleSize,
                        static_cast<std::size_t>(handleSize));
        };
        copyHandle(0, raygenOffset);
        copyHandle(1, raygenOffset + raygenStride);
        copyHandle(2, raygenOffset + raygenStride * 2U);
        copyHandle(3, missOffset);
        copyHandle(4, missOffset + stride);
        copyHandle(5, hitOffset);
        copyHandle(6, hitOffset + stride);
        checkVk(vmaFlushAllocation(allocator, sbtBuffer.allocation, 0, sbtBuffer.size),
                "vmaFlushAllocation(SBT)");
        for (std::uint32_t i = 0; i < 3U; ++i) {
            raygenRegions[i] = {sbtBaseAddress + raygenOffset + raygenStride * i,
                                raygenStride, raygenStride};
        }
        missRegion = {sbtBaseAddress + missOffset, stride, missSize};
        hitRegion = {sbtBaseAddress + hitOffset, stride, hitSize};
        callableRegion = {};
    }

    void createFrameInfrastructure() {
        for (FrameSlot& slot : frameSlots) {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamily;
            checkVk(vkCreateCommandPool(device, &poolInfo, nullptr, &slot.pool),
                    "vkCreateCommandPool(frame)");
            VkCommandBufferAllocateInfo allocationInfo{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocationInfo.commandPool = slot.pool;
            allocationInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocationInfo.commandBufferCount = 1;
            checkVk(vkAllocateCommandBuffers(device, &allocationInfo, &slot.commandBuffer),
                    "vkAllocateCommandBuffers(frame)");
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            checkVk(vkCreateFence(device, &fenceInfo, nullptr, &slot.fence),
                    "vkCreateFence(frame)");
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &slot.acquired),
                    "vkCreateSemaphore(acquire)");
        }
        VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        typeInfo.initialValue = 0;
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        semaphoreInfo.pNext = &typeInfo;
        checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &timeline),
                "vkCreateSemaphore(timeline)");
    }

    void destroySwapchainViews() {
        for (VkSemaphore semaphore : renderFinishedSemaphores)
            vkDestroySemaphore(device, semaphore, nullptr);
        renderFinishedSemaphores.clear();
        for (VkImageView view : swapchainViews) vkDestroyImageView(device, view, nullptr);
        swapchainViews.clear();
        swapchainImages.clear();
        if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    bool createSwapchain() {
        int pixelWidth = 0;
        int pixelHeight = 0;
        if (!SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight)) {
            fail(std::string("SDL_GetWindowSizeInPixels 失败: ") + SDL_GetError());
        }
        if (pixelWidth <= 0 || pixelHeight <= 0) {
            minimized = true;
            return false;
        }
        minimized = false;

        VkSurfaceCapabilitiesKHR capabilities{};
        checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities),
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        std::uint32_t formatCount = 0;
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                                     nullptr),
                "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount,
                                                     formats.data()),
                "vkGetPhysicalDeviceSurfaceFormatsKHR");
        const auto formatIt = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& format) {
            const bool srgbFormat = format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                                    format.format == VK_FORMAT_R8G8B8A8_SRGB;
            return srgbFormat && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (formatIt == formats.end()) fail("交换链没有可用的 sRGB 格式");
        swapchainFormat = formatIt->format;

        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = std::clamp(static_cast<std::uint32_t>(pixelWidth),
                                      capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<std::uint32_t>(pixelHeight),
                                       capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        if (extent.width == 0U || extent.height == 0U) {
            minimized = true;
            return false;
        }

        std::uint32_t presentModeCount = 0;
        checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface,
                                                          &presentModeCount, nullptr),
                "vkGetPhysicalDeviceSurfacePresentModesKHR(count)");
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(
                    physicalDevice, surface, &presentModeCount, presentModes.data()),
                "vkGetPhysicalDeviceSurfacePresentModesKHR");
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        if (std::ranges::find(presentModes, VK_PRESENT_MODE_MAILBOX_KHR) !=
            presentModes.end())
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;

        std::uint32_t imageCount = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount > 0U)
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = formatIt->format;
        createInfo.imageColorSpace = formatIt->colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        checkVk(vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain),
                "vkCreateSwapchainKHR");

        checkVk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr),
                "vkGetSwapchainImagesKHR(count)");
        swapchainImages.resize(imageCount);
        checkVk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount,
                                        swapchainImages.data()),
                "vkGetSwapchainImagesKHR");
        swapchainViews.resize(imageCount);
        renderFinishedSemaphores.resize(imageCount);
        for (std::uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = swapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            checkVk(vkCreateImageView(device, &viewInfo, nullptr, &swapchainViews[i]),
                    "vkCreateImageView(swapchain)");
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                                      &renderFinishedSemaphores[i]),
                    "vkCreateSemaphore(present)");
        }
        return true;
    }

    void destroyPerPixelResources() {
        std::array<BufferResource*, 23> resources{
            &pixels.pathsA,            &pixels.pathsB,           &pixels.hits,
            &pixels.branchOutputs,     &pixels.counters,         &pixels.shadowTasks,
            &pixels.shadowResults,     &pixels.shadingPoints,    &pixels.reservoirInitial,
            &pixels.reservoirTemporal, &pixels.reservoirSpatial, &pixels.reservoirHistory0,
            &pixels.reservoirHistory1, &pixels.guidesCurrent,    &pixels.guidesHistory0,
            &pixels.guidesHistory1,    &pixels.signalsCurrent,   &pixels.temporal0,
            &pixels.temporal1,         &pixels.filterPing,       &pixels.filterPong,
            &pixels.finalPixels,       &pixels.indirect};
        for (BufferResource* resource : resources) destroyBuffer(*resource);
    }

    void createPerPixelResources() {
        const VkDeviceSize count = static_cast<VkDeviceSize>(extent.width) * extent.height;
        const VkDeviceSize branchCount = count * 2U;
        constexpr VkBufferUsageFlags commonUsage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        auto storage = [&](VkDeviceSize size, VkBufferUsageFlags extra = 0U) {
            return createBuffer(size, commonUsage | extra);
        };
        pixels.pathsA = storage(branchCount * 144U);
        pixels.pathsB = storage(branchCount * 144U);
        pixels.hits = storage(branchCount * 64U);
        pixels.branchOutputs = storage(branchCount * 144U);
        pixels.counters = storage(16U);
        pixels.shadowTasks = storage(branchCount * 64U);
        pixels.shadowResults = storage(branchCount * sizeof(std::uint32_t));
        pixels.shadingPoints = storage(count * 64U);
        pixels.reservoirInitial = storage(count * 32U);
        pixels.reservoirTemporal = storage(count * 32U);
        pixels.reservoirSpatial = storage(count * 32U);
        pixels.reservoirHistory0 = storage(count * 32U);
        pixels.reservoirHistory1 = storage(count * 32U);
        pixels.guidesCurrent = storage(count * 192U);
        pixels.guidesHistory0 = storage(count * 192U);
        pixels.guidesHistory1 = storage(count * 192U);
        pixels.signalsCurrent = storage(count * 112U);
        pixels.temporal0 = storage(count * 192U);
        pixels.temporal1 = storage(count * 192U);
        pixels.filterPing = storage(count * 96U);
        pixels.filterPong = storage(count * 96U);
        pixels.finalPixels = storage(count * 16U);
        pixels.indirect = storage(64U, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    }

    void writeDescriptors() {
        destroyBuffer(descriptorBuffer);
        const VkDeviceSize descriptorSize =
            alignUp(descriptorSetSize, descriptorProperties.descriptorBufferOffsetAlignment);
        descriptorBuffer = createBuffer(
            descriptorSize,
            VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            true);
        std::memset(descriptorBuffer.mapped, 0, static_cast<std::size_t>(descriptorBuffer.size));

        const std::array<BufferResource*, 28> resources{
            &globalsBuffer,            &vertexBuffer,             &triangleBuffer,
            &materialBuffer,           &lightBuffer,              &pixels.pathsA,
            &pixels.pathsB,            &pixels.hits,              &pixels.branchOutputs,
            &pixels.counters,          &pixels.shadowTasks,       &pixels.shadowResults,
            &pixels.shadingPoints,     &pixels.reservoirInitial,  &pixels.reservoirTemporal,
            &pixels.reservoirSpatial,  &pixels.reservoirHistory0, &pixels.reservoirHistory1,
            &pixels.guidesCurrent,     &pixels.guidesHistory0,    &pixels.guidesHistory1,
            &pixels.signalsCurrent,    &pixels.temporal0,         &pixels.temporal1,
            &pixels.filterPing,        &pixels.filterPong,        &pixels.finalPixels,
            &pixels.indirect};
        for (std::uint32_t binding = 0; binding < resources.size(); ++binding) {
            VkDeviceSize offset = 0;
            vkGetDescriptorSetLayoutBindingOffsetEXT(device, descriptorSetLayout, binding,
                                                      &offset);
            VkDescriptorAddressInfoEXT addressInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
            addressInfo.address = resources[binding]->address;
            addressInfo.range = resources[binding]->size;
            addressInfo.format = VK_FORMAT_UNDEFINED;
            VkDescriptorGetInfoEXT descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            descriptorInfo.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            descriptorInfo.data.pStorageBuffer = &addressInfo;
            vkGetDescriptorEXT(
                device, &descriptorInfo, descriptorProperties.storageBufferDescriptorSize,
                static_cast<std::byte*>(descriptorBuffer.mapped) + offset);
        }
        const std::array<ImageResource*, 2> images{&baseColorImage, &roughnessImage};
        for (std::uint32_t i = 0; i < images.size(); ++i) {
            const std::uint32_t binding = 28U + i;
            VkDeviceSize offset = 0;
            vkGetDescriptorSetLayoutBindingOffsetEXT(device, descriptorSetLayout, binding,
                                                      &offset);
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = textureSampler;
            imageInfo.imageView = images[i]->view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorGetInfoEXT descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            descriptorInfo.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorInfo.data.pCombinedImageSampler = &imageInfo;
            vkGetDescriptorEXT(
                device, &descriptorInfo,
                descriptorProperties.combinedImageSamplerDescriptorSize,
                static_cast<std::byte*>(descriptorBuffer.mapped) + offset);
        }
        VkDeviceSize accelerationOffset = 0;
        vkGetDescriptorSetLayoutBindingOffsetEXT(device, descriptorSetLayout, 30,
                                                  &accelerationOffset);
        VkDescriptorGetInfoEXT accelerationInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        accelerationInfo.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        accelerationInfo.data.accelerationStructure = tlas.address;
        vkGetDescriptorEXT(
            device, &accelerationInfo, descriptorProperties.accelerationStructureDescriptorSize,
            static_cast<std::byte*>(descriptorBuffer.mapped) + accelerationOffset);
        checkVk(vmaFlushAllocation(allocator, descriptorBuffer.allocation, 0,
                                   descriptorBuffer.size),
                "vmaFlushAllocation(descriptor buffer)");
    }

    void resetFrameStatistics() {
        statisticsStart = std::chrono::steady_clock::now();
        statisticsFrames = 0;
        SDL_SetWindowTitle(window, "VoxelPathTracer — -- FPS, -- ms/frame");
    }

    bool recreateSwapchain() {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height)) {
            fail(std::string("SDL_GetWindowSizeInPixels 失败: ") + SDL_GetError());
        }
        if (width <= 0 || height <= 0) {
            minimized = true;
            resetFrameStatistics();
            return false;
        }
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(swapchain recreate)");
        destroyBuffer(descriptorBuffer);
        destroyPerPixelResources();
        destroySwapchainViews();
        if (!createSwapchain()) return false;
        createPerPixelResources();
        writeDescriptors();
        historyReset = true;
        cameraCut = true;
        framebufferResized = false;
        previousJitter = {};
        resetFrameStatistics();
        return true;
    }

    void bindDescriptorBuffer(VkCommandBuffer commandBuffer) const {
        VkDescriptorBufferBindingInfoEXT bindingInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
        bindingInfo.address = descriptorBuffer.address;
        bindingInfo.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                            VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
        vkCmdBindDescriptorBuffersEXT(commandBuffer, 1, &bindingInfo);
        const std::uint32_t bufferIndex = 0;
        const VkDeviceSize offset = 0;
        for (VkPipelineBindPoint bindPoint : {VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                              VK_PIPELINE_BIND_POINT_COMPUTE,
                                              VK_PIPELINE_BIND_POINT_GRAPHICS}) {
            vkCmdSetDescriptorBufferOffsetsEXT(commandBuffer, bindPoint, pipelineLayout, 0, 1,
                                               &bufferIndex, &offset);
        }
    }

    void bindCompute(VkCommandBuffer commandBuffer, ComputeShaderIndex index,
                     const PushConstantsCpu& push = {}) const {
        const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkShaderEXT shader = computeShaders[index];
        vkCmdBindShadersEXT(commandBuffer, 1, &stage, &shader);
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_ALL, 0,
                           sizeof(push), &push);
    }

    void shaderBarrier(VkCommandBuffer commandBuffer) const {
        memoryBarrier(commandBuffer,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                          VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                          VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }

    void clearCounter(VkCommandBuffer commandBuffer, std::uint32_t queueIndex,
                      bool clearShadow) const {
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(commandBuffer, pixels.counters.buffer,
                        queueIndex * sizeof(std::uint32_t), sizeof(std::uint32_t), 0);
        if (clearShadow) {
            vkCmdFillBuffer(commandBuffer, pixels.counters.buffer, 2U * sizeof(std::uint32_t),
                            sizeof(std::uint32_t), 0);
        }
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    void recordShadowPass(VkCommandBuffer commandBuffer) const {
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          rayTracingPipeline);
        vkCmdTraceRaysIndirectKHR(commandBuffer, &raygenRegions[2], &missRegion, &hitRegion,
                                 &callableRegion, pixels.indirect.address + 32U);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        bindCompute(commandBuffer, ShadowResolve);
        vkCmdDispatchIndirect(commandBuffer, pixels.indirect.buffer, 48U);
        shaderBarrier(commandBuffer);
    }

    void recordImagingPasses(VkCommandBuffer commandBuffer) const {
        const VkDeviceSize count = static_cast<VkDeviceSize>(extent.width) * extent.height;
        const std::array<const BufferResource*, 11> clearBuffers{
            &pixels.branchOutputs,    &pixels.shadingPoints,   &pixels.reservoirInitial,
            &pixels.reservoirTemporal, &pixels.reservoirSpatial, &pixels.guidesCurrent,
            &pixels.signalsCurrent,   &pixels.filterPing,      &pixels.filterPong,
            &pixels.finalPixels,      &pixels.counters};
        for (const BufferResource* resource : clearBuffers) {
            vkCmdFillBuffer(commandBuffer, resource->buffer, 0, resource->size, 0);
        }
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const std::uint32_t primaryCount = static_cast<std::uint32_t>(count);
        vkCmdUpdateBuffer(commandBuffer, pixels.counters.buffer, 0,
                          sizeof(primaryCount), &primaryCount);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_CLEAR_BIT |
                                         VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          rayTracingPipeline);
        const PushConstantsCpu initialPush{};
        vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_ALL, 0,
                           sizeof(initialPush), &initialPush);
        vkCmdTraceRaysKHR(commandBuffer, &raygenRegions[0], &missRegion, &hitRegion,
                          &callableRegion, extent.width, extent.height, 1);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

        PushConstantsCpu shadePush{};
        shadePush.u0 = {0U, 1U, 1U, 0U};
        bindCompute(commandBuffer, Shade, shadePush);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        PushConstantsCpu indirectPush{};
        indirectPush.u0.x = 1U;
        bindCompute(commandBuffer, MakeIndirect, indirectPush);
        vkCmdDispatch(commandBuffer, 1, 1, 1);
        shaderBarrier(commandBuffer);
        recordShadowPass(commandBuffer);

        std::uint32_t currentQueue = 1U;
        for (std::uint32_t segment = 1U; segment <= 8U; ++segment) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                              rayTracingPipeline);
            PushConstantsCpu tracePush{};
            tracePush.u0.x = currentQueue;
            vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_ALL, 0,
                               sizeof(tracePush), &tracePush);
            vkCmdTraceRaysIndirectKHR(commandBuffer, &raygenRegions[1], &missRegion,
                                     &hitRegion, &callableRegion, pixels.indirect.address);
            const std::uint32_t outputQueue = 1U - currentQueue;
            clearCounter(commandBuffer, outputQueue, true);
            memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            shadePush.u0 = {currentQueue, outputQueue, segment < 8U ? 1U : 0U, segment};
            bindCompute(commandBuffer, Shade, shadePush);
            vkCmdDispatchIndirect(commandBuffer, pixels.indirect.buffer, 16U);
            memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            indirectPush.u0.x = outputQueue;
            bindCompute(commandBuffer, MakeIndirect, indirectPush);
            vkCmdDispatch(commandBuffer, 1, 1, 1);
            shaderBarrier(commandBuffer);
            recordShadowPass(commandBuffer);
            currentQueue = outputQueue;
        }

        bindCompute(commandBuffer, ResolveBranches);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        shaderBarrier(commandBuffer);

        bindCompute(commandBuffer, RestirInitial);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        shaderBarrier(commandBuffer);
        bindCompute(commandBuffer, RestirTemporal);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        shaderBarrier(commandBuffer);
        bindCompute(commandBuffer, RestirSpatial);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        shaderBarrier(commandBuffer);

        clearCounter(commandBuffer, currentQueue, true);
        bindCompute(commandBuffer, RestirShadow);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        indirectPush.u0.x = currentQueue;
        bindCompute(commandBuffer, MakeIndirect, indirectPush);
        vkCmdDispatch(commandBuffer, 1, 1, 1);
        shaderBarrier(commandBuffer);
        recordShadowPass(commandBuffer);

        bindCompute(commandBuffer, SvgfTemporal);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        shaderBarrier(commandBuffer);
        for (std::uint32_t iteration = 0; iteration < 5U; ++iteration) {
            PushConstantsCpu filterPush{};
            filterPush.u0.x = iteration % 2U;
            filterPush.u0.y = 1U << iteration;
            bindCompute(commandBuffer, Atrous, filterPush);
            vkCmdDispatch(commandBuffer, (extent.width + 7U) / 8U,
                          (extent.height + 7U) / 8U, 1);
            shaderBarrier(commandBuffer);
        }
        bindCompute(commandBuffer, Compose);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        shaderBarrier(commandBuffer);
        bindCompute(commandBuffer, CommitHistory);
        vkCmdDispatch(commandBuffer, (primaryCount + 63U) / 64U, 1, 1);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    void recordDisplay(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) const {
        VkImageMemoryBarrier2 toAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment.image = swapchainImages[imageIndex];
        toAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toAttachment.subresourceRange.levelCount = 1;
        toAttachment.subresourceRange.layerCount = 1;
        VkDependencyInfo toAttachmentDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        toAttachmentDependency.imageMemoryBarrierCount = 1;
        toAttachmentDependency.pImageMemoryBarriers = &toAttachment;
        vkCmdPipelineBarrier2(commandBuffer, &toAttachmentDependency);

        VkClearValue clear{};
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = swapchainViews[imageIndex];
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue = clear;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(commandBuffer, &rendering);

        const std::array<VkShaderStageFlagBits, 2> stages{VK_SHADER_STAGE_VERTEX_BIT,
                                                         VK_SHADER_STAGE_FRAGMENT_BIT};
        const std::array<VkShaderEXT, 2> shaders{vertexShader, fragmentShader};
        vkCmdBindShadersEXT(commandBuffer, static_cast<std::uint32_t>(stages.size()),
                            stages.data(), shaders.data());
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width),
                            static_cast<float>(extent.height), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewportWithCount(commandBuffer, 1, &viewport);
        vkCmdSetScissorWithCount(commandBuffer, 1, &scissor);
        vkCmdSetVertexInputEXT(commandBuffer, 0, nullptr, 0, nullptr);
        vkCmdSetPrimitiveTopology(commandBuffer, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkCmdSetPrimitiveRestartEnable(commandBuffer, VK_FALSE);
        vkCmdSetRasterizerDiscardEnable(commandBuffer, VK_FALSE);
        vkCmdSetPolygonModeEXT(commandBuffer, VK_POLYGON_MODE_FILL);
        vkCmdSetCullMode(commandBuffer, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(commandBuffer, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetLineWidth(commandBuffer, 1.0F);
        vkCmdSetDepthBiasEnable(commandBuffer, VK_FALSE);
        vkCmdSetDepthTestEnable(commandBuffer, VK_FALSE);
        vkCmdSetDepthWriteEnable(commandBuffer, VK_FALSE);
        vkCmdSetDepthCompareOp(commandBuffer, VK_COMPARE_OP_ALWAYS);
        vkCmdSetDepthBoundsTestEnable(commandBuffer, VK_FALSE);
        vkCmdSetStencilTestEnable(commandBuffer, VK_FALSE);
        vkCmdSetRasterizationSamplesEXT(commandBuffer, VK_SAMPLE_COUNT_1_BIT);
        const VkSampleMask sampleMask = 0xffffffffU;
        vkCmdSetSampleMaskEXT(commandBuffer, VK_SAMPLE_COUNT_1_BIT, &sampleMask);
        vkCmdSetAlphaToCoverageEnableEXT(commandBuffer, VK_FALSE);
        const VkBool32 blendEnable = VK_FALSE;
        vkCmdSetColorBlendEnableEXT(commandBuffer, 0, 1, &blendEnable);
        VkColorBlendEquationEXT blendEquation{};
        blendEquation.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendEquation.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendEquation.colorBlendOp = VK_BLEND_OP_ADD;
        blendEquation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendEquation.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendEquation.alphaBlendOp = VK_BLEND_OP_ADD;
        vkCmdSetColorBlendEquationEXT(commandBuffer, 0, 1, &blendEquation);
        const VkColorComponentFlags colorMask = VK_COLOR_COMPONENT_R_BIT |
                                                VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT |
                                                VK_COLOR_COMPONENT_A_BIT;
        vkCmdSetColorWriteMaskEXT(commandBuffer, 0, 1, &colorMask);
        vkCmdDraw(commandBuffer, 3, 1, 0, 0);
        vkCmdEndRendering(commandBuffer);

        VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.image = swapchainImages[imageIndex];
        toPresent.subresourceRange = toAttachment.subresourceRange;
        VkDependencyInfo presentDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        presentDependency.imageMemoryBarrierCount = 1;
        presentDependency.pImageMemoryBarriers = &toPresent;
        vkCmdPipelineBarrier2(commandBuffer, &presentDependency);
    }

    void releaseMouse() {
        if (mouseCaptured && window != nullptr) SDL_SetWindowRelativeMouseMode(window, false);
        mouseCaptured = false;
    }

    void processEvents() {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_RIGHT && !mouseCaptured) {
                    if (!SDL_SetWindowRelativeMouseMode(window, true)) {
                        fail(std::string("SDL_SetWindowRelativeMouseMode 失败: ") +
                             SDL_GetError());
                    }
                    mouseCaptured = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_RIGHT) releaseMouse();
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (mouseCaptured) {
                    camera.yaw += event.motion.xrel * 0.0025F;
                    camera.pitch = std::clamp(camera.pitch - event.motion.yrel * 0.0025F,
                                              -89.0F * std::numbers::pi_v<float> / 180.0F,
                                              89.0F * std::numbers::pi_v<float> / 180.0F);
                    cameraMovedThisFrame = true;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (mouseCaptured)
                        releaseMouse();
                    else
                        running = false;
                } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_R) {
                    camera.reset();
                    cameraCut = true;
                    historyReset = true;
                    cameraMovedThisFrame = true;
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_RESIZED:
                framebufferResized = true;
                break;
            case SDL_EVENT_WINDOW_MINIMIZED:
                minimized = true;
                releaseMouse();
                resetFrameStatistics();
                lastLoopTime = std::chrono::steady_clock::now();
                break;
            case SDL_EVENT_WINDOW_RESTORED:
                minimized = false;
                framebufferResized = true;
                lastLoopTime = std::chrono::steady_clock::now();
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                focused = false;
                releaseMouse();
                lastLoopTime = std::chrono::steady_clock::now();
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                focused = true;
                lastLoopTime = std::chrono::steady_clock::now();
                break;
            default:
                break;
            }
        }
    }

    void updateCamera(float deltaSeconds) {
        if (!focused || minimized || deltaSeconds <= 0.0F) return;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        Vec3 horizontalForward = camera.forward();
        horizontalForward.y = 0.0F;
        if (length(horizontalForward) < 1.0e-5F) {
            horizontalForward = {std::sin(camera.yaw), 0.0F, -std::cos(camera.yaw)};
        } else {
            horizontalForward = normalize(horizontalForward);
        }
        const Vec3 right = normalize(cross(horizontalForward, {0.0F, 1.0F, 0.0F}));
        Vec3 movement{};
        if (keys[SDL_SCANCODE_W]) movement += horizontalForward;
        if (keys[SDL_SCANCODE_S]) movement += -horizontalForward;
        if (keys[SDL_SCANCODE_D]) movement += right;
        if (keys[SDL_SCANCODE_A]) movement += -right;
        if (keys[SDL_SCANCODE_E]) movement.y += 1.0F;
        if (keys[SDL_SCANCODE_Q]) movement.y -= 1.0F;
        if (length(movement) > 0.0F) {
            const bool fast = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
            const float speed = 3.0F * (fast ? 3.0F : 1.0F);
            camera.position += normalize(movement) * (speed * deltaSeconds);
            cameraMovedThisFrame = true;
        }
    }

    Mat4 cameraViewProjection(const Camera& value) const {
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        const Mat4 projection = perspective(60.0F * std::numbers::pi_v<float> / 180.0F,
                                            aspect, 0.05F, 1000.0F);
        return projection * viewMatrix(value.position, value.forward(), {0.0F, 1.0F, 0.0F});
    }

    GlobalDataCpu updateGlobalData() {
        const Vec3 forward = camera.forward();
        const Vec3 right = camera.right();
        const Vec3 up = normalize(cross(right, forward));
        const Mat4 currentViewProjection = cameraViewProjection(camera);
        const std::uint32_t frameBits = static_cast<std::uint32_t>(renderedFrame);
        const Vec2 jitter{halton(renderedFrame + 1U, 2U) - 0.5F,
                          halton(renderedFrame + 1U, 3U) - 0.5F};
        historyWriteIndex = renderedFrame == 0U ? 0U : 1U - historyReadIndex;

        GlobalDataCpu data{};
        data.currentViewProjection = currentViewProjection;
        data.previousViewProjection = renderedFrame == 0U ? currentViewProjection
                                                           : previousViewProjection;
        data.cameraPositionFrame = {camera.position.x, camera.position.y, camera.position.z,
                                    std::bit_cast<float>(frameBits)};
        data.cameraForwardTanHalfFov = {forward.x, forward.y, forward.z,
                                        std::tan(30.0F * std::numbers::pi_v<float> / 180.0F)};
        data.cameraRightAspect = {right.x, right.y, right.z,
                                  static_cast<float>(extent.width) /
                                      static_cast<float>(extent.height)};
        data.cameraUpNear = {up.x, up.y, up.z, 0.05F};
        data.jitterCurrentPrevious = {jitter.x, jitter.y, previousJitter.x,
                                      previousJitter.y};
        data.extentCounts = {extent.width, extent.height,
                             static_cast<std::uint32_t>(materialSet.materials.size()),
                             static_cast<std::uint32_t>(scene.lights.size())};
        data.flags = {(historyReset || cameraCut) ? 1U : 0U,
                      cameraMovedThisFrame ? 1U : 0U, historyReadIndex,
                      historyWriteIndex};
        data.sceneIds = {materialSet.require("glass"), baseColorImage.mipLevels,
                         materialSet.textureSize, 0U};
        std::memcpy(globalsBuffer.mapped, &data, sizeof(data));
        checkVk(vmaFlushAllocation(allocator, globalsBuffer.allocation, 0, sizeof(data)),
                "vmaFlushAllocation(globals)");
        return data;
    }

    void recordFrame(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) const {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(frame)");
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_HOST_BIT,
                      VK_ACCESS_2_HOST_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_READ_BIT |
                          VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT);
        bindDescriptorBuffer(commandBuffer);
        recordImagingPasses(commandBuffer);
        recordDisplay(commandBuffer, imageIndex);
        checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(frame)");
    }

    bool renderFrame() {
        if (validationFailure.load()) fail("Vulkan Validation 报告了 warning/error");
        FrameSlot& slot = frameSlots[renderedFrame % kFrameSlotCount];
        checkVk(vkWaitForFences(device, 1, &slot.fence, VK_TRUE,
                                std::numeric_limits<std::uint64_t>::max()),
                "vkWaitForFences(frame)");
        std::uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(
            device, swapchain, std::numeric_limits<std::uint64_t>::max(), slot.acquired,
            VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return false;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            checkVk(acquire, "vkAcquireNextImageKHR");
        }

        const GlobalDataCpu globalData = updateGlobalData();
        checkVk(vkResetCommandPool(device, slot.pool, 0), "vkResetCommandPool(frame)");
        recordFrame(slot.commandBuffer, imageIndex);
        checkVk(vkResetFences(device, 1, &slot.fence), "vkResetFences(frame)");

        std::array<VkSemaphoreSubmitInfo, 2> waits{};
        std::uint32_t waitCount = 0;
        waits[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[waitCount].semaphore = slot.acquired;
        waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        ++waitCount;
        if (timelineValue > 0U) {
            waits[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[waitCount].semaphore = timeline;
            waits[waitCount].value = timelineValue;
            waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ++waitCount;
        }
        const std::uint64_t nextTimelineValue = timelineValue + 1U;
        std::array<VkSemaphoreSubmitInfo, 2> signals{};
        signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[0].semaphore = renderFinishedSemaphores[imageIndex];
        signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[1].semaphore = timeline;
        signals[1].value = nextTimelineValue;
        signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = slot.commandBuffer;
        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.waitSemaphoreInfoCount = waitCount;
        submitInfo.pWaitSemaphoreInfos = waits.data();
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandInfo;
        submitInfo.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size());
        submitInfo.pSignalSemaphoreInfos = signals.data();
        checkVk(vkQueueSubmit2(queue, 1, &submitInfo, slot.fence), "vkQueueSubmit2(frame)");
        timelineValue = nextTimelineValue;

        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinishedSemaphores[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;
        const VkResult present = vkQueuePresentKHR(queue, &presentInfo);

        previousCamera = camera;
        previousViewProjection = globalData.currentViewProjection;
        previousJitter = {globalData.jitterCurrentPrevious.x,
                          globalData.jitterCurrentPrevious.y};
        historyReadIndex = historyWriteIndex;
        historyReset = false;
        cameraCut = false;
        ++renderedFrame;

        const bool presentSucceeded = present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR;
        if (presentSucceeded) ++statisticsFrames;
        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR ||
            framebufferResized || acquire == VK_SUBOPTIMAL_KHR) {
            recreateSwapchain();
        } else if (present != VK_SUCCESS) {
            checkVk(present, "vkQueuePresentKHR");
        }
        return presentSucceeded;
    }

    void updateFrameStatistics() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - statisticsStart).count();
        if (elapsed < 0.5 || statisticsFrames == 0U) return;
        const double fps = static_cast<double>(statisticsFrames) / elapsed;
        const double milliseconds = elapsed * 1000.0 / statisticsFrames;
        std::ostringstream title;
        title << std::fixed << std::setprecision(1) << "VoxelPathTracer — " << fps
              << " FPS, " << milliseconds << " ms/frame";
        SDL_SetWindowTitle(window, title.str().c_str());
        statisticsStart = now;
        statisticsFrames = 0;
    }

    int run() {
        initializeWindow();
        createInstance();
        selectPhysicalDevice();
        createDevice();
        createStaticSceneResources();
        buildAccelerationStructures();
        createDescriptorLayout();
        createShaderObjects();
        createRayTracingPipeline();
        createFrameInfrastructure();
        if (!createSwapchain()) fail("初始 framebuffer 尺寸为零");
        createPerPixelResources();
        writeDescriptors();
        lastLoopTime = std::chrono::steady_clock::now();
        resetFrameStatistics();

        while (running) {
            cameraMovedThisFrame = false;
            processEvents();
            if (!running) break;
            const auto now = std::chrono::steady_clock::now();
            float deltaSeconds =
                static_cast<float>(std::chrono::duration<double>(now - lastLoopTime).count());
            lastLoopTime = now;
            if (deltaSeconds > 0.25F) deltaSeconds = 0.0F;
            updateCamera(deltaSeconds);

            int width = 0;
            int height = 0;
            if (!SDL_GetWindowSizeInPixels(window, &width, &height)) {
                fail(std::string("SDL_GetWindowSizeInPixels 失败: ") + SDL_GetError());
            }
            if (width <= 0 || height <= 0 || minimized) {
                minimized = true;
                resetFrameStatistics();
                SDL_Delay(16);
                lastLoopTime = std::chrono::steady_clock::now();
                continue;
            }
            if (framebufferResized || static_cast<std::uint32_t>(width) != extent.width ||
                static_cast<std::uint32_t>(height) != extent.height) {
                if (!recreateSwapchain()) continue;
            }
            renderFrame();
            updateFrameStatistics();
        }
        checkVk(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(exit)");
        if (validationFailure.load()) fail("Vulkan Validation 报告了 warning/error");
        return 0;
    }

    void shutdown() noexcept {
        releaseMouse();
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        if (device != VK_NULL_HANDLE && allocator != VK_NULL_HANDLE) {
            destroyBuffer(descriptorBuffer);
            destroyPerPixelResources();
            destroySwapchainViews();
            for (VkShaderEXT shader : computeShaders) {
                if (shader != VK_NULL_HANDLE) vkDestroyShaderEXT(device, shader, nullptr);
            }
            computeShaders.clear();
            if (vertexShader != VK_NULL_HANDLE) vkDestroyShaderEXT(device, vertexShader, nullptr);
            if (fragmentShader != VK_NULL_HANDLE)
                vkDestroyShaderEXT(device, fragmentShader, nullptr);
            vertexShader = fragmentShader = VK_NULL_HANDLE;
            if (rayTracingPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, rayTracingPipeline, nullptr);
            if (pipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorSetLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
            destroyBuffer(sbtBuffer);
            destroyAcceleration(tlas);
            for (AccelerationResource& acceleration : blas) destroyAcceleration(acceleration);
            blas.clear();
            if (textureSampler != VK_NULL_HANDLE)
                vkDestroySampler(device, textureSampler, nullptr);
            destroyImage(baseColorImage);
            destroyImage(roughnessImage);
            destroyBuffer(globalsBuffer);
            destroyBuffer(vertexBuffer);
            destroyBuffer(indexBuffer);
            destroyBuffer(triangleBuffer);
            destroyBuffer(materialBuffer);
            destroyBuffer(lightBuffer);
            for (FrameSlot& slot : frameSlots) {
                if (slot.acquired != VK_NULL_HANDLE)
                    vkDestroySemaphore(device, slot.acquired, nullptr);
                if (slot.fence != VK_NULL_HANDLE) vkDestroyFence(device, slot.fence, nullptr);
                if (slot.pool != VK_NULL_HANDLE)
                    vkDestroyCommandPool(device, slot.pool, nullptr);
                slot = {};
            }
            if (timeline != VK_NULL_HANDLE) vkDestroySemaphore(device, timeline, nullptr);
            if (initializationPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, initializationPool, nullptr);
            vmaDestroyAllocator(allocator);
            allocator = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            SDL_Vulkan_DestroySurface(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (debugMessenger != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
            debugMessenger = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        if (sdlInitialized) {
            SDL_Quit();
            sdlInitialized = false;
        }
    }
};

VulkanApp::VulkanApp() : impl_(std::make_unique<Impl>()) {}
VulkanApp::~VulkanApp() = default;
int VulkanApp::run() { return impl_->run(); }

} // namespace vpt
