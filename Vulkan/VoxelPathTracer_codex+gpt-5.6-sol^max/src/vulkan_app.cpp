#include "vulkan_app.hpp"

#include "gpu_shared.hpp"
#include "materials.hpp"
#include "scene.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <volk.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arena {
namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void check(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " 失败，VkResult=" + std::to_string(static_cast<int>(result)));
    }
}

VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return alignment == 0 ? value : (value + alignment - 1U) / alignment * alignment;
}

VkDeviceSize checkedProduct(VkDeviceSize a, VkDeviceSize b, std::string_view label) {
    if (a != 0U && b > std::numeric_limits<VkDeviceSize>::max() / a) {
        fail(std::string(label) + " 大小溢出");
    }
    return a * b;
}

struct Buffer {
    VkBuffer handle{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize size{};
    VkDeviceAddress address{};
    void* mapped{};
};

struct Image {
    VkImage handle{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    std::uint32_t mipCount{};
    std::uint32_t layers{};
};

struct AccelerationStructure {
    VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
    Buffer storage;
    VkDeviceAddress address{};
};

struct FrameSlot {
    VkCommandPool commandPool{VK_NULL_HANDLE};
    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
    VkSemaphore acquireSemaphore{VK_NULL_HANDLE};
    std::uint64_t completionValue{};
};

struct CameraState {
    Vec3 position{8.0F, 2.5F, 20.0F};
    float yaw{3.141592653589793F};
    float pitch{-0.03569911F};
    float fovY{radians(60.0F)};
};

struct ShaderObjects {
    VkShaderEXT prepare{VK_NULL_HANDLE};
    VkShaderEXT buildIndirect{VK_NULL_HANDLE};
    VkShaderEXT shade{VK_NULL_HANDLE};
    VkShaderEXT restirInitial{VK_NULL_HANDLE};
    VkShaderEXT restirTemporal{VK_NULL_HANDLE};
    VkShaderEXT restirSpatial{VK_NULL_HANDLE};
    VkShaderEXT restirVisibility{VK_NULL_HANDLE};
    VkShaderEXT resolveShadow{VK_NULL_HANDLE};
    VkShaderEXT svgfTemporal{VK_NULL_HANDLE};
    VkShaderEXT atrous{VK_NULL_HANDLE};
    VkShaderEXT vertex{VK_NULL_HANDLE};
    VkShaderEXT fragment{VK_NULL_HANDLE};
};

struct PixelResources {
    Buffer pathA;
    Buffer pathB;
    Buffer hits;
    Buffer counters;
    Buffer indirect;
    Buffer shadowTasks;
    Buffer shadowResults;
    Buffer branchRecords;
    Buffer reservoirInitial;
    Buffer reservoirTemporal;
    Buffer reservoirHistory;
    Buffer svgfHistory;
    Buffer atrousA;
    Buffer atrousB;
    std::uint32_t pixelCount{};
    std::uint32_t branchCapacity{};
};

class Application {
public:
    Application() = default;
    ~Application() { cleanup(); }
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    void initialize();
    void createWindow();
    void createInstance();
    void selectPhysicalDevice();
    void createDevice();
    void createCommandResources();
    void loadInputs();
    void createStaticBuffers();
    void createTextures();
    void buildAccelerationStructures();
    void createDescriptorLayout();
    void createPipelineLayout();
    void createRayTracingPipeline();
    void createShaderObjects();
    void createSampler();
    void createSwapchain();
    void createPixelResources();
    void createDescriptorBuffer();
    void writeDescriptors();
    void createTimeline();
    void recreateSwapchain();
    void destroySwapchain();
    void destroyPixelResources();
    void cleanup();

    void processEvents(bool& running, bool& cameraCut);
    bool updateCamera(float elapsedSeconds);
    bool drawFrame(bool cameraMoved, bool cameraCut);
    void recordFrame(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, std::uint32_t parity);
    void updateCameraBuffer(std::uint32_t parity, bool cameraMoved, bool cameraCut);
    void resetStatistics();
    void recordPresentedFrame();

    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags properties) const;
    [[nodiscard]] Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      VkMemoryPropertyFlags memoryProperties, bool map = false);
    void destroyBuffer(Buffer& buffer);
    [[nodiscard]] Image createImage(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                                    std::uint32_t layers, VkFormat format, VkImageUsageFlags usage);
    void destroyImage(Image& image);
    void uploadBuffer(Buffer& destination, const void* data, VkDeviceSize size,
                      VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess);
    [[nodiscard]] VkCommandBuffer beginImmediate();
    void endImmediate(VkCommandBuffer commandBuffer);
    [[nodiscard]] std::vector<std::uint32_t> readSpirv(const std::string& fileName) const;
    [[nodiscard]] VkShaderEXT createShaderObject(const std::string& fileName, VkShaderStageFlagBits stage,
                                                 VkShaderStageFlags nextStage);
    void bindCompute(VkCommandBuffer commandBuffer, VkShaderEXT shader, const PushConstantsGpu& push) const;
    void bindDescriptorBuffer(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint) const;
    void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 sourceStage,
                       VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                       VkAccessFlags2 destinationAccess) const;
    void runShadowPass(VkCommandBuffer commandBuffer, const PushConstantsGpu& basePush) const;
    [[nodiscard]] PushConstantsGpu makePush(std::uint32_t parity) const;
    [[nodiscard]] Vec3 cameraForward(const CameraState& state) const;
    [[nodiscard]] Vec3 cameraRight(const CameraState& state) const;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                         void* userData);

    SDL_Window* window_{};
    bool sdlInitialized_{};
    bool mouseCaptured_{};
    bool minimized_{};
    bool resizePending_{};

    std::filesystem::path executableDirectory_;
    MaterialSet materialSet_;
    SceneGeometry scene_;
    std::uint32_t glassMaterialIndex_{};

    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debugMessenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    std::uint32_t queueFamily_{};
    bool validationFailed_{};

    VkPhysicalDeviceProperties physicalProperties_{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties_{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties_{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorProperties_{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};

    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    Buffer triangleBuffer_;
    Buffer materialBuffer_;
    Buffer lightBuffer_;
    Buffer cameraBuffer_;
    Image baseColorImage_;
    Image roughnessImage_;
    VkSampler sampler_{VK_NULL_HANDLE};

    std::vector<AccelerationStructure> blases_;
    AccelerationStructure tlas_;
    Buffer sbtBuffer_;
    VkStridedDeviceAddressRegionKHR traceRaygenRegion_{};
    VkStridedDeviceAddressRegionKHR shadowRaygenRegion_{};
    VkStridedDeviceAddressRegionKHR missRegion_{};
    VkStridedDeviceAddressRegionKHR hitRegion_{};
    VkStridedDeviceAddressRegionKHR callableRegion_{};

    VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkPipeline rayTracingPipeline_{VK_NULL_HANDLE};
    ShaderObjects shaders_;
    Buffer descriptorBuffer_;
    VkDeviceSize descriptorLayoutSize_{};
    std::array<VkDeviceSize, bindingCount> descriptorOffsets_{};

    VkCommandPool immediateCommandPool_{VK_NULL_HANDLE};
    std::array<FrameSlot, 2> frameSlots_{};
    VkSemaphore timeline_{VK_NULL_HANDLE};
    std::uint64_t timelineValue_{};
    std::uint64_t lastHistoryValue_{};

    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkFormat swapchainFormat_{VK_FORMAT_UNDEFINED};
    VkExtent2D extent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkImageLayout> swapchainLayouts_;
    std::vector<VkSemaphore> presentSemaphores_;
    PixelResources pixel_;

    CameraState camera_;
    CameraState previousCamera_;
    Vec2 jitter_{};
    Vec2 previousJitter_{};
    std::uint64_t renderedFrame_{};
    bool historyValid_{};

    Clock::time_point lastLoopTime_{};
    Clock::time_point statisticsStart_{};
    std::uint32_t statisticsFrames_{};
};

VKAPI_ATTR VkBool32 VKAPI_CALL Application::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                           VkDebugUtilsMessageTypeFlagsEXT,
                                                           const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                           void* userData) {
    auto* application = static_cast<Application*>(userData);
    const char* label = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR"
                       : severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? "WARNING"
                                                                                     : "INFO";
    std::cerr << "Vulkan " << label << ": " << (data != nullptr && data->pMessage != nullptr ? data->pMessage : "<无消息>") << '\n';
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && application != nullptr) {
        application->validationFailed_ = true;
    }
    return VK_FALSE;
}

Vec3 Application::cameraForward(const CameraState& state) const {
    const float cosine = std::cos(state.pitch);
    return normalize({cosine * std::sin(state.yaw), std::sin(state.pitch), cosine * std::cos(state.yaw)});
}

Vec3 Application::cameraRight(const CameraState& state) const {
    return normalize(cross(cameraForward(state), Vec3{0.0F, 1.0F, 0.0F}));
}

void Application::createWindow() {
    if (!SDL_Init(SDL_INIT_VIDEO)) fail(std::string("SDL3 初始化失败: ") + SDL_GetError());
    sdlInitialized_ = true;
    window_ = SDL_CreateWindow("VoxelPathTracer | FPS: -- | -- ms/frame", 1280, 720,
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) fail(std::string("SDL3 Vulkan 窗口创建失败: ") + SDL_GetError());
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) fail(std::string("无法定位可执行文件目录: ") + SDL_GetError());
    executableDirectory_ = std::filesystem::path(basePath);
}

void Application::createInstance() {
    check(volkInitialize(), "volkInitialize");
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    check(vkEnumerateInstanceVersion(&loaderVersion), "vkEnumerateInstanceVersion");
    if (loaderVersion < VK_API_VERSION_1_4) fail("Vulkan loader 不支持 Vulkan 1.4");

    Uint32 sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (sdlExtensions == nullptr) fail(std::string("SDL3 无法查询 Vulkan instance 扩展: ") + SDL_GetError());
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);
    std::vector<const char*> layers;
