#include "renderer.hpp"

#include "materials.hpp"
#include "math.hpp"
#include "scene.hpp"

#include <volk.h>
#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arena {
namespace {

constexpr std::uint32_t frameSlotCount = 2;
constexpr std::uint32_t domainCount = 4;
constexpr VkDeviceSize pathStride = 144;
constexpr VkDeviceSize hitStride = 32;
constexpr VkDeviceSize shadowTaskStride = 64;
constexpr VkDeviceSize branchOutputStride = 160;
constexpr VkDeviceSize surfaceStride = 64;
constexpr VkDeviceSize reservoirStride = 64;
constexpr VkDeviceSize historyStride = 96;
constexpr VkDeviceSize frameDataStride = 160;

void requireVk(VkResult result, const std::string& operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(operation + " 失败，VkResult=" + std::to_string(result));
}

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::vector<std::uint32_t> readSpirv(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("无法打开 shader：" + path.string());
    const std::streamsize size = stream.tellg();
    if (size <= 0 || (size % 4) != 0) throw std::runtime_error("SPIR-V 文件尺寸非法：" + path.string());
    stream.seekg(0);
    std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4U);
    if (!stream.read(reinterpret_cast<char*>(words.data()), size)) throw std::runtime_error("读取 shader 失败：" + path.string());
    return words;
}

float halton(std::uint64_t index, std::uint32_t base) {
    float result = 0.0F;
    float factor = 1.0F;
    while (index > 0U) {
        factor /= static_cast<float>(base);
        result += factor * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

struct Buffer {
    VkBuffer handle{VK_NULL_HANDLE};
    VmaAllocation allocation{};
    VkDeviceSize size{};
    void* mapped{};
};

struct Image {
    VkImage handle{VK_NULL_HANDLE};
    VmaAllocation allocation{};
    VkImageView view{VK_NULL_HANDLE};
};

struct FrameDataCpu {
    Vec4 cameraPositionTanFov{};
    Vec4 cameraForwardAspect{};
    Vec4 cameraRightWidth{};
    Vec4 cameraUpHeight{};
    Vec4 previousPositionTanFov{};
    Vec4 previousForwardAspect{};
    Vec4 previousRightWidth{};
    Vec4 previousUpHeight{};
    Vec4 jitterAndTime{};
    std::array<std::uint32_t, 4> frameInfo{};
};
static_assert(sizeof(FrameDataCpu) == frameDataStride);

struct CameraState {
    Vec3 position{8.0F, 2.5F, 20.0F};
    float yaw{-1.57079632679F};
    float pitch{};

    Vec3 forward() const {
        const float cp = std::cos(pitch);
        return normalize({std::cos(yaw) * cp, std::sin(pitch), std::sin(yaw) * cp});
    }

    Vec3 right() const { return normalize(cross(forward(), {0.0F, 1.0F, 0.0F})); }
    Vec3 up() const { return normalize(cross(right(), forward())); }
};

struct PushConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t bounce{};
    std::uint32_t queueParity{};
    std::uint32_t frameSlot{};
    std::uint32_t frameParity{};
    std::uint32_t phase{};
    std::uint32_t iteration{};
};
static_assert(sizeof(PushConstants) == 32);

struct FrameSlot {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer command{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    VkSemaphore imageAvailable{VK_NULL_HANDLE};
};

enum class ComputeStage : std::uint32_t {
    Primary,
    Shade,
    PrepareIndirect,
    ShadowResolve,
    RestirInitial,
    RestirTemporal,
    RestirSpatial,
    Resolve,
    SvgfTemporal,
    SvgfAtrous,
    Count,
};

struct FeatureChain {
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceShaderObjectFeaturesEXT shaderObject{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamicState{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamicState2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};

    FeatureChain() {
        features.pNext = &v12;
        v12.pNext = &v13;
        v13.pNext = &acceleration;
        acceleration.pNext = &rayTracing;
        rayTracing.pNext = &descriptorBuffer;
        descriptorBuffer.pNext = &shaderObject;
        shaderObject.pNext = &dynamicState;
        dynamicState.pNext = &dynamicState2;
        dynamicState2.pNext = &dynamicState3;
    }
};

const std::array<const char*, 8> requiredDeviceExtensions{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
};

} // 匿名命名空间

struct Application::Impl {
    explicit Impl(std::filesystem::path directory) : executableDirectory(std::move(directory)) {}
    ~Impl() { cleanup(); }

    std::filesystem::path executableDirectory;
    SDL_Window* window{};
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debugMessenger{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    std::uint32_t queueFamily{};
    VkQueue queue{VK_NULL_HANDLE};
    VmaAllocator allocator{};
    VkCommandPool uploadPool{VK_NULL_HANDLE};
    VkCommandBuffer uploadCommand{VK_NULL_HANDLE};
    VkFence uploadFence{VK_NULL_HANDLE};

    VkPhysicalDeviceProperties physicalProperties{};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};

    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFormat swapchainFormat{};
    VkExtent2D extent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    std::vector<VkSemaphore> renderFinished;
    std::vector<VkFence> imageOwners;

    MaterialLibrary materialLibrary;
    SceneMesh scene;
    Buffer vertexBuffer;
    Buffer indexBuffer;
    Buffer materialBuffer;
    Buffer lightBuffer;
    Image baseColorImage;
    Image roughnessImage;
    VkSampler textureSampler{VK_NULL_HANDLE};

    Buffer blasBuffer;
    Buffer tlasBuffer;
    VkAccelerationStructureKHR blas{VK_NULL_HANDLE};
    VkAccelerationStructureKHR tlas{VK_NULL_HANDLE};

    Buffer pathsA;
    Buffer pathsB;
    Buffer hits;
    Buffer counters;
    Buffer indirectCommands;
    Buffer shadowTasks;
    Buffer shadowResults;
    Buffer branchOutputs;
    Buffer surfaces;
    Buffer reservoirs;
    Buffer histories;
    Buffer atrousA;
    Buffer atrousB;
    Buffer frameData;

    VkDescriptorSetLayout descriptorLayout{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    Buffer descriptorBuffer;
    VkDeviceSize descriptorLayoutSize{};
    VkPipeline rayTracingPipeline{VK_NULL_HANDLE};
    Buffer shaderBindingTable;
    VkStridedDeviceAddressRegionKHR surfaceRaygenRegion{};
    VkStridedDeviceAddressRegionKHR shadowRaygenRegion{};
    VkStridedDeviceAddressRegionKHR missRegion{};
    VkStridedDeviceAddressRegionKHR hitRegion{};
    VkStridedDeviceAddressRegionKHR callableRegion{};
    std::array<VkShaderEXT, static_cast<std::size_t>(ComputeStage::Count)> computeShaders{};
    VkShaderEXT vertexShader{VK_NULL_HANDLE};
    VkShaderEXT fragmentShader{VK_NULL_HANDLE};

    std::array<FrameSlot, frameSlotCount> frames{};
    VkSemaphore timeline{VK_NULL_HANDLE};
    std::uint64_t timelineValue{};
    std::uint64_t renderedFrame{};
    std::uint32_t currentFrameSlot{};

    CameraState camera{};
    CameraState previousCamera{};
    Vec3 previousJitter{};
    bool running{true};
    bool mouseCaptured{};
    bool framebufferChanged{};
    bool cameraCut{true};
    bool minimized{};
    bool focused{true};
    std::atomic_bool validationFailed{};
    std::mutex validationMutex;
    std::string validationMessage;
    std::chrono::steady_clock::time_point lastLoopTime{};
    std::chrono::steady_clock::time_point statsStart{};
    std::uint64_t statsFrames{};

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        auto* self = static_cast<Impl*>(user);
        if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0U) {
            std::lock_guard lock(self->validationMutex);
            self->validationFailed.store(true, std::memory_order_relaxed);
            self->validationMessage = data->pMessage != nullptr ? data->pMessage : "未知验证层诊断";
            std::cerr << "Vulkan 验证失败：" << self->validationMessage << '\n';
        } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0U) {
            std::cout << "Vulkan INFO：" << (data->pMessage != nullptr ? data->pMessage : "") << '\n';
        }
        return VK_FALSE;
    }

    void initialize() {
        window = SDL_CreateWindow("VoxelPathTracer — FPS: --  ms/frame: --", 1280, 720,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window == nullptr) throw std::runtime_error(std::string("创建 SDL3 窗口失败：") + SDL_GetError());
        volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()));
        createInstance();
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) throw std::runtime_error(std::string("创建 Vulkan surface 失败：") + SDL_GetError());
        selectPhysicalDevice();
        createDevice();
        volkLoadDevice(device);
        createAllocator();
        createUploadContext();
        createSwapchain();
        materialLibrary = loadMaterials(executableDirectory / "materials.json");
        scene = buildScene(materialLibrary);
        createStaticResources();
        createAccelerationStructures();
        createLayouts();
        createRayTracingPipeline();
        createShaderObjects();
        createPixelResources();
        updateDescriptors();
        createFrameResources();
        previousCamera = camera;
        resetStatistics();
        lastLoopTime = std::chrono::steady_clock::now();
    }

    void createInstance() {
        std::uint32_t loaderVersion = VK_API_VERSION_1_0;
        requireVk(vkEnumerateInstanceVersion(&loaderVersion), "查询 Vulkan loader 版本");
        if (loaderVersion < VK_API_VERSION_1_4) throw std::runtime_error("Vulkan loader 不支持 Vulkan 1.4");

        Uint32 sdlCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
        if (sdlExtensions == nullptr) throw std::runtime_error(std::string("查询 SDL Vulkan 扩展失败：") + SDL_GetError());
        std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlCount);