#if defined(_DEBUG)
    std::uint32_t layerCount = 0;
    check(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "枚举 instance layer 数量");
    std::vector<VkLayerProperties> layerProperties(layerCount);
    check(vkEnumerateInstanceLayerProperties(&layerCount, layerProperties.data()), "枚举 instance layer");
    const bool hasValidation = std::any_of(layerProperties.begin(), layerProperties.end(), [](const VkLayerProperties& layer) {
        return std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
    });
    if (!hasValidation) fail("Debug 构建要求 VK_LAYER_KHRONOS_validation，但当前未安装");
    layers.push_back("VK_LAYER_KHRONOS_validation");
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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

#if defined(_DEBUG)
    const VkValidationFeatureEnableEXT enabledValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &enabledValidation;
    createInfo.pNext = &validationFeatures;
#endif
    check(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance(Vulkan 1.4)");
    volkLoadInstance(instance_);

#if defined(_DEBUG)
    VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugInfo.pfnUserCallback = debugCallback;
    debugInfo.pUserData = this;
    check(vkCreateDebugUtilsMessengerEXT(instance_, &debugInfo, nullptr, &debugMessenger_),
          "vkCreateDebugUtilsMessengerEXT");
#endif
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        fail(std::string("SDL_Vulkan_CreateSurface 失败: ") + SDL_GetError());
    }
}

std::set<std::string> enumerateDeviceExtensions(VkPhysicalDevice device) {
    std::uint32_t count = 0;
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr), "枚举 device extension 数量");
    std::vector<VkExtensionProperties> properties(count);
    check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data()), "枚举 device extension");
    std::set<std::string> result;
    for (const VkExtensionProperties& property : properties) result.emplace(property.extensionName);
    return result;
}

void Application::selectPhysicalDevice() {
    std::uint32_t deviceCount = 0;
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "枚举物理设备数量");
    if (deviceCount == 0) fail("没有 Vulkan 物理设备");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    check(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "枚举物理设备");

    const std::array requiredExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    };

    std::vector<std::string> rejectionReasons;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_4) {
            rejectionReasons.emplace_back("设备 API 版本低于 Vulkan 1.4");
            continue;
        }
        const std::set<std::string> availableExtensions = enumerateDeviceExtensions(candidate);
        std::vector<std::string> missingExtensions;
        for (const char* extension : requiredExtensions) {
            if (!availableExtensions.contains(extension)) missingExtensions.emplace_back(extension);
        }
        if (!missingExtensions.empty()) {
            std::ostringstream message;
            message << "缺少 device extension:";
            for (const std::string& extension : missingExtensions) message << ' ' << extension;
            rejectionReasons.push_back(message.str());
            continue;
        }

        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjectFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamicStateFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamicState2Features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3Features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInputFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &features12;
        features12.pNext = &features13;
        features13.pNext = &accelerationFeatures;
        accelerationFeatures.pNext = &rayTracingFeatures;
        rayTracingFeatures.pNext = &descriptorFeatures;
        descriptorFeatures.pNext = &shaderObjectFeatures;
        shaderObjectFeatures.pNext = &dynamicStateFeatures;
        dynamicStateFeatures.pNext = &dynamicState2Features;
        dynamicState2Features.pNext = &dynamicState3Features;
        dynamicState3Features.pNext = &vertexInputFeatures;
        vkGetPhysicalDeviceFeatures2(candidate, &features);

        std::vector<std::string> missing;
        if (features12.timelineSemaphore == VK_FALSE) missing.emplace_back("timelineSemaphore");
        if (features12.bufferDeviceAddress == VK_FALSE) missing.emplace_back("bufferDeviceAddress");
        if (features12.scalarBlockLayout == VK_FALSE) missing.emplace_back("scalarBlockLayout");
        if (features13.dynamicRendering == VK_FALSE) missing.emplace_back("dynamicRendering");
        if (features13.synchronization2 == VK_FALSE) missing.emplace_back("synchronization2");
        if (accelerationFeatures.accelerationStructure == VK_FALSE) missing.emplace_back("accelerationStructure");
        if (rayTracingFeatures.rayTracingPipeline == VK_FALSE) missing.emplace_back("rayTracingPipeline");
        if (rayTracingFeatures.rayTracingPipelineTraceRaysIndirect == VK_FALSE) missing.emplace_back("rayTracingPipelineTraceRaysIndirect");
        if (descriptorFeatures.descriptorBuffer == VK_FALSE) missing.emplace_back("descriptorBuffer");
        if (shaderObjectFeatures.shaderObject == VK_FALSE) missing.emplace_back("shaderObject");
        if (dynamicStateFeatures.extendedDynamicState == VK_FALSE) missing.emplace_back("extendedDynamicState");
        if (dynamicState2Features.extendedDynamicState2 == VK_FALSE) missing.emplace_back("extendedDynamicState2");
        if (dynamicState3Features.extendedDynamicState3PolygonMode == VK_FALSE ||
            dynamicState3Features.extendedDynamicState3RasterizationSamples == VK_FALSE ||
            dynamicState3Features.extendedDynamicState3SampleMask == VK_FALSE ||
            dynamicState3Features.extendedDynamicState3AlphaToCoverageEnable == VK_FALSE ||
            dynamicState3Features.extendedDynamicState3ColorBlendEnable == VK_FALSE ||
            dynamicState3Features.extendedDynamicState3ColorBlendEquation == VK_FALSE ||
            dynamicState3Features.extendedDynamicState3ColorWriteMask == VK_FALSE) {
            missing.emplace_back("shader object 所需 extendedDynamicState3 子特性");
        }
        if (vertexInputFeatures.vertexInputDynamicState == VK_FALSE) missing.emplace_back("vertexInputDynamicState");
        if (!missing.empty()) {
            std::ostringstream message;
            message << "缺少 feature:";
            for (const std::string& feature : missing) message << ' ' << feature;
            rejectionReasons.push_back(message.str());
            continue;
        }

        std::uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
        std::optional<std::uint32_t> compatibleQueue;
        for (std::uint32_t index = 0; index < queueCount; ++index) {
            VkBool32 present = VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface_, &present), "查询 present 支持");
            const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((queues[index].queueFlags & required) == required && present == VK_TRUE) {
                compatibleQueue = index;
                break;
            }
        }
        if (!compatibleQueue.has_value()) {
            rejectionReasons.emplace_back("没有同时支持 graphics/compute/present 的队列族");
            continue;
        }

        VkFormatProperties colorProperties{};
        VkFormatProperties roughnessProperties{};
        vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R8G8B8A8_SRGB, &colorProperties);
        vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R8_UNORM, &roughnessProperties);
        if ((colorProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U ||
            (roughnessProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U) {
            rejectionReasons.emplace_back("程序化材质纹理格式不支持 sampled image");
            continue;
        }

        physicalDevice_ = candidate;
        queueFamily_ = *compatibleQueue;
        physicalProperties_ = properties;
        break;
    }
    if (physicalDevice_ == VK_NULL_HANDLE) {
        std::ostringstream message;
        message << "没有满足 Vulkan 1.4/RT/Descriptor Buffer/Shader Object 合同的设备";
        for (const std::string& reason : rejectionReasons) message << "\n- " << reason;
        fail(message.str());
    }

    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &rayTracingProperties_;
    rayTracingProperties_.pNext = &accelerationProperties_;
    accelerationProperties_.pNext = &descriptorProperties_;
    vkGetPhysicalDeviceProperties2(physicalDevice_, &properties2);
}

void Application::createDevice() {
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.timelineSemaphore = VK_TRUE;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.scalarBlockLayout = VK_TRUE;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    accelerationFeatures.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    rayTracingFeatures.rayTracingPipeline = VK_TRUE;
    rayTracingFeatures.rayTracingPipelineTraceRaysIndirect = VK_TRUE;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    descriptorFeatures.descriptorBuffer = VK_TRUE;
    VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjectFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    shaderObjectFeatures.shaderObject = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamicStateFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    dynamicStateFeatures.extendedDynamicState = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamicState2Features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    dynamicState2Features.extendedDynamicState2 = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3Features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    dynamicState3Features.extendedDynamicState3PolygonMode = VK_TRUE;
    dynamicState3Features.extendedDynamicState3RasterizationSamples = VK_TRUE;
    dynamicState3Features.extendedDynamicState3SampleMask = VK_TRUE;
    dynamicState3Features.extendedDynamicState3AlphaToCoverageEnable = VK_TRUE;
    dynamicState3Features.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    dynamicState3Features.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    dynamicState3Features.extendedDynamicState3ColorWriteMask = VK_TRUE;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInputFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    vertexInputFeatures.vertexInputDynamicState = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features12;
    features12.pNext = &features13;
    features13.pNext = &accelerationFeatures;
    accelerationFeatures.pNext = &rayTracingFeatures;
    rayTracingFeatures.pNext = &descriptorFeatures;
    descriptorFeatures.pNext = &shaderObjectFeatures;
    shaderObjectFeatures.pNext = &dynamicStateFeatures;
    dynamicStateFeatures.pNext = &dynamicState2Features;
    dynamicState2Features.pNext = &dynamicState3Features;
    dynamicState3Features.pNext = &vertexInputFeatures;

    const std::array extensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    };
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &features;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    check(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
}

void Application::createCommandResources() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    check(vkCreateCommandPool(device_, &poolInfo, nullptr, &immediateCommandPool_), "创建初始化 command pool");
    for (FrameSlot& slot : frameSlots_) {
        check(vkCreateCommandPool(device_, &poolInfo, nullptr, &slot.commandPool), "创建 frame command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = slot.commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device_, &allocation, &slot.commandBuffer), "分配 frame command buffer");
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &slot.acquireSemaphore), "创建 acquire semaphore");
    }
}

void Application::createTimeline() {
    VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &typeInfo;
    check(vkCreateSemaphore(device_, &createInfo, nullptr, &timeline_), "创建 timeline semaphore");
}

void Application::loadInputs() {
    const std::filesystem::path materialPath = executableDirectory_ / "materials.json";
    materialSet_ = loadMaterials(materialPath);
    scene_ = buildScene(materialSet_);
    const auto glass = materialSet_.indexById.find("glass");
    if (glass == materialSet_.indexById.end()) fail("场景材质 glass 无法解析");
    glassMaterialIndex_ = glass->second;
}

std::uint32_t Application::findMemoryType(std::uint32_t bits, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties);
    for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0U &&
            (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
            return index;
        }
    }
    fail("找不到满足资源属性的 Vulkan memory type");
}

Buffer Application::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags memoryProperties, bool map) {
    if (size == 0U) fail("尝试创建零大小 buffer");
    Buffer buffer{};
    buffer.size = size;
    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    createInfo.size = size;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &createInfo, nullptr, &buffer.handle), "vkCreateBuffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer.handle, &requirements);
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0U) {
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = flagsInfo.flags != 0U ? &flagsInfo : nullptr;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, memoryProperties);
    check(vkAllocateMemory(device_, &allocation, nullptr, &buffer.memory), "vkAllocateMemory(buffer)");
    check(vkBindBufferMemory(device_, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0U) {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = buffer.handle;
        buffer.address = vkGetBufferDeviceAddress(device_, &addressInfo);
        if (buffer.address == 0U) fail("buffer device address 为零");
    }
    if (map) check(vkMapMemory(device_, buffer.memory, 0, size, 0, &buffer.mapped), "vkMapMemory");
    return buffer;
}

void Application::destroyBuffer(Buffer& buffer) {
    if (device_ == VK_NULL_HANDLE) return;
    if (buffer.mapped != nullptr) vkUnmapMemory(device_, buffer.memory);
    if (buffer.handle != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer.handle, nullptr);
    if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = {};
}

Image Application::createImage(std::uint32_t width, std::uint32_t height, std::uint32_t mips,
                               std::uint32_t layers, VkFormat format, VkImageUsageFlags usage) {
    Image image{};
    image.format = format;
    image.mipCount = mips;
    image.layers = layers;
    VkImageCreateInfo createInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.format = format;
    createInfo.extent = {width, height, 1U};
    createInfo.mipLevels = mips;
    createInfo.arrayLayers = layers;
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_, &createInfo, nullptr, &image.handle), "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image.handle, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_, &allocation, nullptr, &image.memory), "vkAllocateMemory(image)");
    check(vkBindImageMemory(device_, image.handle, image.memory, 0), "vkBindImageMemory");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image.handle;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mips;
    viewInfo.subresourceRange.layerCount = layers;
    check(vkCreateImageView(device_, &viewInfo, nullptr, &image.view), "vkCreateImageView");
    return image;
}

void Application::destroyImage(Image& image) {
    if (device_ == VK_NULL_HANDLE) return;
    if (image.view != VK_NULL_HANDLE) vkDestroyImageView(device_, image.view, nullptr);
    if (image.handle != VK_NULL_HANDLE) vkDestroyImage(device_, image.handle, nullptr);
    if (image.memory != VK_NULL_HANDLE) vkFreeMemory(device_, image.memory, nullptr);
    image = {};
}

VkCommandBuffer Application::beginImmediate() {
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = immediateCommandPool_;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_, &allocation, &commandBuffer), "分配 immediate command buffer");
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(immediate)");
    return commandBuffer;
}