#ifndef NDEBUG
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        std::uint32_t layerCount = 0;
        requireVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "查询实例层数量");
        std::vector<VkLayerProperties> layers(layerCount);
        requireVk(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "查询实例层");
        const bool validationAvailable = std::ranges::any_of(layers, [](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0; });
        if (!validationAvailable) throw std::runtime_error("Debug 构建要求 VK_LAYER_KHRONOS_validation，但当前不可用");
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
#endif
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "VoxelPathTracer";
        application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        application.pEngineName = "RenderArena";
        application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        application.apiVersion = VK_API_VERSION_1_4;

        VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = debugCallback;
        debugInfo.pUserData = this;

#ifndef NDEBUG
        const VkValidationFeatureEnableEXT synchronizationValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validationFeatures.enabledValidationFeatureCount = 1;
        validationFeatures.pEnabledValidationFeatures = &synchronizationValidation;
        debugInfo.pNext = &validationFeatures;
#endif
        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &application;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
#ifndef NDEBUG
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &validationLayer;
        createInfo.pNext = &debugInfo;
#endif
        requireVk(vkCreateInstance(&createInfo, nullptr, &instance), "创建 Vulkan 1.4 实例");
        volkLoadInstance(instance);
#ifndef NDEBUG
        requireVk(vkCreateDebugUtilsMessengerEXT(instance, &debugInfo, nullptr, &debugMessenger), "创建 Vulkan debug messenger");
#endif
    }

    std::optional<std::uint32_t> findQueueFamily(VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, properties.data());
        for (std::uint32_t i = 0; i < count; ++i) {
            VkBool32 present = VK_FALSE;
            requireVk(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present), "查询呈现队列支持");
            if ((properties[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) && present == VK_TRUE) return i;
        }
        return std::nullopt;
    }

    bool supportsExtensions(VkPhysicalDevice candidate, std::string& missing) const {
        std::uint32_t count = 0;
        requireVk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr), "查询设备扩展数量");
        std::vector<VkExtensionProperties> available(count);
        requireVk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, available.data()), "查询设备扩展");
        std::vector<const char*> required(requiredDeviceExtensions.begin(), requiredDeviceExtensions.end());
        required.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        for (const char* name : required) {
            if (!std::ranges::any_of(available, [name](const VkExtensionProperties& extension) { return std::strcmp(extension.extensionName, name) == 0; })) {
                missing = name;
                return false;
            }
        }
        return true;
    }

    bool supportsFeatures(VkPhysicalDevice candidate, FeatureChain& chain, std::string& missing) const {
        vkGetPhysicalDeviceFeatures2(candidate, &chain.features);
#define REQUIRE_FEATURE(expression, name) if (!(expression)) { missing = name; return false; }
        REQUIRE_FEATURE(chain.v12.timelineSemaphore, "timelineSemaphore")
        REQUIRE_FEATURE(chain.v12.bufferDeviceAddress, "bufferDeviceAddress")
        REQUIRE_FEATURE(chain.v12.scalarBlockLayout, "scalarBlockLayout")
        REQUIRE_FEATURE(chain.v13.dynamicRendering, "dynamicRendering")
        REQUIRE_FEATURE(chain.v13.synchronization2, "synchronization2")
        REQUIRE_FEATURE(chain.acceleration.accelerationStructure, "accelerationStructure")
        REQUIRE_FEATURE(chain.rayTracing.rayTracingPipeline, "rayTracingPipeline")
        REQUIRE_FEATURE(chain.rayTracing.rayTracingPipelineTraceRaysIndirect, "rayTracingPipelineTraceRaysIndirect")
        REQUIRE_FEATURE(chain.descriptorBuffer.descriptorBuffer, "descriptorBuffer")
        REQUIRE_FEATURE(chain.shaderObject.shaderObject, "shaderObject")
        REQUIRE_FEATURE(chain.dynamicState.extendedDynamicState, "extendedDynamicState")
        REQUIRE_FEATURE(chain.dynamicState2.extendedDynamicState2, "extendedDynamicState2")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3DepthClampEnable, "extendedDynamicState3DepthClampEnable")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3PolygonMode, "extendedDynamicState3PolygonMode")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3RasterizationSamples, "extendedDynamicState3RasterizationSamples")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3SampleMask, "extendedDynamicState3SampleMask")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3AlphaToCoverageEnable, "extendedDynamicState3AlphaToCoverageEnable")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3LogicOpEnable, "extendedDynamicState3LogicOpEnable")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3ColorBlendEnable, "extendedDynamicState3ColorBlendEnable")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3ColorBlendEquation, "extendedDynamicState3ColorBlendEquation")
        REQUIRE_FEATURE(chain.dynamicState3.extendedDynamicState3ColorWriteMask, "extendedDynamicState3ColorWriteMask")