void Application::endImmediate(VkCommandBuffer commandBuffer) {
    check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(immediate)");
    VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandInfo.commandBuffer = commandBuffer;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;
    check(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2(immediate)");
    check(vkQueueWaitIdle(queue_), "初始化阶段 vkQueueWaitIdle");
    vkFreeCommandBuffers(device_, immediateCommandPool_, 1, &commandBuffer);
}

void Application::uploadBuffer(Buffer& destination, const void* data, VkDeviceSize size,
                               VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
    if (size > destination.size) fail("上传数据大于目标 buffer");
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    std::memcpy(staging.mapped, data, static_cast<std::size_t>(size));
    VkCommandBuffer commandBuffer = beginImmediate();
    VkBufferMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    hostBarrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
    hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    hostBarrier.buffer = staging.handle;
    hostBarrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo hostDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    hostDependency.bufferMemoryBarrierCount = 1;
    hostDependency.pBufferMemoryBarriers = &hostBarrier;
    vkCmdPipelineBarrier2(commandBuffer, &hostDependency);
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(commandBuffer, staging.handle, destination.handle, 1, &copy);
    VkBufferMemoryBarrier2 uploadBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    uploadBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    uploadBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    uploadBarrier.dstStageMask = destinationStage;
    uploadBarrier.dstAccessMask = destinationAccess;
    uploadBarrier.buffer = destination.handle;
    uploadBarrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo uploadDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    uploadDependency.bufferMemoryBarrierCount = 1;
    uploadDependency.pBufferMemoryBarriers = &uploadBarrier;
    vkCmdPipelineBarrier2(commandBuffer, &uploadDependency);
    endImmediate(commandBuffer);
    destroyBuffer(staging);
}

void Application::createStaticBuffers() {
    constexpr VkBufferUsageFlags staticUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    vertexBuffer_ = createBuffer(checkedProduct(scene_.vertices.size(), sizeof(VertexGpu), "vertex buffer"),
                                 staticUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    indexBuffer_ = createBuffer(checkedProduct(scene_.indices.size(), sizeof(std::uint32_t), "index buffer"),
                                staticUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    triangleBuffer_ = createBuffer(checkedProduct(scene_.triangles.size(), sizeof(TriangleGpu), "triangle buffer"),
                                   staticUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    materialBuffer_ = createBuffer(checkedProduct(materialSet_.gpuMaterials.size(), sizeof(MaterialGpu), "material buffer"),
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    lightBuffer_ = createBuffer(checkedProduct(scene_.lights.size(), sizeof(LightGpu), "light buffer"),
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkPipelineStageFlags2 consumers = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                             VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    uploadBuffer(vertexBuffer_, scene_.vertices.data(), vertexBuffer_.size, consumers, access);
    uploadBuffer(indexBuffer_, scene_.indices.data(), indexBuffer_.size, consumers, access);
    uploadBuffer(triangleBuffer_, scene_.triangles.data(), triangleBuffer_.size, consumers, access);
    uploadBuffer(materialBuffer_, materialSet_.gpuMaterials.data(), materialBuffer_.size,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    uploadBuffer(lightBuffer_, scene_.lights.data(), lightBuffer_.size,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    cameraBuffer_ = createBuffer(sizeof(CameraGpu) * 2U,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
}

void Application::createTextures() {
    const TextureMipChain& textures = materialSet_.textures;
    baseColorImage_ = createImage(textures.baseSize, textures.baseSize, textures.mipCount, textures.layerCount,
                                  VK_FORMAT_R8G8B8A8_SRGB,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    roughnessImage_ = createImage(textures.baseSize, textures.baseSize, textures.mipCount, textures.layerCount,
                                  VK_FORMAT_R8_UNORM,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    const auto uploadImage = [this, &textures](Image& image,
                                               const std::vector<std::vector<std::uint8_t>>& levels) {
        VkDeviceSize totalSize = 0;
        for (const auto& level : levels) totalSize += static_cast<VkDeviceSize>(level.size());
        Buffer staging = createBuffer(totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        auto* destination = static_cast<std::byte*>(staging.mapped);
        VkDeviceSize offset = 0;
        std::vector<VkBufferImageCopy> regions;
        regions.reserve(levels.size());
        std::uint32_t size = textures.baseSize;
        for (std::uint32_t level = 0; level < levels.size(); ++level) {
            std::memcpy(destination + offset, levels[level].data(), levels[level].size());
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = textures.layerCount;
            region.imageExtent = {size, size, 1U};
            regions.push_back(region);
            offset += static_cast<VkDeviceSize>(levels[level].size());
            size = std::max(1U, size / 2U);
        }

        VkCommandBuffer commandBuffer = beginImmediate();
        VkBufferMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostBarrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        hostBarrier.buffer = staging.handle;
        hostBarrier.size = VK_WHOLE_SIZE;
        VkImageMemoryBarrier2 toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.image = image.handle;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = image.mipCount;
        toTransfer.subresourceRange.layerCount = image.layers;
        VkDependencyInfo uploadStart{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        uploadStart.bufferMemoryBarrierCount = 1;
        uploadStart.pBufferMemoryBarriers = &hostBarrier;
        uploadStart.imageMemoryBarrierCount = 1;
        uploadStart.pImageMemoryBarriers = &toTransfer;
        vkCmdPipelineBarrier2(commandBuffer, &uploadStart);
        vkCmdCopyBufferToImage(commandBuffer, staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
        VkImageMemoryBarrier2 toSampling{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toSampling.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toSampling.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toSampling.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toSampling.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toSampling.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSampling.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSampling.image = image.handle;
        toSampling.subresourceRange = toTransfer.subresourceRange;
        VkDependencyInfo uploadEnd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        uploadEnd.imageMemoryBarrierCount = 1;
        uploadEnd.pImageMemoryBarriers = &toSampling;
        vkCmdPipelineBarrier2(commandBuffer, &uploadEnd);
        endImmediate(commandBuffer);
        destroyBuffer(staging);
    };
    uploadImage(baseColorImage_, textures.baseColorRgba);
    uploadImage(roughnessImage_, textures.roughness);
}

void Application::createSampler() {
    VkSamplerCreateInfo createInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    createInfo.magFilter = VK_FILTER_LINEAR;
    createInfo.minFilter = VK_FILTER_LINEAR;
    createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    createInfo.minLod = 0.0F;
    createInfo.maxLod = static_cast<float>(materialSet_.textures.mipCount - 1U);
    createInfo.maxAnisotropy = 1.0F;
    check(vkCreateSampler(device_, &createInfo, nullptr, &sampler_), "vkCreateSampler");
}

void Application::buildAccelerationStructures() {
    const auto createAcceleration = [this](VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
        AccelerationStructure result{};
        result.storage = createBuffer(size,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkAccelerationStructureCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.buffer = result.storage.handle;
        createInfo.size = size;
        createInfo.type = type;
        check(vkCreateAccelerationStructureKHR(device_, &createInfo, nullptr, &result.handle),
              "vkCreateAccelerationStructureKHR");
        return result;
    };
    const auto accelerationAddress = [this](VkAccelerationStructureKHR handle) {
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addressInfo.accelerationStructure = handle;
        return vkGetAccelerationStructureDeviceAddressKHR(device_, &addressInfo);
    };
    const auto destroyAcceleration = [this](AccelerationStructure& structure) {
        if (structure.handle != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(device_, structure.handle, nullptr);
        destroyBuffer(structure.storage);
        structure = {};
    };

    blases_.reserve(scene_.groups.size());
    for (const MeshGroup& group : scene_.groups) {
        VkAccelerationStructureGeometryTrianglesDataKHR trianglesData{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        trianglesData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        trianglesData.vertexData.deviceAddress = vertexBuffer_.address;
        trianglesData.vertexStride = sizeof(VertexGpu);
        trianglesData.maxVertex = static_cast<std::uint32_t>(scene_.vertices.size() - 1U);
        trianglesData.indexType = VK_INDEX_TYPE_UINT32;
        trianglesData.indexData.deviceAddress = indexBuffer_.address +
                                                static_cast<VkDeviceSize>(group.firstIndex) * sizeof(std::uint32_t);
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.geometry.triangles = trianglesData;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                          VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;
        const std::uint32_t primitiveCount = group.triangleCount;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &buildInfo, &primitiveCount, &sizes);
        AccelerationStructure uncompacted = createAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                               sizes.accelerationStructureSize);
        buildInfo.dstAccelerationStructure = uncompacted.handle;
        Buffer scratch = createBuffer(sizes.buildScratchSize + accelerationProperties_.minAccelerationStructureScratchOffsetAlignment,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        buildInfo.scratchData.deviceAddress = alignUp(scratch.address,
            accelerationProperties_.minAccelerationStructureScratchOffsetAlignment);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = primitiveCount;
        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &range;
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        queryInfo.queryCount = 1;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        check(vkCreateQueryPool(device_, &queryInfo, nullptr, &queryPool), "创建 BLAS compact query pool");
        VkCommandBuffer buildCommand = beginImmediate();
        vkCmdResetQueryPool(buildCommand, queryPool, 0, 1);
        vkCmdBuildAccelerationStructuresKHR(buildCommand, 1, &buildInfo, &rangePointer);
        VkMemoryBarrier2 buildBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        buildBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        buildBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        buildBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        buildBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo buildDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        buildDependency.memoryBarrierCount = 1;
        buildDependency.pMemoryBarriers = &buildBarrier;
        vkCmdPipelineBarrier2(buildCommand, &buildDependency);
        vkCmdWriteAccelerationStructuresPropertiesKHR(buildCommand, 1, &uncompacted.handle,
                                                       VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                       queryPool, 0);
        endImmediate(buildCommand);

        VkDeviceSize compactedSize = 0;
        check(vkGetQueryPoolResults(device_, queryPool, 0, 1, sizeof(compactedSize), &compactedSize,
                                    sizeof(compactedSize), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "读取 BLAS compact size");
        if (compactedSize == 0U || compactedSize > sizes.accelerationStructureSize) {
            fail("驱动返回无效的 BLAS compact size");
        }
        AccelerationStructure compacted = createAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                              compactedSize);
        VkCopyAccelerationStructureInfoKHR copyInfo{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        copyInfo.src = uncompacted.handle;
        copyInfo.dst = compacted.handle;
        copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        VkCommandBuffer copyCommand = beginImmediate();
        vkCmdCopyAccelerationStructureKHR(copyCommand, &copyInfo);
        VkMemoryBarrier2 copyBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        copyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        copyBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        copyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                   VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        copyBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo copyDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        copyDependency.memoryBarrierCount = 1;
        copyDependency.pMemoryBarriers = &copyBarrier;
        vkCmdPipelineBarrier2(copyCommand, &copyDependency);
        endImmediate(copyCommand);
        compacted.address = accelerationAddress(compacted.handle);
        if (compacted.address == 0U) fail("压缩 BLAS device address 为零");
        destroyAcceleration(uncompacted);
        destroyBuffer(scratch);
        vkDestroyQueryPool(device_, queryPool, nullptr);
        blases_.push_back(compacted);
    }

    std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.reserve(scene_.groups.size());
    for (std::size_t index = 0; index < scene_.groups.size(); ++index) {
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform.matrix[0][0] = 1.0F;
        instance.transform.matrix[1][1] = 1.0F;
        instance.transform.matrix[2][2] = 1.0F;
        if (scene_.groups[index].triangleBase >= (1U << 24U)) fail("TLAS instanceCustomIndex 超出 24 bit");
        instance.instanceCustomIndex = scene_.groups[index].triangleBase;
        instance.mask = 0xffU;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = blases_[index].address;
        instances.push_back(instance);
    }
    Buffer instanceBuffer = createBuffer(checkedProduct(instances.size(), sizeof(VkAccelerationStructureInstanceKHR),
                                                        "TLAS instance buffer"),
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadBuffer(instanceBuffer, instances.data(), instanceBuffer.size,
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VkAccelerationStructureGeometryInstancesDataKHR instancesData{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = instanceBuffer.address;
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instancesData;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    const std::uint32_t primitiveCount = static_cast<std::uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &buildInfo, &primitiveCount, &sizes);
    tlas_ = createAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize);
    Buffer scratch = createBuffer(sizes.buildScratchSize + accelerationProperties_.minAccelerationStructureScratchOffsetAlignment,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    buildInfo.dstAccelerationStructure = tlas_.handle;
    buildInfo.scratchData.deviceAddress = alignUp(scratch.address,
        accelerationProperties_.minAccelerationStructureScratchOffsetAlignment);
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &range;
    VkCommandBuffer commandBuffer = beginImmediate();
    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &buildInfo, &rangePointer);
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
    endImmediate(commandBuffer);
    tlas_.address = accelerationAddress(tlas_.handle);
    if (tlas_.address == 0U) fail("TLAS device address 为零");
    destroyBuffer(scratch);
    destroyBuffer(instanceBuffer);
}

void Application::createDescriptorLayout() {
    std::array<VkDescriptorSetLayoutBinding, bindingCount> bindings{};
    for (std::uint32_t index = 0; index < bindings.size(); ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorCount = 1;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].stageFlags = VK_SHADER_STAGE_ALL;
    }
    bindings[bindingTlas].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[bindingBaseColorTexture].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[bindingRoughnessTexture].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[bindingTextureSampler].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    VkDescriptorSetLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    createInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    createInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    createInfo.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device_, &createInfo, nullptr, &descriptorSetLayout_),
          "创建 Descriptor Buffer layout");
    vkGetDescriptorSetLayoutSizeEXT(device_, descriptorSetLayout_, &descriptorLayoutSize_);
    for (std::uint32_t binding = 0; binding < bindingCount; ++binding) {
        vkGetDescriptorSetLayoutBindingOffsetEXT(device_, descriptorSetLayout_, binding,
                                                 &descriptorOffsets_[binding]);
    }
}

void Application::createPipelineLayout() {
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_ALL;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstantsGpu);
    VkPipelineLayoutCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    createInfo.setLayoutCount = 1;
    createInfo.pSetLayouts = &descriptorSetLayout_;
    createInfo.pushConstantRangeCount = 1;
    createInfo.pPushConstantRanges = &pushRange;
    check(vkCreatePipelineLayout(device_, &createInfo, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");
}

std::vector<std::uint32_t> Application::readSpirv(const std::string& fileName) const {
    const std::filesystem::path path = executableDirectory_ / "shaders" / (fileName + ".spv");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) fail("无法打开必需 shader: " + path.string());
    const std::streamoff end = input.tellg();
    if (end <= 0 || end % 4 != 0) fail("SPIR-V 文件大小无效: " + path.string());
    std::vector<std::uint32_t> words(static_cast<std::size_t>(end) / sizeof(std::uint32_t));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(words.data()), end);
    if (!input) fail("读取 SPIR-V 失败: " + path.string());
    return words;
}

void Application::createRayTracingPipeline() {
    const std::array fileNames{"trace.rgen", "shadow.rgen", "trace.rchit", "trace.rmiss", "shadow.rmiss"};
    const std::array stageTypes{VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                VK_SHADER_STAGE_MISS_BIT_KHR};
    std::array<VkShaderModule, 5> modules{};
    std::array<VkPipelineShaderStageCreateInfo, 5> stages{};
    for (std::size_t index = 0; index < modules.size(); ++index) {
        const std::vector<std::uint32_t> code = readSpirv(fileNames[index]);
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = code.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = code.data();
        check(vkCreateShaderModule(device_, &moduleInfo, nullptr, &modules[index]), "创建 RT shader module");
        stages[index] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[index].stage = stageTypes[index];
        stages[index].module = modules[index];
        stages[index].pName = "main";
    }
    std::array<VkRayTracingShaderGroupCreateInfoKHR, 5> groups{};
    for (VkRayTracingShaderGroupCreateInfoKHR& group : groups) {
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 2;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = 3;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[4].generalShader = 4;
    VkRayTracingPipelineCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    createInfo.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    createInfo.stageCount = static_cast<std::uint32_t>(stages.size());
    createInfo.pStages = stages.data();
    createInfo.groupCount = static_cast<std::uint32_t>(groups.size());
    createInfo.pGroups = groups.data();
    createInfo.maxPipelineRayRecursionDepth = 1;
    createInfo.layout = pipelineLayout_;
    check(vkCreateRayTracingPipelinesKHR(device_, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &createInfo, nullptr,
                                         &rayTracingPipeline_),
          "创建 recursionDepth=1 RT pipeline");
    for (VkShaderModule module : modules) vkDestroyShaderModule(device_, module, nullptr);

    const VkDeviceSize handleSize = rayTracingProperties_.shaderGroupHandleSize;
    const VkDeviceSize handleAlignment = rayTracingProperties_.shaderGroupHandleAlignment;
    const VkDeviceSize baseAlignment = rayTracingProperties_.shaderGroupBaseAlignment;
    const VkDeviceSize generalStride = alignUp(handleSize, handleAlignment);
    const VkDeviceSize raygenStride = alignUp(handleSize, baseAlignment);
    const VkDeviceSize raygenOffset = 0;
    const VkDeviceSize missOffset = alignUp(raygenStride * 2U, baseAlignment);
    const VkDeviceSize hitOffset = alignUp(missOffset + generalStride * 2U, baseAlignment);
    const VkDeviceSize sbtSize = hitOffset + generalStride;
    sbtBuffer_ = createBuffer(sbtSize,
                              VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    std::vector<std::byte> handles(static_cast<std::size_t>(handleSize * groups.size()));
    check(vkGetRayTracingShaderGroupHandlesKHR(device_, rayTracingPipeline_, 0,
                                               static_cast<std::uint32_t>(groups.size()), handles.size(),
                                               handles.data()),
          "读取 RT shader group handles");
    auto* sbt = static_cast<std::byte*>(sbtBuffer_.mapped);
    const auto copyHandle = [&](std::uint32_t group, VkDeviceSize destinationOffset) {
        std::memcpy(sbt + destinationOffset, handles.data() + static_cast<std::size_t>(group * handleSize),
                    static_cast<std::size_t>(handleSize));
    };
    copyHandle(0, raygenOffset);
    copyHandle(1, raygenOffset + raygenStride);
    copyHandle(3, missOffset);
    copyHandle(4, missOffset + generalStride);
    copyHandle(2, hitOffset);
    traceRaygenRegion_ = {sbtBuffer_.address + raygenOffset, raygenStride, raygenStride};
    shadowRaygenRegion_ = {sbtBuffer_.address + raygenOffset + raygenStride, raygenStride, raygenStride};
    missRegion_ = {sbtBuffer_.address + missOffset, generalStride, generalStride * 2U};
    hitRegion_ = {sbtBuffer_.address + hitOffset, generalStride, generalStride};
    callableRegion_ = {};
}

VkShaderEXT Application::createShaderObject(const std::string& fileName, VkShaderStageFlagBits stage,
                                            VkShaderStageFlags nextStage) {
    const std::vector<std::uint32_t> code = readSpirv(fileName);
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_ALL;
    pushRange.size = sizeof(PushConstantsGpu);
    VkShaderCreateInfoEXT createInfo{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    createInfo.stage = stage;
    createInfo.nextStage = nextStage;
    createInfo.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    createInfo.codeSize = code.size() * sizeof(std::uint32_t);
    createInfo.pCode = code.data();
    createInfo.pName = "main";
    createInfo.setLayoutCount = 1;
    createInfo.pSetLayouts = &descriptorSetLayout_;
    createInfo.pushConstantRangeCount = 1;
    createInfo.pPushConstantRanges = &pushRange;
    VkShaderEXT shader = VK_NULL_HANDLE;
    check(vkCreateShadersEXT(device_, 1, &createInfo, nullptr, &shader), "vkCreateShadersEXT(" + fileName + ")");
    return shader;
}

void Application::createShaderObjects() {
    shaders_.prepare = createShaderObject("prepare.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.buildIndirect = createShaderObject("build_indirect.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.shade = createShaderObject("shade.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.restirInitial = createShaderObject("restir_initial.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.restirTemporal = createShaderObject("restir_temporal.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.restirSpatial = createShaderObject("restir_spatial.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.restirVisibility = createShaderObject("restir_visibility.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.resolveShadow = createShaderObject("resolve_shadow.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.svgfTemporal = createShaderObject("svgf_temporal.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.atrous = createShaderObject("atrous.comp", VK_SHADER_STAGE_COMPUTE_BIT, 0);
    shaders_.vertex = createShaderObject("fullscreen.vert", VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    shaders_.fragment = createShaderObject("display.frag", VK_SHADER_STAGE_FRAGMENT_BIT, 0);
}

void Application::createSwapchain() {
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities),
          "查询 surface capabilities");
    std::uint32_t formatCount = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr),
          "查询 surface format 数量");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data()),
          "查询 surface formats");
    const auto format = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& candidate) {
        const bool srgbFormat = candidate.format == VK_FORMAT_B8G8R8A8_SRGB ||
                                candidate.format == VK_FORMAT_R8G8B8A8_SRGB;
        return srgbFormat && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (format == formats.end()) fail("surface 不提供必需的 sRGB swapchain format");
    swapchainFormat_ = format->format;

    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        extent_ = capabilities.currentExtent;
    } else {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
            fail(std::string("SDL_GetWindowSizeInPixels 失败: ") + SDL_GetError());
        }
        extent_.width = std::clamp(static_cast<std::uint32_t>(std::max(width, 1)),
                                   capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<std::uint32_t>(std::max(height, 1)),
                                    capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent_.width == 0U || extent_.height == 0U) fail("非最小化窗口返回零 framebuffer extent");
    const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(extent_.width) * extent_.height;
    if (pixelCount64 > std::numeric_limits<std::uint32_t>::max()) fail("framebuffer 像素数超过 uint32 队列索引");
    if (pixelCount64 * 2U > rayTracingProperties_.maxRayDispatchInvocationCount) {
        fail("framebuffer 对应的双分支 RT dispatch 超过 maxRayDispatchInvocationCount");
    }

    std::uint32_t presentModeCount = 0;
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr),
          "查询 present mode 数量");
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount,
                                                     presentModes.data()),
          "查询 present modes");
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.end()) {
        presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    std::uint32_t imageCount = capabilities.minImageCount + 1U;
    if (capabilities.maxImageCount != 0U) imageCount = std::min(imageCount, capabilities.maxImageCount);
    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = swapchainFormat_;
    createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    createInfo.imageExtent = extent_;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr), "查询 swapchain image 数量");
    swapchainImages_.resize(imageCount);
    check(vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data()),
          "查询 swapchain images");
    swapchainViews_.resize(imageCount);
    swapchainLayouts_.assign(imageCount, VK_IMAGE_LAYOUT_UNDEFINED);
    presentSemaphores_.resize(imageCount);
    for (std::uint32_t index = 0; index < imageCount; ++index) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = swapchainImages_[index];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        check(vkCreateImageView(device_, &viewInfo, nullptr, &swapchainViews_[index]),
              "创建 swapchain image view");
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &presentSemaphores_[index]),
              "创建 swapchain-image present semaphore");
    }
}

void Application::createPixelResources() {
    pixel_.pixelCount = extent_.width * extent_.height;
    pixel_.branchCapacity = pixel_.pixelCount * 2U;
    constexpr VkBufferUsageFlags storageUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const auto deviceBuffer = [this](VkDeviceSize size, VkBufferUsageFlags extra = 0U) {
        return createBuffer(size, storageUsage | extra, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    };
    pixel_.pathA = deviceBuffer(checkedProduct(pixel_.branchCapacity, sizeof(PathStateGpu), "path queue A"));
    pixel_.pathB = deviceBuffer(checkedProduct(pixel_.branchCapacity, sizeof(PathStateGpu), "path queue B"));
    pixel_.hits = deviceBuffer(checkedProduct(pixel_.branchCapacity, sizeof(HitStateGpu), "hit states"));
    pixel_.counters = deviceBuffer(16U, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    pixel_.indirect = deviceBuffer(64U, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    pixel_.shadowTasks = deviceBuffer(checkedProduct(pixel_.branchCapacity, sizeof(ShadowTaskGpu), "shadow tasks"));
    pixel_.shadowResults = deviceBuffer(checkedProduct(pixel_.branchCapacity, sizeof(std::uint32_t), "shadow results"));
    pixel_.branchRecords = deviceBuffer(checkedProduct(checkedProduct(pixel_.branchCapacity, 2U, "branch parity"),
                                                       sizeof(BranchRecordGpu), "branch records"));
    pixel_.reservoirInitial = deviceBuffer(checkedProduct(pixel_.pixelCount, sizeof(ReservoirGpu), "initial reservoirs"));
    pixel_.reservoirTemporal = deviceBuffer(checkedProduct(pixel_.pixelCount, sizeof(ReservoirGpu), "temporal reservoirs"));
    pixel_.reservoirHistory = deviceBuffer(checkedProduct(checkedProduct(pixel_.pixelCount, 2U, "reservoir parity"),
                                                          sizeof(ReservoirGpu), "reservoir history"));
    const VkDeviceSize svgfElements = checkedProduct(checkedProduct(pixel_.pixelCount, kSvgfDomainCount, "SVGF domains"),
                                                     2U, "SVGF parity");
    pixel_.svgfHistory = deviceBuffer(checkedProduct(svgfElements, sizeof(SvgfHistoryGpu), "SVGF history"));
    const VkDeviceSize atrousElements = checkedProduct(pixel_.pixelCount, kSvgfDomainCount, "atrous domains");
    pixel_.atrousA = deviceBuffer(checkedProduct(atrousElements, sizeof(Vec4), "atrous A"));
    pixel_.atrousB = deviceBuffer(checkedProduct(atrousElements, sizeof(Vec4), "atrous B"));
}

void Application::destroyPixelResources() {
    destroyBuffer(pixel_.pathA);
    destroyBuffer(pixel_.pathB);
    destroyBuffer(pixel_.hits);
    destroyBuffer(pixel_.counters);
    destroyBuffer(pixel_.indirect);
    destroyBuffer(pixel_.shadowTasks);
    destroyBuffer(pixel_.shadowResults);
    destroyBuffer(pixel_.branchRecords);
    destroyBuffer(pixel_.reservoirInitial);
    destroyBuffer(pixel_.reservoirTemporal);
    destroyBuffer(pixel_.reservoirHistory);
    destroyBuffer(pixel_.svgfHistory);
    destroyBuffer(pixel_.atrousA);
    destroyBuffer(pixel_.atrousB);
    pixel_ = {};
}

void Application::createDescriptorBuffer() {
    if (descriptorLayoutSize_ > descriptorProperties_.maxResourceDescriptorBufferRange ||
        descriptorLayoutSize_ > descriptorProperties_.maxSamplerDescriptorBufferRange) {
        fail("descriptor layout 超过设备 descriptor buffer range");
    }
    const VkDeviceSize size = alignUp(descriptorLayoutSize_, descriptorProperties_.descriptorBufferOffsetAlignment);
    descriptorBuffer_ = createBuffer(size,
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
    writeDescriptors();
}

void Application::writeDescriptors() {
    auto* bytes = static_cast<std::byte*>(descriptorBuffer_.mapped);
    const auto descriptorSize = [this](VkDescriptorType type) -> std::size_t {
        switch (type) {
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return descriptorProperties_.accelerationStructureDescriptorSize;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return descriptorProperties_.sampledImageDescriptorSize;
        case VK_DESCRIPTOR_TYPE_SAMPLER: return descriptorProperties_.samplerDescriptorSize;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return descriptorProperties_.storageBufferDescriptorSize;
        default: fail("未处理的 descriptor type");
        }
    };
    const auto writeBufferDescriptor = [this, bytes, &descriptorSize](std::uint32_t binding, const Buffer& buffer) {
        VkDescriptorAddressInfoEXT address{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        address.address = buffer.address;
        address.range = buffer.size;
        address.format = VK_FORMAT_UNDEFINED;
        VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        info.data.pStorageBuffer = &address;
        vkGetDescriptorEXT(device_, &info, descriptorSize(info.type), bytes + descriptorOffsets_[binding]);
    };
    VkDescriptorGetInfoEXT accelerationInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    accelerationInfo.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    accelerationInfo.data.accelerationStructure = tlas_.address;
    vkGetDescriptorEXT(device_, &accelerationInfo, descriptorSize(accelerationInfo.type),
                       bytes + descriptorOffsets_[bindingTlas]);
    writeBufferDescriptor(bindingVertices, vertexBuffer_);
    writeBufferDescriptor(bindingIndices, indexBuffer_);
    writeBufferDescriptor(bindingTriangles, triangleBuffer_);
    writeBufferDescriptor(bindingMaterials, materialBuffer_);
    writeBufferDescriptor(bindingLights, lightBuffer_);
    VkDescriptorImageInfo colorInfo{};
    colorInfo.imageView = baseColorImage_.view;
    colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorGetInfoEXT colorDescriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    colorDescriptor.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    colorDescriptor.data.pSampledImage = &colorInfo;
    vkGetDescriptorEXT(device_, &colorDescriptor, descriptorSize(colorDescriptor.type),
                       bytes + descriptorOffsets_[bindingBaseColorTexture]);
    VkDescriptorImageInfo roughnessInfo{};
    roughnessInfo.imageView = roughnessImage_.view;
    roughnessInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorGetInfoEXT roughnessDescriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    roughnessDescriptor.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    roughnessDescriptor.data.pSampledImage = &roughnessInfo;
    vkGetDescriptorEXT(device_, &roughnessDescriptor, descriptorSize(roughnessDescriptor.type),
                       bytes + descriptorOffsets_[bindingRoughnessTexture]);
    VkDescriptorGetInfoEXT samplerDescriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    samplerDescriptor.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    samplerDescriptor.data.pSampler = &sampler_;
    vkGetDescriptorEXT(device_, &samplerDescriptor, descriptorSize(samplerDescriptor.type),
                       bytes + descriptorOffsets_[bindingTextureSampler]);
    writeBufferDescriptor(bindingPathQueueA, pixel_.pathA);
    writeBufferDescriptor(bindingPathQueueB, pixel_.pathB);
    writeBufferDescriptor(bindingHitStates, pixel_.hits);
    writeBufferDescriptor(bindingCounters, pixel_.counters);
    writeBufferDescriptor(bindingIndirectArgs, pixel_.indirect);
    writeBufferDescriptor(bindingShadowTasks, pixel_.shadowTasks);
    writeBufferDescriptor(bindingShadowResults, pixel_.shadowResults);
    writeBufferDescriptor(bindingBranchRecords, pixel_.branchRecords);
    writeBufferDescriptor(bindingReservoirInitial, pixel_.reservoirInitial);
    writeBufferDescriptor(bindingReservoirTemporal, pixel_.reservoirTemporal);
    writeBufferDescriptor(bindingReservoirHistory, pixel_.reservoirHistory);
    writeBufferDescriptor(bindingSvgfHistory, pixel_.svgfHistory);
    writeBufferDescriptor(bindingAtrousA, pixel_.atrousA);
    writeBufferDescriptor(bindingAtrousB, pixel_.atrousB);
    writeBufferDescriptor(bindingCamera, cameraBuffer_);
}

void Application::destroySwapchain() {
    for (VkSemaphore semaphore : presentSemaphores_) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, semaphore, nullptr);
    }
    presentSemaphores_.clear();
    for (VkImageView view : swapchainViews_) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device_, view, nullptr);
    }
    swapchainViews_.clear();
    swapchainImages_.clear();
    swapchainLayouts_.clear();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Application::recreateSwapchain() {
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
        fail(std::string("查询 framebuffer 尺寸失败: ") + SDL_GetError());
    }
    if (width == 0 || height == 0) {
        minimized_ = true;
        return;
    }
    VkSurfaceCapabilitiesKHR capabilities{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities),
          "恢复时查询 surface capabilities");
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max() &&
        (capabilities.currentExtent.width == 0U || capabilities.currentExtent.height == 0U)) {
        minimized_ = true;
        return;
    }
    check(vkDeviceWaitIdle(device_), "swapchain 重建 vkDeviceWaitIdle");
    destroyBuffer(descriptorBuffer_);
    destroyPixelResources();
    destroySwapchain();
    createSwapchain();
    createPixelResources();
    createDescriptorBuffer();
    historyValid_ = false;
    previousJitter_ = {};
    resizePending_ = false;
    minimized_ = false;
    resetStatistics();
}

PushConstantsGpu Application::makePush(std::uint32_t parity) const {
    PushConstantsGpu push{};
    push.params.z = parity;
    push.counts = {pixel_.pixelCount, pixel_.branchCapacity, extent_.width, extent_.height};
    push.extra = {static_cast<std::uint32_t>(scene_.lights.size()),
                  static_cast<std::uint32_t>(materialSet_.materials.size()),
                  glassMaterialIndex_, kSvgfDomainCount};
    return push;
}

void Application::bindDescriptorBuffer(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint) const {
    VkDescriptorBufferBindingInfoEXT binding{VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
    binding.address = descriptorBuffer_.address;
    binding.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                    VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
    vkCmdBindDescriptorBuffersEXT(commandBuffer, 1, &binding);
    const std::uint32_t bufferIndex = 0;
    const VkDeviceSize offset = 0;
    vkCmdSetDescriptorBufferOffsetsEXT(commandBuffer, bindPoint, pipelineLayout_, 0, 1,
                                       &bufferIndex, &offset);
}

void Application::bindCompute(VkCommandBuffer commandBuffer, VkShaderEXT shader,
                              const PushConstantsGpu& push) const {
    const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    vkCmdBindShadersEXT(commandBuffer, 1, &stage, &shader);
    bindDescriptorBuffer(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
}

void Application::memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 sourceStage,
                                VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                                VkAccessFlags2 destinationAccess) const {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = sourceStage;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstStageMask = destinationStage;
    barrier.dstAccessMask = destinationAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void Application::runShadowPass(VkCommandBuffer commandBuffer, const PushConstantsGpu& basePush) const {
    PushConstantsGpu push = basePush;
    push.params.y = 1U;
    bindCompute(commandBuffer, shaders_.buildIndirect, push);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline_);
    bindDescriptorBuffer(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdTraceRaysIndirectKHR(commandBuffer, &shadowRaygenRegion_, &missRegion_, &hitRegion_, &callableRegion_,
                              pixel_.indirect.address + 32U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    bindCompute(commandBuffer, shaders_.resolveShadow, push);
    vkCmdDispatchIndirect(commandBuffer, pixel_.indirect.handle, 48U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

void Application::recordFrame(VkCommandBuffer commandBuffer, std::uint32_t imageIndex, std::uint32_t parity) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(frame)");
    PushConstantsGpu push = makePush(parity);
    const std::uint32_t pixelGroups = (pixel_.pixelCount + 63U) / 64U;

    bindCompute(commandBuffer, shaders_.prepare, push);
    vkCmdDispatch(commandBuffer, pixelGroups, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline_);
    bindDescriptorBuffer(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    vkCmdTraceRaysKHR(commandBuffer, &traceRaygenRegion_, &missRegion_, &hitRegion_, &callableRegion_,
                      pixel_.pixelCount, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    vkCmdFillBuffer(commandBuffer, pixel_.counters.handle, 4U, 8U, 0U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    push.params.x = 0U;
    push.params.y = 0U;
    bindCompute(commandBuffer, shaders_.shade, push);
    vkCmdDispatch(commandBuffer, pixelGroups, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    bindCompute(commandBuffer, shaders_.restirInitial, push);
    vkCmdDispatch(commandBuffer, pixelGroups, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    bindCompute(commandBuffer, shaders_.restirTemporal, push);
    vkCmdDispatch(commandBuffer, pixelGroups, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    bindCompute(commandBuffer, shaders_.restirSpatial, push);
    vkCmdDispatch(commandBuffer, pixelGroups, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    vkCmdFillBuffer(commandBuffer, pixel_.counters.handle, 8U, 4U, 0U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    bindCompute(commandBuffer, shaders_.restirVisibility, push);
    vkCmdDispatch(commandBuffer, pixelGroups, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    runShadowPass(commandBuffer, push);

    push.params.x = 1U;
    push.params.y = 0U;
    bindCompute(commandBuffer, shaders_.buildIndirect, push);
    vkCmdDispatch(commandBuffer, 1, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    std::uint32_t currentQueue = 1U;
    for (std::uint32_t segment = 0; segment < 8U; ++segment) {
        push.params.x = currentQueue;
        push.params.y = segment + 1U;
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rayTracingPipeline_);
        bindDescriptorBuffer(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);
        vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
        vkCmdTraceRaysIndirectKHR(commandBuffer, &traceRaygenRegion_, &missRegion_, &hitRegion_,
                                  &callableRegion_, pixel_.indirect.address);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        const VkDeviceSize nextCounterOffset = currentQueue == 0U ? 4U : 0U;
        vkCmdFillBuffer(commandBuffer, pixel_.counters.handle, nextCounterOffset, 4U, 0U);
        vkCmdFillBuffer(commandBuffer, pixel_.counters.handle, 8U, 4U, 0U);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        bindCompute(commandBuffer, shaders_.shade, push);
        vkCmdDispatchIndirect(commandBuffer, pixel_.indirect.handle, 16U);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        runShadowPass(commandBuffer, push);
        currentQueue = 1U - currentQueue;
        push.params.x = currentQueue;
        push.params.y = 0U;
        bindCompute(commandBuffer, shaders_.buildIndirect, push);
        vkCmdDispatch(commandBuffer, 1, 1, 1);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    push.params.y = 0U;
    bindCompute(commandBuffer, shaders_.svgfTemporal, push);
    const std::uint32_t svgfGroups = (pixel_.pixelCount * kSvgfDomainCount + 63U) / 64U;
    vkCmdDispatch(commandBuffer, svgfGroups, 1, 1);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    constexpr std::array steps{1U, 2U, 4U, 8U, 16U};
    for (std::uint32_t iteration = 0; iteration < steps.size(); ++iteration) {
        push.params.y = iteration;
        push.params.w = steps[iteration];
        bindCompute(commandBuffer, shaders_.atrous, push);
        vkCmdDispatch(commandBuffer, svgfGroups, 1, 1);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      iteration + 1U == steps.size() ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                                     : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    VkImageMemoryBarrier2 toAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment.oldLayout = swapchainLayouts_[imageIndex];
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.image = swapchainImages_[imageIndex];
    toAttachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toAttachment.subresourceRange.levelCount = 1;
    toAttachment.subresourceRange.layerCount = 1;
    VkDependencyInfo attachmentDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    attachmentDependency.imageMemoryBarrierCount = 1;
    attachmentDependency.pImageMemoryBarriers = &toAttachment;
    vkCmdPipelineBarrier2(commandBuffer, &attachmentDependency);

    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = swapchainViews_[imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea.extent = extent_;
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    vkCmdBeginRendering(commandBuffer, &renderingInfo);
    const std::array<VkShaderStageFlagBits, 5> graphicsStages{
        VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT};
    const std::array<VkShaderEXT, 5> graphicsShaders{
        shaders_.vertex, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, shaders_.fragment};
    vkCmdBindShadersEXT(commandBuffer, static_cast<std::uint32_t>(graphicsStages.size()),
                        graphicsStages.data(), graphicsShaders.data());
    bindDescriptorBuffer(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(push), &push);
    VkViewport viewport{0.0F, 0.0F, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0F, 1.0F};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewportWithCount(commandBuffer, 1, &viewport);
    vkCmdSetScissorWithCount(commandBuffer, 1, &scissor);
    vkCmdSetVertexInputEXT(commandBuffer, 0, nullptr, 0, nullptr);
    vkCmdSetPrimitiveTopology(commandBuffer, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(commandBuffer, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(commandBuffer, VK_FALSE);
    vkCmdSetCullMode(commandBuffer, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(commandBuffer, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthBiasEnable(commandBuffer, VK_FALSE);
    vkCmdSetDepthTestEnable(commandBuffer, VK_FALSE);
    vkCmdSetDepthWriteEnable(commandBuffer, VK_FALSE);
    vkCmdSetDepthCompareOp(commandBuffer, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBoundsTestEnable(commandBuffer, VK_FALSE);
    vkCmdSetStencilTestEnable(commandBuffer, VK_FALSE);
    vkCmdSetLineWidth(commandBuffer, 1.0F);
    vkCmdSetPolygonModeEXT(commandBuffer, VK_POLYGON_MODE_FILL);
    vkCmdSetRasterizationSamplesEXT(commandBuffer, VK_SAMPLE_COUNT_1_BIT);
    const VkSampleMask sampleMask = 1U;
    vkCmdSetSampleMaskEXT(commandBuffer, VK_SAMPLE_COUNT_1_BIT, &sampleMask);
    vkCmdSetAlphaToCoverageEnableEXT(commandBuffer, VK_FALSE);
    const VkBool32 blendEnable = VK_FALSE;
    vkCmdSetColorBlendEnableEXT(commandBuffer, 0, 1, &blendEnable);
    const VkColorBlendEquationEXT blendEquation{VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                                VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
    vkCmdSetColorBlendEquationEXT(commandBuffer, 0, 1, &blendEquation);
    const VkColorComponentFlags writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    vkCmdSetColorWriteMaskEXT(commandBuffer, 0, 1, &writeMask);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRendering(commandBuffer);

    VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.image = swapchainImages_[imageIndex];
    toPresent.subresourceRange = toAttachment.subresourceRange;
    VkDependencyInfo presentDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    presentDependency.imageMemoryBarrierCount = 1;
    presentDependency.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(commandBuffer, &presentDependency);
    swapchainLayouts_[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(frame)");
}

void Application::processEvents(bool& running, bool& cameraCut) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            running = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            resizePending_ = event.window.data1 > 0 && event.window.data2 > 0;
            minimized_ = event.window.data1 == 0 || event.window.data2 == 0;
            if (minimized_) resetStatistics();
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            minimized_ = true;
            resetStatistics();
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            minimized_ = false;
            resizePending_ = true;
            lastLoopTime_ = Clock::now();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (mouseCaptured_) {
                SDL_SetWindowRelativeMouseMode(window_, false);
                mouseCaptured_ = false;
            }
            lastLoopTime_ = Clock::now();
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_RIGHT && !mouseCaptured_) {
                if (!SDL_SetWindowRelativeMouseMode(window_, true)) {
                    fail(std::string("捕获相对鼠标失败: ") + SDL_GetError());
                }
                mouseCaptured_ = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_RIGHT && mouseCaptured_) {
                SDL_SetWindowRelativeMouseMode(window_, false);
                mouseCaptured_ = false;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (mouseCaptured_) {
                camera_.yaw += event.motion.xrel * 0.0025F;
                camera_.pitch = std::clamp(camera_.pitch - event.motion.yrel * 0.0025F,
                                           -radians(89.0F), radians(89.0F));
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat && event.key.key == SDLK_R) {
                camera_ = CameraState{};
                cameraCut = true;
            } else if (!event.key.repeat && event.key.key == SDLK_ESCAPE) {
                if (mouseCaptured_) {
                    SDL_SetWindowRelativeMouseMode(window_, false);
                    mouseCaptured_ = false;
                } else {
                    running = false;
                }
            }
            break;
        default:
            break;
        }
    }
}

bool Application::updateCamera(float elapsedSeconds) {
    if (minimized_) return false;
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    const auto pressed = [keys, keyCount](SDL_Scancode code) {
        const int index = static_cast<int>(code);
        return index >= 0 && index < keyCount && keys[index];
    };
    Vec3 movement{};
    Vec3 forward = cameraForward(camera_);
    Vec3 horizontalForward = normalize(Vec3{forward.x, 0.0F, forward.z});
    Vec3 right = cameraRight(camera_);
    if (pressed(SDL_SCANCODE_W)) movement += horizontalForward;
    if (pressed(SDL_SCANCODE_S)) movement -= horizontalForward;
    if (pressed(SDL_SCANCODE_D)) movement += right;
    if (pressed(SDL_SCANCODE_A)) movement -= right;
    if (pressed(SDL_SCANCODE_E)) movement.y += 1.0F;
    if (pressed(SDL_SCANCODE_Q)) movement.y -= 1.0F;
    if (dot(movement, movement) <= 0.0F) return false;
    const bool fast = pressed(SDL_SCANCODE_LSHIFT) || pressed(SDL_SCANCODE_RSHIFT);
    const float speed = fast ? 9.0F : 3.0F;
    camera_.position += normalize(movement) * (speed * elapsedSeconds);
    return true;
}

float halton(std::uint64_t index, std::uint32_t base) {
    float result = 0.0F;
    float fraction = 1.0F;
    while (index != 0U) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

void Application::updateCameraBuffer(std::uint32_t parity, bool cameraMoved, bool cameraCut) {
    jitter_ = {halton(renderedFrame_ + 1U, 2U) - 0.5F, halton(renderedFrame_ + 1U, 3U) - 0.5F};
    const Vec3 forward = cameraForward(camera_);
    const Vec3 right = cameraRight(camera_);
    const Vec3 up = normalize(cross(right, forward));
    const Vec3 previousForward = cameraForward(previousCamera_);
    const Vec3 previousRight = cameraRight(previousCamera_);
    const Vec3 previousUp = normalize(cross(previousRight, previousForward));
    CameraGpu cameraData{};
    cameraData.currentPositionTanHalfFov = makeVec4(camera_.position, std::tan(camera_.fovY * 0.5F));
    cameraData.currentForwardAspect = makeVec4(forward, static_cast<float>(extent_.width) /
                                                       static_cast<float>(extent_.height));
    cameraData.currentRight = makeVec4(right);
    cameraData.currentUp = makeVec4(up);
    cameraData.previousPositionTanHalfFov = makeVec4(previousCamera_.position,
                                                     std::tan(previousCamera_.fovY * 0.5F));
    cameraData.previousForwardAspect = makeVec4(previousForward, static_cast<float>(extent_.width) /
                                                                static_cast<float>(extent_.height));
    cameraData.previousRight = makeVec4(previousRight);
    cameraData.previousUp = makeVec4(previousUp);
    cameraData.jitterPixels = {jitter_.x, jitter_.y, previousJitter_.x, previousJitter_.y};
    std::uint32_t flags = historyValid_ && !cameraCut ? 1U : 0U;
    if (cameraMoved || cameraCut) flags |= 2U;
    cameraData.frame = {extent_.width, extent_.height, static_cast<std::uint32_t>(renderedFrame_), flags};
    auto* cameras = static_cast<CameraGpu*>(cameraBuffer_.mapped);
    cameras[parity] = cameraData;
}

bool Application::drawFrame(bool cameraMoved, bool cameraCut) {
    if (resizePending_) {
        recreateSwapchain();
        if (minimized_) return false;
        cameraCut = true;
    }
    const std::uint32_t parity = static_cast<std::uint32_t>(renderedFrame_ & 1U);
    FrameSlot& slot = frameSlots_[parity];
    if (slot.completionValue != 0U) {
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timeline_;
        waitInfo.pValues = &slot.completionValue;
        check(vkWaitSemaphores(device_, &waitInfo, std::numeric_limits<std::uint64_t>::max()),
              "等待 frame slot timeline");
    }
    updateCameraBuffer(parity, cameraMoved, cameraCut);
    std::uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_, std::numeric_limits<std::uint64_t>::max(),
                                             slot.acquireSemaphore, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) check(acquire, "vkAcquireNextImageKHR");
    check(vkResetCommandPool(device_, slot.commandPool, 0), "vkResetCommandPool(frame)");
    recordFrame(slot.commandBuffer, imageIndex, parity);

    std::array<VkSemaphoreSubmitInfo, 2> waits{};
    std::uint32_t waitCount = 0;
    waits[waitCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waits[waitCount].semaphore = slot.acquireSemaphore;
    waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    ++waitCount;
    if (lastHistoryValue_ != 0U) {
        waits[waitCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[waitCount].semaphore = timeline_;
        waits[waitCount].value = lastHistoryValue_;
        waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++waitCount;
    }
    const std::uint64_t signalValue = ++timelineValue_;
    std::array<VkSemaphoreSubmitInfo, 2> signals{};
    signals[0] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signals[0].semaphore = timeline_;
    signals[0].value = signalValue;
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signals[1].semaphore = presentSemaphores_[imageIndex];
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandInfo.commandBuffer = slot.commandBuffer;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = waitCount;
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;
    submit.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size());
    submit.pSignalSemaphoreInfos = signals.data();
    check(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2(frame)");
    slot.completionValue = signalValue;
    lastHistoryValue_ = signalValue;

    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &presentSemaphores_[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult present = vkQueuePresentKHR(queue_, &presentInfo);
    const bool presentSucceeded = present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR;
    previousCamera_ = camera_;
    previousJitter_ = jitter_;
    ++renderedFrame_;
    historyValid_ = true;
    if (presentSucceeded) recordPresentedFrame();
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    } else if (present != VK_SUCCESS) {
        check(present, "vkQueuePresentKHR");
    }
    return presentSucceeded;
}

void Application::resetStatistics() {
    statisticsFrames_ = 0;
    statisticsStart_ = {};
    if (window_ != nullptr) SDL_SetWindowTitle(window_, "VoxelPathTracer | FPS: -- | -- ms/frame");
}

void Application::recordPresentedFrame() {
    const Clock::time_point now = Clock::now();
    if (statisticsFrames_ == 0U) {
        statisticsStart_ = now;
        statisticsFrames_ = 1U;
        return;
    }
    ++statisticsFrames_;
    const double elapsed = std::chrono::duration<double>(now - statisticsStart_).count();
    if (elapsed < 0.5 || statisticsFrames_ < 2U) return;
    const double frames = static_cast<double>(statisticsFrames_ - 1U);
    const double fps = frames / elapsed;
    const double milliseconds = elapsed * 1000.0 / frames;
    std::ostringstream title;
    title.setf(std::ios::fixed);
    title.precision(1);
    title << "VoxelPathTracer | " << fps << " FPS | " << milliseconds << " ms/frame";
    SDL_SetWindowTitle(window_, title.str().c_str());
    statisticsStart_ = now;
    statisticsFrames_ = 1U;
}

void Application::initialize() {
    createWindow();
    loadInputs();
    createInstance();
    selectPhysicalDevice();
    createDevice();
    createCommandResources();
    createTimeline();
    createStaticBuffers();
    createTextures();
    createSampler();
    buildAccelerationStructures();
    createDescriptorLayout();
    createPipelineLayout();
    createRayTracingPipeline();
    createShaderObjects();
    createSwapchain();
    createPixelResources();
    createDescriptorBuffer();
    previousCamera_ = camera_;
    resetStatistics();
    lastLoopTime_ = Clock::now();
}

int Application::run() {
    initialize();
    bool running = true;
    while (running) {
        bool cameraCut = false;
        processEvents(running, cameraCut);
        if (!running) break;
        const Clock::time_point now = Clock::now();
        float elapsed = std::chrono::duration<float>(now - lastLoopTime_).count();
        lastLoopTime_ = now;
        if (minimized_) {
            int width = 0;
            int height = 0;
            if (!SDL_GetWindowSizeInPixels(window_, &width, &height)) {
                fail(std::string("最小化期间查询 framebuffer 尺寸失败: ") + SDL_GetError());
            }
            if (width > 0 && height > 0) {
                minimized_ = false;
                resizePending_ = true;
            } else {
                SDL_Delay(16);
                lastLoopTime_ = Clock::now();
                continue;
            }
        }
        elapsed = std::min(elapsed, 0.1F);
        const bool cameraMoved = updateCamera(elapsed) || cameraCut;
        drawFrame(cameraMoved, cameraCut);
        if (validationFailed_) fail("Vulkan validation 产生 warning/error，运行按失败处理");
    }
    return 0;
}

void Application::cleanup() {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    if (mouseCaptured_ && window_ != nullptr) SDL_SetWindowRelativeMouseMode(window_, false);
    if (device_ != VK_NULL_HANDLE) {
        destroyBuffer(descriptorBuffer_);
        destroyPixelResources();
        destroySwapchain();
        const std::array shaderList{
            shaders_.prepare, shaders_.buildIndirect, shaders_.shade, shaders_.restirInitial,
            shaders_.restirTemporal, shaders_.restirSpatial, shaders_.restirVisibility,
            shaders_.resolveShadow, shaders_.svgfTemporal, shaders_.atrous,
            shaders_.vertex, shaders_.fragment};
        for (VkShaderEXT shader : shaderList) {
            if (shader != VK_NULL_HANDLE) vkDestroyShaderEXT(device_, shader, nullptr);
        }
        shaders_ = {};
        if (rayTracingPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, rayTracingPipeline_, nullptr);
        destroyBuffer(sbtBuffer_);
        if (pipelineLayout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (descriptorSetLayout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
        if (tlas_.handle != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(device_, tlas_.handle, nullptr);
        destroyBuffer(tlas_.storage);
        for (AccelerationStructure& structure : blases_) {
            if (structure.handle != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(device_, structure.handle, nullptr);
            destroyBuffer(structure.storage);
        }
        blases_.clear();
        if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_, nullptr);
        destroyImage(baseColorImage_);
        destroyImage(roughnessImage_);
        destroyBuffer(cameraBuffer_);
        destroyBuffer(lightBuffer_);
        destroyBuffer(materialBuffer_);
        destroyBuffer(triangleBuffer_);
        destroyBuffer(indexBuffer_);
        destroyBuffer(vertexBuffer_);
        if (timeline_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, timeline_, nullptr);
        for (FrameSlot& slot : frameSlots_) {
            if (slot.acquireSemaphore != VK_NULL_HANDLE) vkDestroySemaphore(device_, slot.acquireSemaphore, nullptr);
            if (slot.commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device_, slot.commandPool, nullptr);
        }
        if (immediateCommandPool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, immediateCommandPool_, nullptr);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (debugMessenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
    if (window_ != nullptr) SDL_DestroyWindow(window_);
    window_ = nullptr;
    if (sdlInitialized_) SDL_Quit();
    sdlInitialized_ = false;
}

} // namespace

int runVoxelPathTracer() {
    Application application;
    return application.run();
}

} // namespace arena