#undef REQUIRE_FEATURE
        return true;
    }

    void selectPhysicalDevice() {
        std::uint32_t count = 0;
        requireVk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "查询物理设备数量");
        if (count == 0) throw std::runtime_error("没有 Vulkan 物理设备");
        std::vector<VkPhysicalDevice> devices(count);
        requireVk(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "查询物理设备");
        std::string report;
        int bestScore = -1;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            std::string missing;
            FeatureChain chain;
            const auto family = findQueueFamily(candidate);
            const bool version = properties.apiVersion >= VK_API_VERSION_1_4;
            const bool extensions = supportsExtensions(candidate, missing);
            const bool features = extensions && supportsFeatures(candidate, chain, missing);
            VkFormatProperties colorProperties{};
            VkFormatProperties roughnessProperties{};
            vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R8G8B8A8_SRGB, &colorProperties);
            vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R8_UNORM, &roughnessProperties);
            constexpr VkFormatFeatureFlags textureFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            const bool formats = (colorProperties.optimalTilingFeatures & textureFeatures) == textureFeatures
                && (roughnessProperties.optimalTilingFeatures & textureFeatures) == textureFeatures;
            if (!version || !family.has_value() || !extensions || !features || !formats) {
                report += std::string(properties.deviceName) + "：";
                if (!version) report += "缺少 Vulkan 1.4";
                else if (!family.has_value()) report += "缺少 graphics+compute+present 队列";
                else if (!extensions || !features) report += "缺少 " + missing;
                else report += "缺少所需纹理格式能力";
                report += "\n";
                continue;
            }
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore = score;
                physicalDevice = candidate;
                queueFamily = *family;
                physicalProperties = properties;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) throw std::runtime_error("没有满足完整能力合同的 Vulkan 设备：\n" + report);

        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &descriptorProperties;
        descriptorProperties.pNext = &rayTracingProperties;
        rayTracingProperties.pNext = &accelerationProperties;
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
        if (rayTracingProperties.maxRayRecursionDepth < 1U) throw std::runtime_error("设备 maxRayRecursionDepth 小于 1");
        std::cout << "使用 Vulkan 设备：" << physicalProperties.deviceName << '\n';
    }

    void createDevice() {
        FeatureChain supported;
        std::string missing;
        supportsFeatures(physicalDevice, supported, missing);
        FeatureChain enabled;
        enabled.v12.timelineSemaphore = VK_TRUE;
        enabled.v12.bufferDeviceAddress = VK_TRUE;
        enabled.v12.scalarBlockLayout = VK_TRUE;
        enabled.v13.dynamicRendering = VK_TRUE;
        enabled.v13.synchronization2 = VK_TRUE;
        enabled.acceleration.accelerationStructure = VK_TRUE;
        enabled.rayTracing.rayTracingPipeline = VK_TRUE;
        enabled.rayTracing.rayTracingPipelineTraceRaysIndirect = VK_TRUE;
        enabled.descriptorBuffer.descriptorBuffer = VK_TRUE;
        enabled.shaderObject.shaderObject = VK_TRUE;
        enabled.dynamicState.extendedDynamicState = VK_TRUE;
        enabled.dynamicState2.extendedDynamicState2 = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3DepthClampEnable = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3PolygonMode = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3RasterizationSamples = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3SampleMask = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3AlphaToCoverageEnable = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3LogicOpEnable = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
        enabled.dynamicState3.extendedDynamicState3ColorWriteMask = VK_TRUE;

        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        std::vector<const char*> extensions(requiredDeviceExtensions.begin(), requiredDeviceExtensions.end());
        extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createInfo.pNext = &enabled.features;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        requireVk(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "创建设备");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
    }

    void createAllocator() {
        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        functions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
        functions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
        functions.vkAllocateMemory = vkAllocateMemory;
        functions.vkFreeMemory = vkFreeMemory;
        functions.vkMapMemory = vkMapMemory;
        functions.vkUnmapMemory = vkUnmapMemory;
        functions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
        functions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
        functions.vkBindBufferMemory = vkBindBufferMemory;
        functions.vkBindImageMemory = vkBindImageMemory;
        functions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
        functions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
        functions.vkCreateBuffer = vkCreateBuffer;
        functions.vkDestroyBuffer = vkDestroyBuffer;
        functions.vkCreateImage = vkCreateImage;
        functions.vkDestroyImage = vkDestroyImage;
        functions.vkCmdCopyBuffer = vkCmdCopyBuffer;
        functions.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
        functions.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;
        functions.vkBindBufferMemory2KHR = vkBindBufferMemory2;
        functions.vkBindImageMemory2KHR = vkBindImageMemory2;
        functions.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;
        functions.vkGetDeviceBufferMemoryRequirements = vkGetDeviceBufferMemoryRequirements;
        functions.vkGetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements;
        functions.vkGetPhysicalDeviceProperties2KHR = vkGetPhysicalDeviceProperties2;

        VmaAllocatorCreateInfo info{};
        info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        info.physicalDevice = physicalDevice;
        info.device = device;
        info.instance = instance;
        info.vulkanApiVersion = VK_API_VERSION_1_4;
        info.pVulkanFunctions = &functions;
        requireVk(vmaCreateAllocator(&info, &allocator), "创建 VMA allocator");
    }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool mapped = false, VkDeviceSize minimumAlignment = 1) {
        Buffer result{};
        result.size = size;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = mapped ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocationInfo.minAlignment = minimumAlignment;
        if (mapped) allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo actual{};
        requireVk(vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &result.handle, &result.allocation, &actual), "创建 buffer");
        result.mapped = actual.pMappedData;
        return result;
    }

    void destroyBuffer(Buffer& buffer) {
        if (buffer.handle != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, buffer.handle, buffer.allocation);
        buffer = {};
    }

    VkDeviceAddress bufferAddress(const Buffer& buffer) const {
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = buffer.handle;
        return vkGetBufferDeviceAddress(device, &info);
    }

    void createUploadContext() {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        requireVk(vkCreateCommandPool(device, &poolInfo, nullptr, &uploadPool), "创建上传命令池");
        VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = uploadPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        requireVk(vkAllocateCommandBuffers(device, &allocateInfo, &uploadCommand), "分配上传命令缓冲");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        requireVk(vkCreateFence(device, &fenceInfo, nullptr, &uploadFence), "创建上传 fence");
    }

    void immediate(const std::function<void(VkCommandBuffer)>& record) {
        requireVk(vkResetFences(device, 1, &uploadFence), "重置上传 fence");
        requireVk(vkResetCommandBuffer(uploadCommand, 0), "重置上传命令缓冲");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        requireVk(vkBeginCommandBuffer(uploadCommand, &begin), "开始上传命令缓冲");
        record(uploadCommand);
        requireVk(vkEndCommandBuffer(uploadCommand), "结束上传命令缓冲");
        VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = uploadCommand;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commandInfo;
        requireVk(vkQueueSubmit2(queue, 1, &submit, uploadFence), "提交初始化命令");
        requireVk(vkWaitForFences(device, 1, &uploadFence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "等待初始化命令");
    }

    Buffer uploadBuffer(std::span<const std::byte> bytes, VkBufferUsageFlags usage, VkDeviceSize minimumAlignment = 1) {
        Buffer destination = createBuffer(bytes.size(), usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, minimumAlignment);
        Buffer staging = createBuffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::memcpy(staging.mapped, bytes.data(), bytes.size());
        vmaFlushAllocation(allocator, staging.allocation, 0, bytes.size());
        immediate([&](VkCommandBuffer command) {
            VkBufferCopy copy{0, 0, bytes.size()};
            vkCmdCopyBuffer(command, staging.handle, destination.handle, 1, &copy);
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            barrier.buffer = destination.handle;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
        });
        destroyBuffer(staging);
        return destination;
    }

    void createSwapchain() {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height)) throw std::runtime_error(std::string("查询 framebuffer 尺寸失败：") + SDL_GetError());
        if (width <= 0 || height <= 0) {
            minimized = true;
            return;
        }
        minimized = false;
        VkSurfaceCapabilitiesKHR capabilities{};
        requireVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities), "查询 surface 能力");
        std::uint32_t formatCount = 0;
        requireVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr), "查询 surface 格式数量");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        requireVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data()), "查询 surface 格式");
        const auto format = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& value) {
            return (value.format == VK_FORMAT_B8G8R8A8_SRGB || value.format == VK_FORMAT_R8G8B8A8_SRGB) && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (format == formats.end()) throw std::runtime_error("surface 没有可用的 sRGB 交换链格式");
        swapchainFormat = format->format;
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) extent = capabilities.currentExtent;
        else {
            extent.width = std::clamp(static_cast<std::uint32_t>(width), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<std::uint32_t>(height), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        std::uint32_t imageCount = std::max(capabilities.minImageCount + 1U, 2U);
        if (capabilities.maxImageCount > 0U) imageCount = std::min(imageCount, capabilities.maxImageCount);
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = imageCount;
        info.imageFormat = swapchainFormat;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = capabilities.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        requireVk(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "创建交换链");
        requireVk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr), "查询交换链图像数量");
        swapchainImages.resize(imageCount);
        requireVk(vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data()), "查询交换链图像");
        swapchainViews.resize(imageCount);
        renderFinished.resize(imageCount);
        imageOwners.assign(imageCount, VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (std::uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = swapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            requireVk(vkCreateImageView(device, &viewInfo, nullptr, &swapchainViews[i]), "创建交换链 image view");
            requireVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished[i]), "创建 present semaphore");
        }
    }

    void createTextureImages() {
        const std::uint32_t layers = static_cast<std::uint32_t>(materialLibrary.materials.size());
        const std::uint32_t levels = static_cast<std::uint32_t>(materialLibrary.mips.size());
        if (layers > physicalProperties.limits.maxImageArrayLayers
            || materialLibrary.textureSize > physicalProperties.limits.maxImageDimension2D) {
            throw std::runtime_error("材质纹理尺寸或数组层数超过设备限制");
        }
        auto create = [&](VkFormat format, Image& image) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {materialLibrary.textureSize, materialLibrary.textureSize, 1};
            info.mipLevels = levels;
            info.arrayLayers = layers;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            requireVk(vmaCreateImage(allocator, &info, &allocation, &image.handle, &image.allocation, nullptr), "创建材质纹理");
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.handle;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            view.format = format;
            view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view.subresourceRange.levelCount = levels;
            view.subresourceRange.layerCount = layers;
            requireVk(vkCreateImageView(device, &view, nullptr, &image.view), "创建材质纹理 view");
        };
        create(VK_FORMAT_R8G8B8A8_SRGB, baseColorImage);
        create(VK_FORMAT_R8_UNORM, roughnessImage);

        VkDeviceSize colorBytes = 0;
        VkDeviceSize roughnessBytes = 0;
        for (const TextureMip& mip : materialLibrary.mips) {
            colorBytes += mip.colorRgba.size();
            roughnessBytes += mip.roughness.size();
        }
        Buffer colorStaging = createBuffer(colorBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        Buffer roughnessStaging = createBuffer(roughnessBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::vector<VkBufferImageCopy> colorCopies;
        std::vector<VkBufferImageCopy> roughnessCopies;
        VkDeviceSize colorOffset = 0;
        VkDeviceSize roughnessOffset = 0;
        for (std::uint32_t level = 0; level < levels; ++level) {
            const TextureMip& mip = materialLibrary.mips[level];
            std::memcpy(static_cast<std::byte*>(colorStaging.mapped) + colorOffset, mip.colorRgba.data(), mip.colorRgba.size());
            std::memcpy(static_cast<std::byte*>(roughnessStaging.mapped) + roughnessOffset, mip.roughness.data(), mip.roughness.size());
            VkBufferImageCopy colorCopy{};
            colorCopy.bufferOffset = colorOffset;
            colorCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            colorCopy.imageSubresource.mipLevel = level;
            colorCopy.imageSubresource.layerCount = layers;
            colorCopy.imageExtent = {mip.width, mip.height, 1};
            colorCopies.push_back(colorCopy);
            VkBufferImageCopy roughnessCopy = colorCopy;
            roughnessCopy.bufferOffset = roughnessOffset;
            roughnessCopies.push_back(roughnessCopy);
            colorOffset += mip.colorRgba.size();
            roughnessOffset += mip.roughness.size();
        }
        vmaFlushAllocation(allocator, colorStaging.allocation, 0, VK_WHOLE_SIZE);
        vmaFlushAllocation(allocator, roughnessStaging.allocation, 0, VK_WHOLE_SIZE);
        immediate([&](VkCommandBuffer command) {
            std::array<VkImageMemoryBarrier2, 2> beginBarriers{};
            for (std::uint32_t i = 0; i < 2; ++i) {
                beginBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                beginBarriers[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                beginBarriers[i].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                beginBarriers[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                beginBarriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                beginBarriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                beginBarriers[i].image = i == 0 ? baseColorImage.handle : roughnessImage.handle;
                beginBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                beginBarriers[i].subresourceRange.levelCount = levels;
                beginBarriers[i].subresourceRange.layerCount = layers;
            }
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 2;
            dependency.pImageMemoryBarriers = beginBarriers.data();
            vkCmdPipelineBarrier2(command, &dependency);
            vkCmdCopyBufferToImage(command, colorStaging.handle, baseColorImage.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, colorCopies.data());
            vkCmdCopyBufferToImage(command, roughnessStaging.handle, roughnessImage.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, roughnessCopies.data());
            for (auto& barrier : beginBarriers) {
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            vkCmdPipelineBarrier2(command, &dependency);
        });
        destroyBuffer(colorStaging);
        destroyBuffer(roughnessStaging);

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.maxLod = static_cast<float>(levels - 1U);
        requireVk(vkCreateSampler(device, &sampler, nullptr, &textureSampler), "创建材质 sampler");
    }

    void createStaticResources() {
        vertexBuffer = uploadBuffer(std::as_bytes(std::span(scene.vertices)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        indexBuffer = uploadBuffer(std::as_bytes(std::span(scene.indices)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        materialBuffer = uploadBuffer(std::as_bytes(std::span(materialLibrary.gpuMaterials)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        lightBuffer = uploadBuffer(std::as_bytes(std::span(scene.lights)), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        createTextureImages();
        frameData = createBuffer(frameDataStride * frameSlotCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }

    VkAccelerationStructureKHR createAccelerationStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size, Buffer& storage) {
        storage = createBuffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
        VkAccelerationStructureCreateInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        info.buffer = storage.handle;
        info.size = size;
        info.type = type;
        VkAccelerationStructureKHR result = VK_NULL_HANDLE;
        requireVk(vkCreateAccelerationStructureKHR(device, &info, nullptr, &result), "创建 acceleration structure");
        return result;
    }

    void createAccelerationStructures() {
        VkAccelerationStructureGeometryTrianglesDataKHR triangles{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = bufferAddress(vertexBuffer);
        triangles.vertexStride = sizeof(Vertex);
        triangles.maxVertex = static_cast<std::uint32_t>(scene.vertices.size() - 1U);
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = bufferAddress(indexBuffer);
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = triangles;
        const std::uint32_t primitiveCount = static_cast<std::uint32_t>(scene.indices.size() / 3U);
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizes);
        Buffer uncompactedBuffer;
        VkAccelerationStructureKHR uncompacted = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize, uncompactedBuffer);
        Buffer scratch = createBuffer(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, accelerationProperties.minAccelerationStructureScratchOffsetAlignment);
        buildInfo.dstAccelerationStructure = uncompacted;
        buildInfo.scratchData.deviceAddress = bufferAddress(scratch);
        VkAccelerationStructureBuildRangeInfoKHR range{primitiveCount, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &range;
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        queryInfo.queryCount = 1;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        requireVk(vkCreateQueryPool(device, &queryInfo, nullptr, &queryPool), "创建 BLAS compact query");
        immediate([&](VkCommandBuffer command) {
            vkCmdResetQueryPool(command, queryPool, 0, 1);
            vkCmdBuildAccelerationStructuresKHR(command, 1, &buildInfo, &rangePointer);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
            vkCmdWriteAccelerationStructuresPropertiesKHR(command, 1, &uncompacted, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, queryPool, 0);
        });
        VkDeviceSize compactSize = 0;
        requireVk(vkGetQueryPoolResults(device, queryPool, 0, 1, sizeof(compactSize), &compactSize, sizeof(compactSize), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "读取 BLAS 压缩尺寸");
        blas = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, compactSize, blasBuffer);
        immediate([&](VkCommandBuffer command) {
            VkCopyAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
            copy.src = uncompacted;
            copy.dst = blas;
            copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkCmdCopyAccelerationStructureKHR(command, &copy);
        });
        vkDestroyAccelerationStructureKHR(device, uncompacted, nullptr);
        destroyBuffer(uncompactedBuffer);
        destroyBuffer(scratch);
        vkDestroyQueryPool(device, queryPool, nullptr);

        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addressInfo.accelerationStructure = blas;
        const VkDeviceAddress blasAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
        VkAccelerationStructureInstanceKHR instanceData{};
        instanceData.transform = {{{1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, 0.0F}}};
        instanceData.instanceCustomIndex = 0;
        instanceData.mask = 0xff;
        instanceData.instanceShaderBindingTableRecordOffset = 0;
        instanceData.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instanceData.accelerationStructureReference = blasAddress;
        Buffer instanceBuffer = uploadBuffer(std::as_bytes(std::span(&instanceData, 1)), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 16);
        VkAccelerationStructureGeometryInstancesDataKHR instances{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        instances.data.deviceAddress = bufferAddress(instanceBuffer);
        VkAccelerationStructureGeometryKHR tlasGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeometry.geometry.instances = instances;
        VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuild.geometryCount = 1;
        tlasBuild.pGeometries = &tlasGeometry;
        const std::uint32_t instanceCount = 1;
        VkAccelerationStructureBuildSizesInfoKHR tlasSizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild, &instanceCount, &tlasSizes);
        tlas = createAccelerationStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlasSizes.accelerationStructureSize, tlasBuffer);
        Buffer tlasScratch = createBuffer(tlasSizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, accelerationProperties.minAccelerationStructureScratchOffsetAlignment);
        tlasBuild.dstAccelerationStructure = tlas;
        tlasBuild.scratchData.deviceAddress = bufferAddress(tlasScratch);
        VkAccelerationStructureBuildRangeInfoKHR tlasRange{1, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* tlasRangePointer = &tlasRange;
        immediate([&](VkCommandBuffer command) {
            VkMemoryBarrier2 uploadBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            uploadBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            uploadBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            uploadBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            uploadBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &uploadBarrier;
            vkCmdPipelineBarrier2(command, &dependency);
            vkCmdBuildAccelerationStructuresKHR(command, 1, &tlasBuild, &tlasRangePointer);
            uploadBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            uploadBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            uploadBarrier.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
            uploadBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier2(command, &dependency);
        });
        destroyBuffer(tlasScratch);
        destroyBuffer(instanceBuffer);
    }

    void createLayouts() {
        std::array<VkDescriptorSetLayoutBinding, 21> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
        }
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        requireVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorLayout), "创建 descriptor buffer layout");
        vkGetDescriptorSetLayoutSizeEXT(device, descriptorLayout, &descriptorLayoutSize);
        descriptorLayoutSize = alignUp(descriptorLayoutSize, descriptorProperties.descriptorBufferOffsetAlignment);
        if (descriptorProperties.maxResourceDescriptorBufferBindings < 1U
            || descriptorLayoutSize > descriptorProperties.maxResourceDescriptorBufferRange) {
            throw std::runtime_error("descriptor buffer 布局超过设备容量");
        }

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_ALL;
        push.size = sizeof(PushConstants);
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount = 1;
        pipelineInfo.pSetLayouts = &descriptorLayout;
        pipelineInfo.pushConstantRangeCount = 1;
        pipelineInfo.pPushConstantRanges = &push;
        requireVk(vkCreatePipelineLayout(device, &pipelineInfo, nullptr, &pipelineLayout), "创建 pipeline layout");
    }

    VkShaderModule createModule(const std::filesystem::path& path) {
        const auto code = readSpirv(path);
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        requireVk(vkCreateShaderModule(device, &info, nullptr, &module), "创建 shader module " + path.string());
        return module;
    }

    void createRayTracingPipeline() {
        const auto shaderPath = executableDirectory / "shaders";
        std::array<VkShaderModule, 6> modules{
            createModule(shaderPath / "surface.rgen.spv"), createModule(shaderPath / "shadow.rgen.spv"),
            createModule(shaderPath / "surface.rmiss.spv"), createModule(shaderPath / "shadow.rmiss.spv"),
            createModule(shaderPath / "surface.rchit.spv"), createModule(shaderPath / "shadow.rchit.spv")};
        const std::array<VkShaderStageFlagBits, 6> stageTypes{
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
            VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
        std::array<VkPipelineShaderStageCreateInfo, 6> stages{};
        for (std::uint32_t i = 0; i < stages.size(); ++i) {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = stageTypes[i];
            stages[i].module = modules[i];
            stages[i].pName = "main";
        }
        std::array<VkRayTracingShaderGroupCreateInfoKHR, 6> groups{};
        for (auto& group : groups) {
            group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            group.generalShader = VK_SHADER_UNUSED_KHR;
            group.closestHitShader = VK_SHADER_UNUSED_KHR;
            group.anyHitShader = VK_SHADER_UNUSED_KHR;
            group.intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        for (std::uint32_t i = 0; i < 4; ++i) {
            groups[i].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            groups[i].generalShader = i;
        }
        groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[4].closestHitShader = 4;
        groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[5].closestHitShader = 5;
        VkRayTracingPipelineCreateInfoKHR pipelineInfo{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pipelineInfo.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.groupCount = static_cast<std::uint32_t>(groups.size());
        pipelineInfo.pGroups = groups.data();
        pipelineInfo.maxPipelineRayRecursionDepth = 1;
        pipelineInfo.layout = pipelineLayout;
        const VkResult pipelineResult = vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rayTracingPipeline);
        for (VkShaderModule module : modules) vkDestroyShaderModule(device, module, nullptr);
        requireVk(pipelineResult, "创建 ray tracing pipeline");

        const VkDeviceSize handleSize = rayTracingProperties.shaderGroupHandleSize;
        const VkDeviceSize recordAlignment = rayTracingProperties.shaderGroupHandleAlignment;
        const VkDeviceSize baseAlignment = rayTracingProperties.shaderGroupBaseAlignment;
        const VkDeviceSize recordStride = alignUp(handleSize, recordAlignment);
        if (recordStride > rayTracingProperties.maxShaderGroupStride) throw std::runtime_error("SBT record stride 超过设备限制");
        const VkDeviceSize surfaceOffset = 0;
        const VkDeviceSize shadowOffset = baseAlignment;
        const VkDeviceSize missOffset = alignUp(shadowOffset + baseAlignment, baseAlignment);
        const VkDeviceSize hitOffset = alignUp(missOffset + recordStride * 2U, baseAlignment);
        const VkDeviceSize tableSize = hitOffset + recordStride * 2U;
        std::vector<std::byte> handles(handleSize * groups.size());
        requireVk(vkGetRayTracingShaderGroupHandlesKHR(device, rayTracingPipeline, 0, static_cast<std::uint32_t>(groups.size()), handles.size(), handles.data()), "读取 SBT handles");
        shaderBindingTable = createBuffer(tableSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, true, baseAlignment);
        std::memset(shaderBindingTable.mapped, 0, static_cast<std::size_t>(tableSize));
        auto copyHandle = [&](std::uint32_t group, VkDeviceSize offset) {
            std::memcpy(static_cast<std::byte*>(shaderBindingTable.mapped) + offset, handles.data() + handleSize * group, static_cast<std::size_t>(handleSize));
        };
        copyHandle(0, surfaceOffset);
        copyHandle(1, shadowOffset);
        copyHandle(2, missOffset);
        copyHandle(3, missOffset + recordStride);
        copyHandle(4, hitOffset);
        copyHandle(5, hitOffset + recordStride);
        vmaFlushAllocation(allocator, shaderBindingTable.allocation, 0, VK_WHOLE_SIZE);
        const VkDeviceAddress address = bufferAddress(shaderBindingTable);
        surfaceRaygenRegion = {address + surfaceOffset, baseAlignment, baseAlignment};
        shadowRaygenRegion = {address + shadowOffset, baseAlignment, baseAlignment};
        missRegion = {address + missOffset, recordStride, recordStride * 2U};
        hitRegion = {address + hitOffset, recordStride, recordStride * 2U};
    }

    VkShaderEXT createShaderObject(const std::filesystem::path& path, VkShaderStageFlagBits stage, VkShaderStageFlags nextStage, VkShaderCreateFlagsEXT flags = 0) {
        const auto code = readSpirv(path);
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_ALL;
        push.size = sizeof(PushConstants);
        VkShaderCreateInfoEXT info{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        info.flags = flags;
        info.stage = stage;
        info.nextStage = nextStage;
        info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        info.pName = "main";
        info.setLayoutCount = 1;
        info.pSetLayouts = &descriptorLayout;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push;
        VkShaderEXT shader = VK_NULL_HANDLE;
        requireVk(vkCreateShadersEXT(device, 1, &info, nullptr, &shader), "创建 shader object " + path.string());
        return shader;
    }

    void createShaderObjects() {
        const auto directory = executableDirectory / "shaders";
        const std::array<const char*, static_cast<std::size_t>(ComputeStage::Count)> names{
            "primary.comp.spv", "shade.comp.spv", "prepare_indirect.comp.spv", "shadow_resolve.comp.spv",
            "restir_initial.comp.spv", "restir_temporal.comp.spv", "restir_spatial.comp.spv", "resolve.comp.spv",
            "svgf_temporal.comp.spv", "svgf_atrous.comp.spv"};
        for (std::size_t i = 0; i < names.size(); ++i) computeShaders[i] = createShaderObject(directory / names[i], VK_SHADER_STAGE_COMPUTE_BIT, 0);
        const auto vertexCode = readSpirv(directory / "display.vert.spv");
        const auto fragmentCode = readSpirv(directory / "display.frag.spv");
        VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, sizeof(PushConstants)};
        std::array<VkShaderCreateInfoEXT, 2> infos{};
        const std::array<std::span<const std::uint32_t>, 2> codes{std::span(vertexCode), std::span(fragmentCode)};
        const std::array<VkShaderStageFlagBits, 2> stages{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        for (std::uint32_t i = 0; i < 2; ++i) {
            infos[i].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
            infos[i].flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
            infos[i].stage = stages[i];
            infos[i].nextStage = i == 0 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
            infos[i].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
            infos[i].codeSize = codes[i].size_bytes();
            infos[i].pCode = codes[i].data();
            infos[i].pName = "main";
            infos[i].setLayoutCount = 1;
            infos[i].pSetLayouts = &descriptorLayout;
            infos[i].pushConstantRangeCount = 1;
            infos[i].pPushConstantRanges = &push;
        }
        std::array<VkShaderEXT, 2> shaders{};
        requireVk(vkCreateShadersEXT(device, 2, infos.data(), nullptr, shaders.data()), "创建显示 shader objects");
        vertexShader = shaders[0];
        fragmentShader = shaders[1];
    }

    void createPixelResources() {
        const VkDeviceSize pixels = static_cast<VkDeviceSize>(extent.width) * extent.height;
        const VkDeviceSize capacity = pixels * 2U;
        if (pixels == 0U || pixels > std::numeric_limits<std::uint32_t>::max() / domainCount
            || capacity > rayTracingProperties.maxRayDispatchInvocationCount) {
            throw std::runtime_error("framebuffer 尺寸超过光线调度容量");
        }
        const VkDeviceSize maxStorageRange = physicalProperties.limits.maxStorageBufferRange;
        const std::array<VkDeviceSize, 10> requiredRanges{
            capacity * pathStride, capacity * hitStride, capacity * shadowTaskStride,
            capacity * sizeof(std::uint32_t), capacity * branchOutputStride,
            pixels * 2U * surfaceStride, pixels * 4U * reservoirStride,
            pixels * 2U * domainCount * historyStride,
            pixels * domainCount * sizeof(Vec4), frameDataStride * frameSlotCount};
        if (std::ranges::any_of(requiredRanges, [maxStorageRange](VkDeviceSize size) { return size > maxStorageRange; })) {
            throw std::runtime_error("framebuffer 资源超过 maxStorageBufferRange");
        }
        const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        pathsA = createBuffer(capacity * pathStride, storage);
        pathsB = createBuffer(capacity * pathStride, storage);
        hits = createBuffer(capacity * hitStride, storage);
        counters = createBuffer(16, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        indirectCommands = createBuffer(64, storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        shadowTasks = createBuffer(capacity * shadowTaskStride, storage);
        shadowResults = createBuffer(capacity * sizeof(std::uint32_t), storage);
        branchOutputs = createBuffer(capacity * branchOutputStride, storage);
        surfaces = createBuffer(pixels * 2U * surfaceStride, storage);
        reservoirs = createBuffer(pixels * 4U * reservoirStride, storage);
        histories = createBuffer(pixels * 2U * domainCount * historyStride, storage);
        atrousA = createBuffer(pixels * domainCount * sizeof(Vec4), storage);
        atrousB = createBuffer(pixels * domainCount * sizeof(Vec4), storage);
        cameraCut = true;
        renderedFrame = 0;
    }

    void destroyPixelResources() {
        destroyBuffer(pathsA);
        destroyBuffer(pathsB);
        destroyBuffer(hits);
        destroyBuffer(counters);
        destroyBuffer(indirectCommands);
        destroyBuffer(shadowTasks);
        destroyBuffer(shadowResults);
        destroyBuffer(branchOutputs);
        destroyBuffer(surfaces);
        destroyBuffer(reservoirs);
        destroyBuffer(histories);
        destroyBuffer(atrousA);
        destroyBuffer(atrousB);
        destroyBuffer(descriptorBuffer);
    }

    void updateDescriptors() {
        destroyBuffer(descriptorBuffer);
        descriptorBuffer = createBuffer(descriptorLayoutSize, VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT, true, descriptorProperties.descriptorBufferOffsetAlignment);
        std::memset(descriptorBuffer.mapped, 0, static_cast<std::size_t>(descriptorLayoutSize));
        VkAccelerationStructureDeviceAddressInfoKHR accelerationAddress{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        accelerationAddress.accelerationStructure = tlas;
        const VkDeviceAddress tlasAddress = vkGetAccelerationStructureDeviceAddressKHR(device, &accelerationAddress);

        auto writeDescriptor = [&](std::uint32_t binding, VkDescriptorType type, const VkDescriptorDataEXT& data, std::size_t descriptorSize) {
            VkDeviceSize offset = 0;
            vkGetDescriptorSetLayoutBindingOffsetEXT(device, descriptorLayout, binding, &offset);
            VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            info.type = type;
            info.data = data;
            vkGetDescriptorEXT(device, &info, descriptorSize, static_cast<std::byte*>(descriptorBuffer.mapped) + offset);
        };
        VkDescriptorDataEXT accelerationData{};
        accelerationData.accelerationStructure = tlasAddress;
        writeDescriptor(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, accelerationData, descriptorProperties.accelerationStructureDescriptorSize);

        const std::array<const Buffer*, 18> buffers{
            &vertexBuffer, &indexBuffer, &materialBuffer,
            &pathsA, &pathsB, &hits, &counters, &indirectCommands, &shadowTasks, &shadowResults,
            &branchOutputs, &surfaces, &reservoirs, &histories, &atrousA, &atrousB, &lightBuffer, &frameData};
        const std::array<std::uint32_t, 18> bindings{1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            VkDescriptorAddressInfoEXT address{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
            address.address = bufferAddress(*buffers[i]);
            address.range = buffers[i]->size;
            VkDescriptorDataEXT data{};
            data.pStorageBuffer = &address;
            writeDescriptor(bindings[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, data, descriptorProperties.storageBufferDescriptorSize);
        }
        const std::array<Image*, 2> images{&baseColorImage, &roughnessImage};
        for (std::uint32_t i = 0; i < 2; ++i) {
            VkDescriptorImageInfo imageInfo{textureSampler, images[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkDescriptorDataEXT data{};
            data.pCombinedImageSampler = &imageInfo;
            writeDescriptor(4U + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, data, descriptorProperties.combinedImageSamplerDescriptorSize);
        }
        vmaFlushAllocation(allocator, descriptorBuffer.allocation, 0, VK_WHOLE_SIZE);
    }

    void createFrameResources() {
        VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo timelineInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        timelineInfo.pNext = &timelineType;
        requireVk(vkCreateSemaphore(device, &timelineInfo, nullptr, &timeline), "创建 timeline semaphore");
        for (FrameSlot& frame : frames) {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamily;
            requireVk(vkCreateCommandPool(device, &poolInfo, nullptr, &frame.pool), "创建 frame command pool");
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = frame.pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            requireVk(vkAllocateCommandBuffers(device, &allocate, &frame.command), "分配 frame command buffer");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            requireVk(vkCreateFence(device, &fence, nullptr, &frame.fence), "创建 frame fence");
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            requireVk(vkCreateSemaphore(device, &semaphore, nullptr, &frame.imageAvailable), "创建 acquire semaphore");
        }
    }

    void cleanupSwapchain() {
        for (VkSemaphore semaphore : renderFinished) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
        for (VkImageView view : swapchainViews) if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
        renderFinished.clear();
        swapchainViews.clear();
        swapchainImages.clear();
        imageOwners.clear();
        if (swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }

    void cleanup() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        if (device != VK_NULL_HANDLE) {
            for (FrameSlot& frame : frames) {
                if (frame.imageAvailable != VK_NULL_HANDLE) vkDestroySemaphore(device, frame.imageAvailable, nullptr);
                if (frame.fence != VK_NULL_HANDLE) vkDestroyFence(device, frame.fence, nullptr);
                if (frame.pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, frame.pool, nullptr);
            }
            if (timeline != VK_NULL_HANDLE) vkDestroySemaphore(device, timeline, nullptr);
            cleanupSwapchain();
            destroyPixelResources();
            for (VkShaderEXT shader : computeShaders) if (shader != VK_NULL_HANDLE) vkDestroyShaderEXT(device, shader, nullptr);
            if (vertexShader != VK_NULL_HANDLE) vkDestroyShaderEXT(device, vertexShader, nullptr);
            if (fragmentShader != VK_NULL_HANDLE) vkDestroyShaderEXT(device, fragmentShader, nullptr);
            if (rayTracingPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, rayTracingPipeline, nullptr);
            destroyBuffer(shaderBindingTable);
            if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
            if (tlas != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(device, tlas, nullptr);
            if (blas != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(device, blas, nullptr);
            destroyBuffer(tlasBuffer);
            destroyBuffer(blasBuffer);
            if (textureSampler != VK_NULL_HANDLE) vkDestroySampler(device, textureSampler, nullptr);
            if (baseColorImage.view != VK_NULL_HANDLE) vkDestroyImageView(device, baseColorImage.view, nullptr);
            if (roughnessImage.view != VK_NULL_HANDLE) vkDestroyImageView(device, roughnessImage.view, nullptr);
            if (baseColorImage.handle != VK_NULL_HANDLE) vmaDestroyImage(allocator, baseColorImage.handle, baseColorImage.allocation);
            if (roughnessImage.handle != VK_NULL_HANDLE) vmaDestroyImage(allocator, roughnessImage.handle, roughnessImage.allocation);
            destroyBuffer(frameData);
            destroyBuffer(lightBuffer);
            destroyBuffer(materialBuffer);
            destroyBuffer(indexBuffer);
            destroyBuffer(vertexBuffer);
            if (uploadFence != VK_NULL_HANDLE) vkDestroyFence(device, uploadFence, nullptr);
            if (uploadPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, uploadPool, nullptr);
            if (allocator != nullptr) vmaDestroyAllocator(allocator);
            vkDestroyDevice(device, nullptr);
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) SDL_Vulkan_DestroySurface(instance, surface, nullptr);
        if (debugMessenger != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) vkDestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        if (window != nullptr) SDL_DestroyWindow(window);
    }

    void resetStatistics() {
        statsStart = std::chrono::steady_clock::now();
        statsFrames = 0;
        SDL_SetWindowTitle(window, "VoxelPathTracer — FPS: --  ms/frame: --");
    }

    void updateStatistics() {
        ++statsFrames;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - statsStart).count();
        if (seconds >= 0.5) {
            const double fps = static_cast<double>(statsFrames) / seconds;
            const double milliseconds = 1000.0 / fps;
            char title[160]{};
            std::snprintf(title, sizeof(title), "VoxelPathTracer — FPS: %.1f  ms/frame: %.2f", fps, milliseconds);
            SDL_SetWindowTitle(window, title);
            statsStart = now;
            statsFrames = 0;
        }
    }

    void releaseMouse() {
        if (mouseCaptured) {
            SDL_SetWindowRelativeMouseMode(window, false);
            mouseCaptured = false;
        }
    }

    void resetCamera() {
        camera = {};
        camera.position = {8.0F, 2.5F, 20.0F};
        camera.yaw = -1.57079632679F;
        camera.pitch = 0.0F;
        cameraCut = true;
    }

    void processEvents() {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED) framebufferChanged = true;
            else if (event.type == SDL_EVENT_WINDOW_MINIMIZED) { minimized = true; releaseMouse(); resetStatistics(); }
            else if (event.type == SDL_EVENT_WINDOW_RESTORED) { minimized = false; framebufferChanged = true; lastLoopTime = std::chrono::steady_clock::now(); }
            else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) { focused = false; releaseMouse(); lastLoopTime = std::chrono::steady_clock::now(); }
            else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) { focused = true; lastLoopTime = std::chrono::steady_clock::now(); }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
                if (SDL_SetWindowRelativeMouseMode(window, true)) mouseCaptured = true;
            } else if (event.type == SDL_EVENT_MOUSE_MOTION && mouseCaptured) {
                camera.yaw += event.motion.xrel * 0.0025F;
                camera.pitch = std::clamp(camera.pitch - event.motion.yrel * 0.0025F, -1.55334303427F, 1.55334303427F);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_R) resetCamera();
                else if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (mouseCaptured) releaseMouse();
                    else running = false;
                }
            }
        }
    }

    void updateCamera(float deltaSeconds) {
        if (!focused || minimized) return;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        Vec3 horizontalForward = camera.forward();
        horizontalForward.y = 0.0F;
        horizontalForward = normalize(horizontalForward);
        Vec3 horizontalRight = normalize(cross(horizontalForward, {0.0F, 1.0F, 0.0F}));
        Vec3 movement{};
        if (keys[SDL_SCANCODE_W]) movement += horizontalForward;
        if (keys[SDL_SCANCODE_S]) movement += horizontalForward * -1.0F;
        if (keys[SDL_SCANCODE_D]) movement += horizontalRight;
        if (keys[SDL_SCANCODE_A]) movement += horizontalRight * -1.0F;
        if (keys[SDL_SCANCODE_E]) movement.y += 1.0F;
        if (keys[SDL_SCANCODE_Q]) movement.y -= 1.0F;
        if (length(movement) > 0.0F) {
            const float speed = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] ? 9.0F : 3.0F;
            camera.position += normalize(movement) * speed * deltaSeconds;
        }
    }

    bool cameraMoved() const {
        return length(camera.position - previousCamera.position) > 1e-6F || std::abs(camera.yaw - previousCamera.yaw) > 1e-6F || std::abs(camera.pitch - previousCamera.pitch) > 1e-6F;
    }

    void recreateSwapchain() {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        if (width <= 0 || height <= 0) {
            minimized = true;
            return;
        }
        requireVk(vkDeviceWaitIdle(device), "交换链重建前等待设备");
        cleanupSwapchain();
        destroyPixelResources();
        createSwapchain();
        createPixelResources();
        updateDescriptors();
        framebufferChanged = false;
        cameraCut = true;
        previousCamera = camera;
        previousJitter = {};
        resetStatistics();
    }

    void memoryBarrier(VkCommandBuffer command, VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess,
        VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = sourceStage;
        barrier.srcAccessMask = sourceAccess;
        barrier.dstStageMask = destinationStage;
        barrier.dstAccessMask = destinationAccess;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    void bindDescriptors(VkCommandBuffer command, VkPipelineBindPoint bindPoint) {
        VkDescriptorBufferBindingInfoEXT binding{VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
        binding.address = bufferAddress(descriptorBuffer);
        binding.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
        vkCmdBindDescriptorBuffersEXT(command, 1, &binding);
        const std::uint32_t index = 0;
        const VkDeviceSize offset = 0;
        vkCmdSetDescriptorBufferOffsetsEXT(command, bindPoint, pipelineLayout, 0, 1, &index, &offset);
    }

    void bindCompute(VkCommandBuffer command, ComputeStage stage) {
        const VkShaderStageFlagBits shaderStage = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkShaderEXT shader = computeShaders[static_cast<std::size_t>(stage)];
        vkCmdBindShadersEXT(command, 1, &shaderStage, &shader);
    }

    void pushConstants(VkCommandBuffer command, const PushConstants& push) {
        vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    }
    void dispatchLinear(VkCommandBuffer command, std::uint64_t invocations) const {
        const std::uint64_t groups = std::max<std::uint64_t>((invocations + 63U) / 64U, 1U);
        const std::uint64_t groupWidth = std::min<std::uint64_t>(groups, physicalProperties.limits.maxComputeWorkGroupCount[0]);
        const std::uint64_t groupHeight = (groups + groupWidth - 1U) / groupWidth;
        if (groupHeight > physicalProperties.limits.maxComputeWorkGroupCount[1]) {
            throw std::runtime_error("framebuffer 尺寸超过 compute 多维调度容量");
        }
        vkCmdDispatch(command, static_cast<std::uint32_t>(groupWidth), static_cast<std::uint32_t>(groupHeight), 1);
    }


    void fillFrameData(std::uint32_t slot, PushConstants& push) {
        const Vec3 jitter{halton(renderedFrame + 1U, 2U) - 0.5F, halton(renderedFrame + 1U, 3U) - 0.5F, 0.0F};
        const float tanHalfFov = std::tan(60.0F * 3.14159265359F / 360.0F);
        FrameDataCpu data{};
        const Vec3 forward = camera.forward();
        const Vec3 right = camera.right();
        const Vec3 up = camera.up();
        const Vec3 previousForward = previousCamera.forward();
        const Vec3 previousRight = previousCamera.right();
        const Vec3 previousUp = previousCamera.up();
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        data.cameraPositionTanFov = {camera.position.x, camera.position.y, camera.position.z, tanHalfFov};
        data.cameraForwardAspect = {forward.x, forward.y, forward.z, aspect};
        data.cameraRightWidth = {right.x, right.y, right.z, static_cast<float>(extent.width)};
        data.cameraUpHeight = {up.x, up.y, up.z, static_cast<float>(extent.height)};
        data.previousPositionTanFov = {previousCamera.position.x, previousCamera.position.y, previousCamera.position.z, tanHalfFov};
        data.previousForwardAspect = {previousForward.x, previousForward.y, previousForward.z, aspect};
        data.previousRightWidth = {previousRight.x, previousRight.y, previousRight.z, static_cast<float>(extent.width)};
        data.previousUpHeight = {previousUp.x, previousUp.y, previousUp.z, static_cast<float>(extent.height)};
        data.jitterAndTime = {jitter.x, jitter.y, previousJitter.x, previousJitter.y};
        data.frameInfo = {static_cast<std::uint32_t>(renderedFrame), static_cast<std::uint32_t>(scene.lights.size()), static_cast<std::uint32_t>(cameraMoved()), cameraCut ? 1U : 0U};
        std::memcpy(static_cast<std::byte*>(frameData.mapped) + frameDataStride * slot, &data, sizeof(data));
        vmaFlushAllocation(allocator, frameData.allocation, frameDataStride * slot, sizeof(data));
        push.width = extent.width;
        push.height = extent.height;
        push.frameSlot = slot;
        push.frameParity = static_cast<std::uint32_t>(renderedFrame & 1U);
        push.phase = physicalProperties.limits.maxComputeWorkGroupCount[0];
        previousJitter = jitter;
    }

    void recordDisplay(VkCommandBuffer command, std::uint32_t imageIndex, const PushConstants& push) {
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
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &toAttachment;
        vkCmdPipelineBarrier2(command, &dependency);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = swapchainViews[imageIndex];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(command, &rendering);
        bindDescriptors(command, VK_PIPELINE_BIND_POINT_GRAPHICS);
        pushConstants(command, push);
        const std::array<VkShaderStageFlagBits, 2> stages{VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        const std::array<VkShaderEXT, 2> shaders{vertexShader, fragmentShader};
        vkCmdBindShadersEXT(command, 2, stages.data(), shaders.data());
        VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0F, 1.0F};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewportWithCount(command, 1, &viewport);
        vkCmdSetScissorWithCount(command, 1, &scissor);
        vkCmdSetPrimitiveTopology(command, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkCmdSetPrimitiveRestartEnable(command, VK_FALSE);
        vkCmdSetRasterizerDiscardEnable(command, VK_FALSE);
        vkCmdSetCullMode(command, VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(command, VK_FRONT_FACE_COUNTER_CLOCKWISE);
        vkCmdSetDepthBiasEnable(command, VK_FALSE);
        vkCmdSetDepthTestEnable(command, VK_FALSE);
        vkCmdSetDepthWriteEnable(command, VK_FALSE);
        vkCmdSetDepthCompareOp(command, VK_COMPARE_OP_ALWAYS);
        vkCmdSetDepthBoundsTestEnable(command, VK_FALSE);
        vkCmdSetStencilTestEnable(command, VK_FALSE);
        vkCmdSetVertexInputEXT(command, 0, nullptr, 0, nullptr);
        vkCmdSetDepthClampEnableEXT(command, VK_FALSE);
        vkCmdSetPolygonModeEXT(command, VK_POLYGON_MODE_FILL);
        vkCmdSetRasterizationSamplesEXT(command, VK_SAMPLE_COUNT_1_BIT);
        const VkSampleMask sampleMask = 1U;
        vkCmdSetSampleMaskEXT(command, VK_SAMPLE_COUNT_1_BIT, &sampleMask);
        vkCmdSetAlphaToCoverageEnableEXT(command, VK_FALSE);
        vkCmdSetLogicOpEnableEXT(command, VK_FALSE);
        const VkBool32 blendEnable = VK_FALSE;
        vkCmdSetColorBlendEnableEXT(command, 0, 1, &blendEnable);
        VkColorBlendEquationEXT equation{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
        vkCmdSetColorBlendEquationEXT(command, 0, 1, &equation);
        const VkColorComponentFlags mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        vkCmdSetColorWriteMaskEXT(command, 0, 1, &mask);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRendering(command);

        VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.image = swapchainImages[imageIndex];
        toPresent.subresourceRange = toAttachment.subresourceRange;
        dependency.pImageMemoryBarriers = &toPresent;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    void recordFrame(VkCommandBuffer command, std::uint32_t imageIndex, PushConstants push) {
        bindDescriptors(command, VK_PIPELINE_BIND_POINT_COMPUTE);
        bindDescriptors(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
        push.queueParity = 0;
        push.bounce = 0;
        push.iteration = 0;
        pushConstants(command, push);
        bindCompute(command, ComputeStage::Primary);
        dispatchLinear(command, static_cast<std::uint64_t>(push.width) * push.height);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline);
        pushConstants(command, push);
        vkCmdTraceRaysKHR(command, &surfaceRaygenRegion, &missRegion, &hitRegion, &callableRegion, push.width, push.height, 1);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        for (std::uint32_t bounce = 0; bounce < 8; ++bounce) {
            push.bounce = bounce;
            push.queueParity = bounce & 1U;
            pushConstants(command, push);
            memoryBarrier(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdFillBuffer(command, counters.handle, sizeof(std::uint32_t), sizeof(std::uint32_t) * 2U, 0U);
            memoryBarrier(command, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            bindCompute(command, ComputeStage::Shade);
            if (bounce == 0) dispatchLinear(command, static_cast<std::uint64_t>(push.width) * push.height);
            else vkCmdDispatchIndirect(command, indirectCommands.handle, sizeof(std::uint32_t) * 4U);
            memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            bindCompute(command, ComputeStage::PrepareIndirect);
            vkCmdDispatch(command, 1, 1, 1);
            memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline);
            pushConstants(command, push);
            vkCmdTraceRaysIndirectKHR(command, &shadowRaygenRegion, &missRegion, &hitRegion, &callableRegion,
                bufferAddress(indirectCommands) + sizeof(std::uint32_t) * 8U);
            memoryBarrier(command, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            bindCompute(command, ComputeStage::ShadowResolve);
            vkCmdDispatchIndirect(command, indirectCommands.handle, sizeof(std::uint32_t) * 12U);
            memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            if (bounce + 1U < 8U) {
                push.queueParity = (bounce + 1U) & 1U;
                push.bounce = bounce + 1U;
                pushConstants(command, push);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline);
                vkCmdTraceRaysIndirectKHR(command, &surfaceRaygenRegion, &missRegion, &hitRegion, &callableRegion, bufferAddress(indirectCommands));
                memoryBarrier(command, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            }
        }

        push.queueParity = 0;
        push.bounce = 0;
        pushConstants(command, push);
        bindCompute(command, ComputeStage::RestirInitial);
        dispatchLinear(command, static_cast<std::uint64_t>(push.width) * push.height);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        bindCompute(command, ComputeStage::RestirTemporal);
        dispatchLinear(command, static_cast<std::uint64_t>(push.width) * push.height);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(command, counters.handle, sizeof(std::uint32_t) * 2U, sizeof(std::uint32_t), 0U);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        bindCompute(command, ComputeStage::RestirSpatial);
        dispatchLinear(command, static_cast<std::uint64_t>(push.width) * push.height);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        bindCompute(command, ComputeStage::PrepareIndirect);
        vkCmdDispatch(command, 1, 1, 1);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline);
        vkCmdTraceRaysIndirectKHR(command, &shadowRaygenRegion, &missRegion, &hitRegion, &callableRegion,
            bufferAddress(indirectCommands) + sizeof(std::uint32_t) * 8U);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        bindCompute(command, ComputeStage::ShadowResolve);
        vkCmdDispatchIndirect(command, indirectCommands.handle, sizeof(std::uint32_t) * 12U);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        bindCompute(command, ComputeStage::Resolve);
        dispatchLinear(command, static_cast<std::uint64_t>(push.width) * push.height);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        bindCompute(command, ComputeStage::SvgfTemporal);
        dispatchLinear(command, static_cast<std::uint64_t>(push.width) * push.height * domainCount);
        memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        for (std::uint32_t iteration = 0; iteration < 5; ++iteration) {
            push.iteration = iteration;
            pushConstants(command, push);
            bindCompute(command, ComputeStage::SvgfAtrous);
            dispatchLinear(command, static_cast<std::uint64_t>(push.width) * push.height * domainCount);
            memoryBarrier(command, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                iteration == 4 ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
        }
        recordDisplay(command, imageIndex, push);
    }

    bool renderFrame() {
        if (framebufferChanged) recreateSwapchain();
        if (minimized || swapchain == VK_NULL_HANDLE) return false;
        FrameSlot& frame = frames[currentFrameSlot];
        requireVk(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "等待 frame slot");
        std::uint32_t imageIndex = 0;
        const VkResult acquire = vkAcquireNextImageKHR(device, swapchain, std::numeric_limits<std::uint64_t>::max(), frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            framebufferChanged = true;
            recreateSwapchain();
            return false;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) requireVk(acquire, "获取交换链图像");
        if (imageOwners[imageIndex] != VK_NULL_HANDLE) requireVk(vkWaitForFences(device, 1, &imageOwners[imageIndex], VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "等待交换链图像所有者");
        imageOwners[imageIndex] = frame.fence;
        requireVk(vkResetFences(device, 1, &frame.fence), "重置 frame fence");
        requireVk(vkResetCommandPool(device, frame.pool, 0), "重置 frame command pool");

        PushConstants push{};
        fillFrameData(currentFrameSlot, push);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        requireVk(vkBeginCommandBuffer(frame.command, &begin), "开始 frame command buffer");
        recordFrame(frame.command, imageIndex, push);
        requireVk(vkEndCommandBuffer(frame.command), "结束 frame command buffer");

        std::array<VkSemaphoreSubmitInfo, 2> waits{};
        waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[0].semaphore = frame.imageAvailable;
        waits[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        std::uint32_t waitCount = 1;
        if (timelineValue > 0) {
            waits[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[1].semaphore = timeline;
            waits[1].value = timelineValue;
            waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            waitCount = 2;
        }
        ++timelineValue;
        std::array<VkSemaphoreSubmitInfo, 2> signals{};
        signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[0].semaphore = renderFinished[imageIndex];
        signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[1].semaphore = timeline;
        signals[1].value = timelineValue;
        signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandInfo.commandBuffer = frame.command;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.waitSemaphoreInfoCount = waitCount;
        submit.pWaitSemaphoreInfos = waits.data();
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commandInfo;
        submit.signalSemaphoreInfoCount = 2;
        submit.pSignalSemaphoreInfos = signals.data();
        requireVk(vkQueueSubmit2(queue, 1, &submit, frame.fence), "提交渲染 frame");
        ++renderedFrame;
        previousCamera = camera;
        cameraCut = false;

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinished[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        const VkResult result = vkQueuePresentKHR(queue, &present);
        const bool presented = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR) framebufferChanged = true;
        else requireVk(result, "呈现交换链图像");
        currentFrameSlot = (currentFrameSlot + 1U) % frameSlotCount;
        if (presented) updateStatistics();
        if (validationFailed.load(std::memory_order_relaxed)) {
            std::lock_guard lock(validationMutex);
            throw std::runtime_error("Vulkan validation 诊断：" + validationMessage);
        }
        return presented;
    }

    int run() {
        while (running) {
            processEvents();
            const auto now = std::chrono::steady_clock::now();
            float delta = static_cast<float>(std::chrono::duration<double>(now - lastLoopTime).count());
            lastLoopTime = now;
            delta = std::min(delta, 0.1F);
            updateCamera(delta);
            if (!running) break;
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);
            if (width <= 0 || height <= 0 || minimized) {
                minimized = true;
                SDL_WaitEventTimeout(nullptr, 50);
                lastLoopTime = std::chrono::steady_clock::now();
                continue;
            }
            minimized = false;
            renderFrame();
        }
        return 0;
    }
};

Application::Application(std::filesystem::path executableDirectory) {
    auto implementation = std::make_unique<Impl>(std::move(executableDirectory));
    implementation->initialize();
    implementation_ = implementation.release();
}

Application::~Application() { delete implementation_; }

int Application::run() { return implementation_->run(); }

} // 命名空间 arena
