#include "renderer.hpp"

#include "common.hpp"
#include "materials.hpp"
#include "scene.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arena {
namespace {

#ifndef NDEBUG
constexpr bool validationEnabled = true;
#else
constexpr bool validationEnabled = false;
#endif

constexpr std::uint32_t frameSlotCount = 2U;
constexpr std::uint32_t domainCount = 5U;
constexpr std::uint32_t descriptorBindingCount = 36U;
constexpr std::uint32_t counterWordCount = 32U;
constexpr VkDeviceSize traceCommandOffset = 4U * sizeof(std::uint32_t);
constexpr VkDeviceSize shadeDispatchOffset = 8U * sizeof(std::uint32_t);
constexpr VkDeviceSize shadowTraceCommandOffset = 12U * sizeof(std::uint32_t);
constexpr VkDeviceSize shadowDispatchOffset = 16U * sizeof(std::uint32_t);
constexpr VkShaderStageFlags usedShaderStages =
    VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
    VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
    VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

struct alignas(16) FrameGpu {
    std::array<float, 4> cameraPosition{};
    std::array<float, 4> cameraRight{};
    std::array<float, 4> cameraUp{};
    std::array<float, 4> cameraForward{};
    std::array<float, 4> previousPosition{};
    std::array<float, 4> previousRight{};
    std::array<float, 4> previousUp{};
    std::array<float, 4> previousForward{};
    std::array<std::uint32_t, 4> extentFrameFlags{};
    std::array<float, 4> jitter{};
    std::array<float, 4> projection{};
    std::array<std::uint32_t, 4> counts{};
    std::array<float, 4> glassMinimum{};
    std::array<float, 4> glassMaximum{};
};

struct PushConstants {
    std::uint32_t frameSlot{};
    std::uint32_t passIndex{};
    std::uint32_t auxiliary{};
    std::uint32_t flags{};
};

struct Buffer {
    VkBuffer handle{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize size{};
    void* mapped{};
};

struct Image {
    VkImage handle{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    std::uint32_t mipLevels{};
    std::uint32_t layers{};
};

struct AccelerationStructure {
    Buffer storage;
    VkAccelerationStructureKHR handle{VK_NULL_HANDLE};
};

struct SwapchainState {
    VkSwapchainKHR handle{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> renderFinished;
    std::vector<bool> initialized;
};

struct FrameSlot {
    VkCommandPool commandPool{VK_NULL_HANDLE};
    VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
    VkSemaphore imageAvailable{VK_NULL_HANDLE};
    Buffer descriptorBuffer;
    std::uint64_t completionValue{};
};

struct Camera {
    Vec3 position{8.0F, 2.5F, 20.0F};
    float yaw{};
    float pitch{};
    float fovRadians{60.0F * 3.14159265358979323846F / 180.0F};

    [[nodiscard]] Vec3 forward() const
    {
        const float cosine = std::cos(pitch);
        return normalize({std::sin(yaw) * cosine, std::sin(pitch), -std::cos(yaw) * cosine});
    }

    [[nodiscard]] Vec3 right() const { return normalize(cross(forward(), {0.0F, 1.0F, 0.0F})); }
    [[nodiscard]] Vec3 up() const { return normalize(cross(right(), forward())); }

    void reset()
    {
        position = {8.0F, 2.5F, 20.0F};
        const Vec3 initialForward = normalize(Vec3{8.0F, 2.0F, 6.0F} - position);
        yaw = std::atan2(initialForward.x, -initialForward.z);
        pitch = std::asin(initialForward.y);
    }
};

enum class ComputeShader : std::size_t {
    clear,
    guideInit,
    restirInitial,
    restirTemporal,
    restirSpatial,
    restirResolve,
    resetCounts,
    shade,
    prepareArgs,
    shadowResolve,
    terminalResolve,
    signalResolve,
    svgfTemporal,
    svgfVariance,
    atrous,
    compose,
    count,
};

struct DynamicResources {
    std::uint32_t pixelCount{};
    std::uint32_t pathCapacity{};
    Buffer pathA;
    Buffer pathB;
    Buffer hits;
    Buffer counters;
    Buffer shadowTasks;
    Buffer shadowVisibility;
    Buffer branchResults;
    Buffer commonResults;
    Buffer analytic;
    Buffer restirDirect;
    Buffer reservoirInitial;
    Buffer reservoirTemporal;
    std::array<Buffer, 2> reservoirHistory;
    Buffer rawSignals;
    Buffer albedo;
    std::array<Buffer, 2> guides;
    std::array<Buffer, 2> historyColor;
    std::array<Buffer, 2> historyMoments;
    std::array<Buffer, 2> historyLength;
    Buffer atrousA;
    Buffer atrousB;
    Buffer finalColor;
};

struct ShaderBindingTable {
    Buffer buffer;
    std::array<VkStridedDeviceAddressRegionKHR, 3> raygen{};
    VkStridedDeviceAddressRegionKHR miss{};
    VkStridedDeviceAddressRegionKHR hit{};
    VkStridedDeviceAddressRegionKHR callable{};
};

class Application final {
public:
    Application();
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int run();

private:
    void initializeSdl();
    void initializeVulkan();
    void createInstance();
    void selectPhysicalDevice();
    void createDevice();
    void createDebugMessenger();
    void createCommandInfrastructure();
    void loadSceneResources();
    void createDescriptorLayout();
    void createStaticBuffers();
    void createTextures();
    void buildAccelerationStructures();
    void createRayTracingPipeline();
    void createShaderObjects();
    void createFrameSlots();
    void createTimelineSemaphore();
    void createSwapchain();
    void createDynamicResources();
    void writeDescriptorBuffer(FrameSlot& slot, std::uint32_t readHistory,
                               std::uint32_t writeHistory);

    void destroySwapchain();
    void destroyDynamicResources();
    void destroyBuffer(Buffer& buffer);
    void destroyImage(Image& image);
    void destroyAcceleration(AccelerationStructure& acceleration);
    void shutdown() noexcept;

    [[nodiscard]] Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      VkMemoryPropertyFlags memoryProperties,
                                      std::string_view label);
    [[nodiscard]] Image createImage(VkFormat format, std::uint32_t width, std::uint32_t height,
                                    std::uint32_t mipLevels, std::uint32_t layers,
                                    VkImageUsageFlags usage);
    [[nodiscard]] std::uint32_t findMemoryType(std::uint32_t bits,
                                               VkMemoryPropertyFlags properties) const;
    [[nodiscard]] VkDeviceAddress bufferAddress(const Buffer& buffer) const;
    [[nodiscard]] VkDeviceAddress accelerationAddress(VkAccelerationStructureKHR acceleration) const;
    void uploadBuffer(Buffer& destination, std::span<const std::byte> bytes,
                      VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess);
    void submitImmediate(const std::function<void(VkCommandBuffer)>& record);
    void uploadTexture(Image& image, const std::vector<TextureMip>& mips,
                       std::uint32_t bytesPerTexel);

    [[nodiscard]] VkShaderModule loadShaderModule(const std::string& filename) const;
    [[nodiscard]] std::vector<std::uint32_t> loadSpirv(const std::string& filename) const;
    [[nodiscard]] VkShaderEXT createShaderObject(const std::string& filename,
                                                 VkShaderStageFlagBits stage,
                                                 VkShaderStageFlags nextStage = 0U,
                                                 VkShaderCreateFlagsEXT flags = 0U) const;

    void processEvents(bool& running, bool& resizeRequested, bool& cameraCut,
                       bool& timingReset);
    [[nodiscard]] bool updateCamera(float deltaSeconds);
    void rebuildForResize();
    [[nodiscard]] bool drawFrame(bool cameraMoved, bool cameraCut);
    void recordFrame(VkCommandBuffer commandBuffer, std::uint32_t slotIndex,
                     std::uint32_t imageIndex);
    void updateFrameData(std::uint32_t slotIndex, bool cameraMoved, bool cameraCut);
    void updateStatistics(bool presented);
    void resetStatistics();

    void bindCompute(VkCommandBuffer commandBuffer, ComputeShader shader,
                     const PushConstants& constants) const;
    void bindRayTracing(VkCommandBuffer commandBuffer, const PushConstants& constants) const;
    void memoryBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 sourceStage,
                       VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                       VkAccessFlags2 destinationAccess) const;
    void setGraphicsState(VkCommandBuffer commandBuffer) const;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData);

    SDL_Window* window_{};
    std::filesystem::path executableDirectory_;
    bool mouseCaptured_{};
    bool focused_{true};
    bool minimized_{};
    std::atomic_bool validationFailure_{};

    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debugMessenger_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkPhysicalDeviceProperties physicalProperties_{};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorProperties_{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties_{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties_{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    std::uint32_t queueFamily_{std::numeric_limits<std::uint32_t>::max()};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    VkCommandPool immediatePool_{VK_NULL_HANDLE};

    VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
    VkDeviceSize descriptorLayoutSize_{};
    std::array<VkDeviceSize, descriptorBindingCount> descriptorOffsets_{};

    Buffer frameDataBuffer_;
    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    Buffer triangleBuffer_;
    Buffer materialBuffer_;
    Buffer lightBuffer_;
    Image colorTexture_;
    Image roughnessTexture_;
    VkSampler textureSampler_{VK_NULL_HANDLE};
    std::vector<AccelerationStructure> blas_;
    AccelerationStructure tlas_;

    VkPipeline rayTracingPipeline_{VK_NULL_HANDLE};
    ShaderBindingTable sbt_;
    std::array<VkShaderEXT, static_cast<std::size_t>(ComputeShader::count)> computeShaders_{};
    VkShaderEXT vertexShader_{VK_NULL_HANDLE};
    VkShaderEXT fragmentShader_{VK_NULL_HANDLE};

    std::array<FrameSlot, frameSlotCount> frameSlots_{};
    VkSemaphore timelineSemaphore_{VK_NULL_HANDLE};
    std::uint64_t timelineValue_{};
    SwapchainState swapchain_;
    DynamicResources dynamic_;

    MaterialLibrary materials_;
    MaterialTextureSet materialTextures_;
    SceneMesh scene_;

    Camera camera_;
    Camera previousCamera_;
    Vec2 currentJitter_{};
    Vec2 previousJitter_{};
    std::uint64_t renderedFrameIndex_{};
    std::uint32_t frameSlotIndex_{};
    std::uint32_t historyReadIndex_{};
    bool historyValid_{};
    bool initialized_{};

    std::chrono::steady_clock::time_point statisticsStart_{};
    std::uint32_t statisticsFrames_{};
};

static_assert(sizeof(FrameGpu) == 224U);
static_assert(sizeof(PushConstants) == 16U);

[[nodiscard]] std::span<const std::byte> asBytes(const auto& vector)
{
    using Value = typename std::remove_reference_t<decltype(vector)>::value_type;
    return {reinterpret_cast<const std::byte*>(vector.data()), vector.size() * sizeof(Value)};
}

[[nodiscard]] float radicalInverse(std::uint64_t value, const std::uint32_t base)
{
    float result = 0.0F;
    float inverse = 1.0F / static_cast<float>(base);
    float factor = inverse;
    while (value > 0U) {
        result += static_cast<float>(value % base) * factor;
        value /= base;
        factor *= inverse;
    }
    return result;
}

Application::Application()
{
    camera_.reset();
    previousCamera_ = camera_;
    try {
        initializeSdl();
        initializeVulkan();
        initialized_ = true;
    } catch (...) {
        shutdown();
        throw;
    }
}

Application::~Application()
{
    shutdown();
}

void Application::initializeSdl()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw Error("SDL3 初始化失败：" + std::string(SDL_GetError()));
    }
    window_ = SDL_CreateWindow("VoxelPathTracer — -- FPS / -- ms/frame", 1280, 720,
                               SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                   SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window_ == nullptr) {
        throw Error("SDL3 创建窗口失败：" + std::string(SDL_GetError()));
    }
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr || *basePath == '\0') {
        throw Error("SDL3 无法取得可执行文件目录：" + std::string(SDL_GetError()));
    }
    executableDirectory_ = std::filesystem::path(basePath);
}

void Application::initializeVulkan()
{
    vkCheck(volkInitialize(), "volkInitialize");
    if (volkGetInstanceVersion() < VK_API_VERSION_1_4) {
        throw Error("Vulkan loader 不支持 Vulkan 1.4");
    }
    createInstance();
    volkLoadInstance(instance_);
    createDebugMessenger();
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
        throw Error("SDL3 创建 Vulkan Surface 失败：" + std::string(SDL_GetError()));
    }
    selectPhysicalDevice();
    createDevice();
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0U, &queue_);
    createCommandInfrastructure();
    loadSceneResources();
    createDescriptorLayout();
    createStaticBuffers();
    createTextures();
    buildAccelerationStructures();
    createRayTracingPipeline();
    createShaderObjects();
    createTimelineSemaphore();
    createFrameSlots();
    createSwapchain();
    createDynamicResources();
    resetStatistics();
}

void Application::createInstance()
{
    std::uint32_t sdlExtensionCount = 0U;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (sdlExtensions == nullptr) {
        throw Error("SDL3 查询 Vulkan instance 扩展失败：" + std::string(SDL_GetError()));
    }
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);
    if constexpr (validationEnabled) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }

    std::vector<const char*> layers;
    if constexpr (validationEnabled) {
        std::uint32_t layerCount = 0U;
        vkCheck(vkEnumerateInstanceLayerProperties(&layerCount, nullptr),
                "vkEnumerateInstanceLayerProperties(count)");
        std::vector<VkLayerProperties> available(layerCount);
        vkCheck(vkEnumerateInstanceLayerProperties(&layerCount, available.data()),
                "vkEnumerateInstanceLayerProperties(data)");
        const bool found = std::ranges::any_of(available, [](const VkLayerProperties& layer) {
            return std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
        });
        if (!found) {
            throw Error("Debug 构建要求 VK_LAYER_KHRONOS_validation，但当前 loader 未提供");
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    applicationInfo.pApplicationName = "VoxelPathTracer";
    applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.pEngineName = "RenderArena";
    applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    applicationInfo.apiVersion = VK_API_VERSION_1_4;

    VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugInfo.pfnUserCallback = &Application::debugCallback;
    debugInfo.pUserData = &validationFailure_;
    const VkValidationFeatureEnableEXT synchronizationValidation =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.pNext = &debugInfo;
    validationFeatures.enabledValidationFeatureCount = 1U;
    validationFeatures.pEnabledValidationFeatures = &synchronizationValidation;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pNext = validationEnabled ? &validationFeatures : nullptr;
    createInfo.pApplicationInfo = &applicationInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();
    vkCheck(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance");
}

void Application::createDebugMessenger()
{
    if constexpr (!validationEnabled) {
        return;
    }
    VkDebugUtilsMessengerCreateInfoEXT createInfo{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = &Application::debugCallback;
    createInfo.pUserData = &validationFailure_;
    vkCheck(vkCreateDebugUtilsMessengerEXT(instance_, &createInfo, nullptr, &debugMessenger_),
            "vkCreateDebugUtilsMessengerEXT");
}

VKAPI_ATTR VkBool32 VKAPI_CALL Application::debugCallback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    const VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData)
{
    static_cast<void>(type);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        auto* failed = static_cast<std::atomic_bool*>(userData);
        failed->store(true, std::memory_order_relaxed);
        std::cerr << "Vulkan validation: "
                  << (callbackData != nullptr && callbackData->pMessage != nullptr
                          ? callbackData->pMessage
                          : "未知诊断")
                  << '\n';
    }
    return VK_FALSE;
}

void Application::selectPhysicalDevice()
{
    constexpr std::array requiredExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    };

    std::uint32_t deviceCount = 0U;
    vkCheck(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr),
            "vkEnumeratePhysicalDevices(count)");
    if (deviceCount == 0U) {
        throw Error("没有 Vulkan 物理设备");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkCheck(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()),
            "vkEnumeratePhysicalDevices(data)");

    std::string lastReport;
    for (const VkPhysicalDevice candidate : devices) {
        std::vector<std::string> missing;
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_4) {
            missing.emplace_back("Vulkan 1.4 device API");
        }

        std::uint32_t extensionCount = 0U;
        vkCheck(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr),
                "vkEnumerateDeviceExtensionProperties(count)");
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkCheck(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount,
                                                      availableExtensions.data()),
                "vkEnumerateDeviceExtensionProperties(data)");
        std::set<std::string_view> names;
        for (const auto& extension : availableExtensions) {
            names.emplace(extension.extensionName);
        }
        for (const char* required : requiredExtensions) {
            if (!names.contains(required)) {
                missing.emplace_back(required);
            }
        }

        VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        VkPhysicalDeviceShaderObjectFeaturesEXT shaderObject{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamicState{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamicState2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInput{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &vulkan12;
        vulkan12.pNext = &vulkan13;
        vulkan13.pNext = &acceleration;
        acceleration.pNext = &rayTracing;
        rayTracing.pNext = &descriptor;
        descriptor.pNext = &shaderObject;
        shaderObject.pNext = &dynamicState;
        dynamicState.pNext = &dynamicState2;
        dynamicState2.pNext = &dynamicState3;
        dynamicState3.pNext = &vertexInput;
        vkGetPhysicalDeviceFeatures2(candidate, &features);

        const auto requireFeature = [&missing](const VkBool32 supported, const char* name) {
            if (supported == VK_FALSE) {
                missing.emplace_back(name);
            }
        };
        requireFeature(vulkan12.timelineSemaphore, "timelineSemaphore");
        requireFeature(vulkan12.bufferDeviceAddress, "bufferDeviceAddress");
        requireFeature(vulkan12.scalarBlockLayout, "scalarBlockLayout");
        requireFeature(vulkan13.dynamicRendering, "dynamicRendering");
        requireFeature(vulkan13.synchronization2, "synchronization2");
        requireFeature(acceleration.accelerationStructure, "accelerationStructure");
        requireFeature(rayTracing.rayTracingPipeline, "rayTracingPipeline");
        requireFeature(rayTracing.rayTracingPipelineTraceRaysIndirect,
                       "rayTracingPipelineTraceRaysIndirect");
        requireFeature(descriptor.descriptorBuffer, "descriptorBuffer");
        requireFeature(shaderObject.shaderObject, "shaderObject");
        requireFeature(dynamicState.extendedDynamicState, "extendedDynamicState");
        requireFeature(dynamicState2.extendedDynamicState2, "extendedDynamicState2");
        requireFeature(vertexInput.vertexInputDynamicState, "vertexInputDynamicState");
        requireFeature(dynamicState3.extendedDynamicState3PolygonMode,
                       "extendedDynamicState3PolygonMode");
        requireFeature(dynamicState3.extendedDynamicState3DepthClampEnable,
                       "extendedDynamicState3DepthClampEnable");
        requireFeature(dynamicState3.extendedDynamicState3RasterizationSamples,
                       "extendedDynamicState3RasterizationSamples");
        requireFeature(dynamicState3.extendedDynamicState3SampleMask,
                       "extendedDynamicState3SampleMask");
        requireFeature(dynamicState3.extendedDynamicState3AlphaToCoverageEnable,
                       "extendedDynamicState3AlphaToCoverageEnable");
        requireFeature(dynamicState3.extendedDynamicState3LogicOpEnable,
                       "extendedDynamicState3LogicOpEnable");
        requireFeature(dynamicState3.extendedDynamicState3ColorBlendEnable,
                       "extendedDynamicState3ColorBlendEnable");
        requireFeature(dynamicState3.extendedDynamicState3ColorBlendEquation,
                       "extendedDynamicState3ColorBlendEquation");
        requireFeature(dynamicState3.extendedDynamicState3ColorWriteMask,
                       "extendedDynamicState3ColorWriteMask");

        std::uint32_t queueCount = 0U;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
        std::optional<std::uint32_t> family;
        for (std::uint32_t index = 0U; index < queueCount; ++index) {
            VkBool32 present = VK_FALSE;
            vkCheck(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface_, &present),
                    "vkGetPhysicalDeviceSurfaceSupportKHR");
            const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((queues[index].queueFlags & required) == required && present != VK_FALSE) {
                family = index;
                break;
            }
        }
        if (!family.has_value()) {
            missing.emplace_back("支持 graphics+compute+present 的统一队列族");
        }

        if (missing.empty()) {
            physicalDevice_ = candidate;
            queueFamily_ = *family;
            physicalProperties_ = properties;
            vkGetPhysicalDeviceMemoryProperties(candidate, &memoryProperties_);
            descriptorProperties_.pNext = &rayTracingProperties_;
            rayTracingProperties_.pNext = &accelerationProperties_;
            VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties2.pNext = &descriptorProperties_;
            vkGetPhysicalDeviceProperties2(candidate, &properties2);
            return;
        }

        std::ostringstream report;
        report << properties.deviceName << " 缺少：";
        for (std::size_t index = 0; index < missing.size(); ++index) {
            report << (index == 0U ? " " : ", ") << missing[index];
        }
        lastReport = report.str();
    }
    throw Error("没有满足 VoxelPathTracer 能力合同的设备。最后检查结果：" + lastReport);
}

void Application::createDevice()
{
    constexpr std::array extensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    };
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1U;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vulkan12.timelineSemaphore = VK_TRUE;
    vulkan12.bufferDeviceAddress = VK_TRUE;
    vulkan12.scalarBlockLayout = VK_TRUE;
    VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    vulkan13.dynamicRendering = VK_TRUE;
    vulkan13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    acceleration.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracing{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    rayTracing.rayTracingPipeline = VK_TRUE;
    rayTracing.rayTracingPipelineTraceRaysIndirect = VK_TRUE;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    descriptor.descriptorBuffer = VK_TRUE;
    VkPhysicalDeviceShaderObjectFeaturesEXT shaderObject{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    shaderObject.shaderObject = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT dynamicState{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    dynamicState.extendedDynamicState = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT dynamicState2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    dynamicState2.extendedDynamicState2 = VK_TRUE;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    dynamicState3.extendedDynamicState3PolygonMode = VK_TRUE;
    dynamicState3.extendedDynamicState3DepthClampEnable = VK_TRUE;
    dynamicState3.extendedDynamicState3RasterizationSamples = VK_TRUE;
    dynamicState3.extendedDynamicState3SampleMask = VK_TRUE;
    dynamicState3.extendedDynamicState3AlphaToCoverageEnable = VK_TRUE;
    dynamicState3.extendedDynamicState3LogicOpEnable = VK_TRUE;
    dynamicState3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    dynamicState3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    dynamicState3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInput{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    vertexInput.vertexInputDynamicState = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &vulkan12;
    vulkan12.pNext = &vulkan13;
    vulkan13.pNext = &acceleration;
    acceleration.pNext = &rayTracing;
    rayTracing.pNext = &descriptor;
    descriptor.pNext = &shaderObject;
    shaderObject.pNext = &dynamicState;
    dynamicState.pNext = &dynamicState2;
    dynamicState2.pNext = &dynamicState3;
    dynamicState3.pNext = &vertexInput;

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &features;
    createInfo.queueCreateInfoCount = 1U;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    vkCheck(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");
}

void Application::createCommandInfrastructure()
{
    VkCommandPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    createInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = queueFamily_;
    vkCheck(vkCreateCommandPool(device_, &createInfo, nullptr, &immediatePool_),
            "vkCreateCommandPool(immediate)");
}

void Application::loadSceneResources()
{
    const std::filesystem::path materialPath = executableDirectory_ / "materials.json";
    materials_ = MaterialLibrary::load(materialPath);
    materialTextures_ = materials_.generateTextures();
    scene_ = buildShowroom(materials_);
}

void Application::createDescriptorLayout()
{
    std::array<VkDescriptorSetLayoutBinding, descriptorBindingCount> bindings{};
    constexpr VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT |
                                           VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                           VK_SHADER_STAGE_MISS_BIT_KHR |
                                           VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    for (std::uint32_t index = 0U; index < descriptorBindingCount; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[index].descriptorCount = 1U;
        bindings[index].stageFlags = stages;
    }
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[34].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[35].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorSetLayout_),
            "vkCreateDescriptorSetLayout");
    vkGetDescriptorSetLayoutSizeEXT(device_, descriptorSetLayout_, &descriptorLayoutSize_);
    for (std::uint32_t index = 0U; index < descriptorBindingCount; ++index) {
        vkGetDescriptorSetLayoutBindingOffsetEXT(device_, descriptorSetLayout_, index,
                                                 &descriptorOffsets_[index]);
    }
    if (descriptorLayoutSize_ > descriptorProperties_.maxResourceDescriptorBufferRange ||
        descriptorProperties_.maxDescriptorBufferBindings < 1U) {
        throw Error("设备 Descriptor Buffer 容量不足以容纳统一资源布局");
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = stages;
    pushRange.offset = 0U;
    pushRange.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1U;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1U;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    vkCheck(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_),
            "vkCreatePipelineLayout");
}

std::uint32_t Application::findMemoryType(const std::uint32_t bits,
                                          const VkMemoryPropertyFlags properties) const
{
    for (std::uint32_t index = 0U; index < memoryProperties_.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0U &&
            (memoryProperties_.memoryTypes[index].propertyFlags & properties) == properties) {
            return index;
        }
    }
    throw Error("找不到满足资源属性的 Vulkan memory type");
}

Buffer Application::createBuffer(const VkDeviceSize size, const VkBufferUsageFlags usage,
                                 const VkMemoryPropertyFlags memoryProperties,
                                 const std::string_view label)
{
    if (size == 0U) {
        throw Error("禁止创建零尺寸 Vulkan buffer");
    }
    Buffer buffer;
    buffer.size = size;
    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    createInfo.size = size;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCheck(vkCreateBuffer(device_, &createInfo, nullptr, &buffer.handle),
            "vkCreateBuffer(" + std::string(label) + ")");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer.handle, &requirements);
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flagsInfo.flags = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0U
                          ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
                          : 0U;
    VkMemoryAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocationInfo.pNext = flagsInfo.flags != 0U ? &flagsInfo : nullptr;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, memoryProperties);
    vkCheck(vkAllocateMemory(device_, &allocationInfo, nullptr, &buffer.memory),
            "vkAllocateMemory(" + std::string(label) + ")");
    vkCheck(vkBindBufferMemory(device_, buffer.handle, buffer.memory, 0U),
            "vkBindBufferMemory(" + std::string(label) + ")");
    if ((memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
        vkCheck(vkMapMemory(device_, buffer.memory, 0U, size, 0U, &buffer.mapped),
                "vkMapMemory(" + std::string(label) + ")");
    }
    if (vkSetDebugUtilsObjectNameEXT != nullptr && !label.empty()) {
        const std::string name(label);
        VkDebugUtilsObjectNameInfoEXT nameInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
        nameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
        nameInfo.objectHandle = reinterpret_cast<std::uint64_t>(buffer.handle);
        nameInfo.pObjectName = name.c_str();
        vkCheck(vkSetDebugUtilsObjectNameEXT(device_, &nameInfo), "vkSetDebugUtilsObjectNameEXT");
    }
    return buffer;
}

Image Application::createImage(const VkFormat format, const std::uint32_t width,
                               const std::uint32_t height, const std::uint32_t mipLevels,
                               const std::uint32_t layers, const VkImageUsageFlags usage)
{
    Image image;
    image.format = format;
    image.mipLevels = mipLevels;
    image.layers = layers;
    VkImageCreateInfo createInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.format = format;
    createInfo.extent = {width, height, 1U};
    createInfo.mipLevels = mipLevels;
    createInfo.arrayLayers = layers;
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(device_, &createInfo, nullptr, &image.handle), "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image.handle, &requirements);
    VkMemoryAllocateInfo allocationInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex =
        findMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkCheck(vkAllocateMemory(device_, &allocationInfo, nullptr, &image.memory),
            "vkAllocateMemory(image)");
    vkCheck(vkBindImageMemory(device_, image.handle, image.memory, 0U), "vkBindImageMemory");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image.handle;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0U;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0U;
    viewInfo.subresourceRange.layerCount = layers;
    vkCheck(vkCreateImageView(device_, &viewInfo, nullptr, &image.view), "vkCreateImageView(texture)");
    return image;
}

VkDeviceAddress Application::bufferAddress(const Buffer& buffer) const
{
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer.handle;
    return vkGetBufferDeviceAddress(device_, &info);
}

VkDeviceAddress Application::accelerationAddress(const VkAccelerationStructureKHR acceleration) const
{
    VkAccelerationStructureDeviceAddressInfoKHR info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    info.accelerationStructure = acceleration;
    return vkGetAccelerationStructureDeviceAddressKHR(device_, &info);
}

void Application::submitImmediate(const std::function<void(VkCommandBuffer)>& record)
{
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = immediatePool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1U;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer),
            "vkAllocateCommandBuffers(immediate)");
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(immediate)");
    record(commandBuffer);
    vkCheck(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(immediate)");

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCheck(vkCreateFence(device_, &fenceInfo, nullptr, &fence), "vkCreateFence(immediate)");
    VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandInfo.commandBuffer = commandBuffer;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.commandBufferInfoCount = 1U;
    submitInfo.pCommandBufferInfos = &commandInfo;
    vkCheck(vkQueueSubmit2(queue_, 1U, &submitInfo, fence), "vkQueueSubmit2(immediate)");
    vkCheck(vkWaitForFences(device_, 1U, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()),
            "vkWaitForFences(immediate)");
    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, immediatePool_, 1U, &commandBuffer);
}

void Application::uploadBuffer(Buffer& destination, const std::span<const std::byte> bytes,
                               const VkPipelineStageFlags2 destinationStage,
                               const VkAccessFlags2 destinationAccess)
{
    if (bytes.size_bytes() > destination.size) {
        throw Error("上传数据超过目标 buffer 容量");
    }
    Buffer staging = createBuffer(bytes.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  "upload staging");
    std::memcpy(staging.mapped, bytes.data(), bytes.size_bytes());
    submitImmediate([&](const VkCommandBuffer commandBuffer) {
        VkBufferMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostBarrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        hostBarrier.buffer = staging.handle;
        hostBarrier.offset = 0U;
        hostBarrier.size = bytes.size_bytes();
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1U;
        dependency.pBufferMemoryBarriers = &hostBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        VkBufferCopy copy{0U, 0U, bytes.size_bytes()};
        vkCmdCopyBuffer(commandBuffer, staging.handle, destination.handle, 1U, &copy);
        VkBufferMemoryBarrier2 uploadBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        uploadBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        uploadBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        uploadBarrier.dstStageMask = destinationStage;
        uploadBarrier.dstAccessMask = destinationAccess;
        uploadBarrier.buffer = destination.handle;
        uploadBarrier.offset = 0U;
        uploadBarrier.size = destination.size;
        dependency.pBufferMemoryBarriers = &uploadBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    });
    destroyBuffer(staging);
}

void Application::createStaticBuffers()
{
    constexpr VkBufferUsageFlags commonUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    vertexBuffer_ = createBuffer(scene_.vertices.size() * sizeof(GpuVertex),
                                 commonUsage | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "vertices");
    indexBuffer_ = createBuffer(scene_.indices.size() * sizeof(std::uint32_t),
                                commonUsage | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "indices");
    triangleBuffer_ = createBuffer(scene_.triangles.size() * sizeof(GpuTriangle), commonUsage,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "triangles");
    const std::vector<GpuMaterial> gpuMaterials = makeGpuMaterials(materials_);
    materialBuffer_ = createBuffer(gpuMaterials.size() * sizeof(GpuMaterial), commonUsage,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "materials");
    lightBuffer_ = createBuffer(scene_.lights.size() * sizeof(GpuLight), commonUsage,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "lights");
    frameDataBuffer_ = createBuffer(sizeof(FrameGpu) * frameSlotCount,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    "frame data");

    constexpr VkPipelineStageFlags2 shaderAndBuildStages =
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkAccessFlags2 shaderAndBuildAccess =
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    uploadBuffer(vertexBuffer_, asBytes(scene_.vertices), shaderAndBuildStages, shaderAndBuildAccess);
    uploadBuffer(indexBuffer_, asBytes(scene_.indices), shaderAndBuildStages, shaderAndBuildAccess);
    uploadBuffer(triangleBuffer_, asBytes(scene_.triangles),
                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    uploadBuffer(materialBuffer_, asBytes(gpuMaterials),
                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    uploadBuffer(lightBuffer_, asBytes(scene_.lights),
                 VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void Application::uploadTexture(Image& image, const std::vector<TextureMip>& mips,
                                const std::uint32_t bytesPerTexel)
{
    VkDeviceSize totalSize = 0U;
    for (const TextureMip& mip : mips) {
        totalSize += mip.texels.size();
    }
    Buffer staging = createBuffer(totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  "texture staging");
    auto* destination = static_cast<std::byte*>(staging.mapped);
    VkDeviceSize offset = 0U;
    std::vector<VkBufferImageCopy> copies;
    for (std::uint32_t mipIndex = 0U; mipIndex < mips.size(); ++mipIndex) {
        const TextureMip& mip = mips[mipIndex];
        const VkDeviceSize layerSize = static_cast<VkDeviceSize>(mip.width) * mip.height * bytesPerTexel;
        std::memcpy(destination + offset, mip.texels.data(), mip.texels.size());
        for (std::uint32_t layer = 0U; layer < image.layers; ++layer) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset + layerSize * layer;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = mipIndex;
            copy.imageSubresource.baseArrayLayer = layer;
            copy.imageSubresource.layerCount = 1U;
            copy.imageExtent = {mip.width, mip.height, 1U};
            copies.push_back(copy);
        }
        offset += mip.texels.size();
    }
    submitImmediate([&](const VkCommandBuffer commandBuffer) {
        VkBufferMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostBarrier.srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT;
        hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        hostBarrier.buffer = staging.handle;
        hostBarrier.offset = 0U;
        hostBarrier.size = staging.size;
        VkImageMemoryBarrier2 toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toTransfer.srcAccessMask = 0U;
        toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.image = image.handle;
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, image.mipLevels, 0U, image.layers};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1U;
        dependency.pBufferMemoryBarriers = &hostBarrier;
        dependency.imageMemoryBarrierCount = 1U;
        dependency.pImageMemoryBarriers = &toTransfer;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        vkCmdCopyBufferToImage(commandBuffer, staging.handle, image.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(copies.size()), copies.data());
        VkImageMemoryBarrier2 toShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toShader.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        toShader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toShader.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toShader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.image = image.handle;
        toShader.subresourceRange = toTransfer.subresourceRange;
        dependency.bufferMemoryBarrierCount = 0U;
        dependency.pBufferMemoryBarriers = nullptr;
        dependency.pImageMemoryBarriers = &toShader;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    });
    destroyBuffer(staging);
}

void Application::createTextures()
{
    const std::uint32_t layerCount = static_cast<std::uint32_t>(materials_.materials().size());
    const auto requireFormat = [this](const VkFormat format) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                            VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & needed) != needed) {
            throw Error("设备不支持材质纹理所需的 sampled/linear/transfer 格式能力");
        }
    };
    requireFormat(VK_FORMAT_R8G8B8A8_SRGB);
    requireFormat(VK_FORMAT_R8_UNORM);
    colorTexture_ = createImage(VK_FORMAT_R8G8B8A8_SRGB, materialTextures_.baseSize,
                                materialTextures_.baseSize, materialTextures_.mipLevels, layerCount,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    roughnessTexture_ = createImage(VK_FORMAT_R8_UNORM, materialTextures_.baseSize,
                                    materialTextures_.baseSize, materialTextures_.mipLevels, layerCount,
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    uploadTexture(colorTexture_, materialTextures_.colorMips, 4U);
    uploadTexture(roughnessTexture_, materialTextures_.roughnessMips, 1U);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.mipLodBias = 0.0F;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0F;
    samplerInfo.maxLod = static_cast<float>(materialTextures_.mipLevels - 1U);
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    vkCheck(vkCreateSampler(device_, &samplerInfo, nullptr, &textureSampler_), "vkCreateSampler");
}

void Application::buildAccelerationStructures()
{
    const auto createAcceleration = [this](const VkDeviceSize size,
                                           const VkAccelerationStructureTypeKHR type,
                                           const std::string_view label) {
        AccelerationStructure result;
        result.storage = createBuffer(size,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, label);
        VkAccelerationStructureCreateInfoKHR createInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        createInfo.buffer = result.storage.handle;
        createInfo.offset = 0U;
        createInfo.size = size;
        createInfo.type = type;
        vkCheck(vkCreateAccelerationStructureKHR(device_, &createInfo, nullptr, &result.handle),
                "vkCreateAccelerationStructureKHR");
        return result;
    };

    const std::size_t groupCount = scene_.groups.size();
    std::vector<VkAccelerationStructureGeometryKHR> geometries(groupCount);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(groupCount);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(groupCount);
    std::vector<VkAccelerationStructureBuildSizesInfoKHR> sizes(
        groupCount, VkAccelerationStructureBuildSizesInfoKHR{
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR});
    std::vector<AccelerationStructure> originals;
    originals.reserve(groupCount);
    VkDeviceSize totalScratch = 0U;
    const VkDeviceAddress vertexAddress = bufferAddress(vertexBuffer_);
    const VkDeviceAddress indexAddress = bufferAddress(indexBuffer_);
    for (std::size_t index = 0; index < groupCount; ++index) {
        const GeometryGroup& group = scene_.groups[index];
        VkAccelerationStructureGeometryTrianglesDataKHR trianglesData{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        trianglesData.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        trianglesData.vertexData.deviceAddress = vertexAddress;
        trianglesData.vertexStride = sizeof(GpuVertex);
        trianglesData.maxVertex = static_cast<std::uint32_t>(scene_.vertices.size() - 1U);
        trianglesData.indexType = VK_INDEX_TYPE_UINT32;
        trianglesData.indexData.deviceAddress = indexAddress;
        geometries[index] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometries[index].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometries[index].geometry.triangles = trianglesData;
        geometries[index].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        ranges[index].primitiveCount = group.triangleCount;
        ranges[index].primitiveOffset = group.firstTriangle * 3U * sizeof(std::uint32_t);
        ranges[index].firstVertex = 0U;
        ranges[index].transformOffset = 0U;
        buildInfos[index] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        buildInfos[index].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfos[index].flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                                  VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        buildInfos[index].mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfos[index].geometryCount = 1U;
        buildInfos[index].pGeometries = &geometries[index];
        const std::uint32_t primitiveCount = group.triangleCount;
        vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &buildInfos[index], &primitiveCount, &sizes[index]);
        originals.push_back(createAcceleration(sizes[index].accelerationStructureSize,
                                               VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                               "uncompacted BLAS"));
        buildInfos[index].dstAccelerationStructure = originals.back().handle;
        totalScratch = alignUp(totalScratch,
                               static_cast<VkDeviceSize>(accelerationProperties_.minAccelerationStructureScratchOffsetAlignment));
        totalScratch += sizes[index].buildScratchSize;
    }
    Buffer scratch = createBuffer(totalScratch,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "BLAS scratch");
    VkDeviceSize scratchOffset = 0U;
    for (std::size_t index = 0; index < groupCount; ++index) {
        scratchOffset = alignUp(scratchOffset,
                                static_cast<VkDeviceSize>(accelerationProperties_.minAccelerationStructureScratchOffsetAlignment));
        buildInfos[index].scratchData.deviceAddress = bufferAddress(scratch) + scratchOffset;
        scratchOffset += sizes[index].buildScratchSize;
    }
    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    queryInfo.queryCount = static_cast<std::uint32_t>(groupCount);
    VkQueryPool queryPool = VK_NULL_HANDLE;
    vkCheck(vkCreateQueryPool(device_, &queryInfo, nullptr, &queryPool), "vkCreateQueryPool(BLAS compact)");
    submitImmediate([&](const VkCommandBuffer commandBuffer) {
        vkCmdResetQueryPool(commandBuffer, queryPool, 0U, static_cast<std::uint32_t>(groupCount));
        for (std::size_t index = 0; index < groupCount; ++index) {
            const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &ranges[index];
            vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1U, &buildInfos[index], &rangePointer);
        }
        VkMemoryBarrier2 buildBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        buildBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        buildBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        buildBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        buildBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1U;
        dependency.pMemoryBarriers = &buildBarrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        std::vector<VkAccelerationStructureKHR> handles;
        handles.reserve(groupCount);
        for (const auto& acceleration : originals) {
            handles.push_back(acceleration.handle);
        }
        vkCmdWriteAccelerationStructuresPropertiesKHR(
            commandBuffer, static_cast<std::uint32_t>(handles.size()), handles.data(),
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, queryPool, 0U);
    });

    std::vector<std::uint64_t> compactSizes(groupCount);
    vkCheck(vkGetQueryPoolResults(device_, queryPool, 0U, static_cast<std::uint32_t>(groupCount),
                                  compactSizes.size() * sizeof(std::uint64_t), compactSizes.data(),
                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
            "vkGetQueryPoolResults(BLAS compact)");
    blas_.reserve(groupCount);
    for (const std::uint64_t compactSize : compactSizes) {
        if (compactSize == 0U) {
            throw Error("驱动返回了零 BLAS compact size");
        }
        blas_.push_back(createAcceleration(compactSize,
                                           VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                           "compacted BLAS"));
    }
    submitImmediate([&](const VkCommandBuffer commandBuffer) {
        for (std::size_t index = 0; index < groupCount; ++index) {
            VkCopyAccelerationStructureInfoKHR copyInfo{
                VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
            copyInfo.src = originals[index].handle;
            copyInfo.dst = blas_[index].handle;
            copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkCmdCopyAccelerationStructureKHR(commandBuffer, &copyInfo);
        }
    });
    for (auto& original : originals) {
        destroyAcceleration(original);
    }
    destroyBuffer(scratch);
    vkDestroyQueryPool(device_, queryPool, nullptr);

    std::vector<VkAccelerationStructureInstanceKHR> instances(groupCount);
    for (std::size_t index = 0; index < groupCount; ++index) {
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform.matrix[0][0] = 1.0F;
        instance.transform.matrix[1][1] = 1.0F;
        instance.transform.matrix[2][2] = 1.0F;
        instance.instanceCustomIndex = scene_.groups[index].firstTriangle;
        instance.mask = 0xffU;
        instance.instanceShaderBindingTableRecordOffset = 0U;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = accelerationAddress(blas_[index].handle);
        instances[index] = instance;
    }
    Buffer instanceBuffer = createBuffer(
        instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "TLAS instances");
    uploadBuffer(instanceBuffer, asBytes(instances),
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VkAccelerationStructureGeometryInstancesDataKHR instancesData{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = bufferAddress(instanceBuffer);
    VkAccelerationStructureGeometryKHR tlasGeometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = instancesData;
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.geometryCount = 1U;
    tlasBuild.pGeometries = &tlasGeometry;
    const std::uint32_t instanceCount = static_cast<std::uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR tlasSize{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasBuild, &instanceCount, &tlasSize);
    tlas_ = createAcceleration(tlasSize.accelerationStructureSize,
                               VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, "TLAS");
    Buffer tlasScratch = createBuffer(tlasSize.buildScratchSize,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "TLAS scratch");
    tlasBuild.dstAccelerationStructure = tlas_.handle;
    tlasBuild.scratchData.deviceAddress = bufferAddress(tlasScratch);
    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = instanceCount;
    submitImmediate([&](const VkCommandBuffer commandBuffer) {
        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &tlasRange;
        vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1U, &tlasBuild, &rangePointer);
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1U;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    });
    destroyBuffer(tlasScratch);
    destroyBuffer(instanceBuffer);
}

std::vector<std::uint32_t> Application::loadSpirv(const std::string& filename) const
{
    const std::filesystem::path path = executableDirectory_ / "shaders" / filename;
    const std::vector<std::byte> bytes = readBinaryFile(path);
    std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

VkShaderModule Application::loadShaderModule(const std::string& filename) const
{
    const std::vector<std::uint32_t> code = loadSpirv(filename);
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = code.size() * sizeof(std::uint32_t);
    createInfo.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCheck(vkCreateShaderModule(device_, &createInfo, nullptr, &module),
            "vkCreateShaderModule(" + filename + ")");
    return module;
}

void Application::createRayTracingPipeline()
{
    constexpr std::array<const char*, 6> filenames{
        "primary.rgen.spv",      "continuation.rgen.spv", "shadow.rgen.spv",
        "radiance.rmiss.spv",   "shadow.rmiss.spv",      "radiance.rchit.spv",
    };
    constexpr std::array<VkShaderStageFlagBits, 6> stages{
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,      VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,      VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR,        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
    };
    std::array<VkShaderModule, filenames.size()> modules{};
    std::array<VkPipelineShaderStageCreateInfo, filenames.size()> stageInfos{};
    for (std::size_t index = 0; index < filenames.size(); ++index) {
        modules[index] = loadShaderModule(filenames[index]);
        stageInfos[index] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stageInfos[index].stage = stages[index];
        stageInfos[index].module = modules[index];
        stageInfos[index].pName = "main";
    }
    std::array<VkRayTracingShaderGroupCreateInfoKHR, 6> groups{};
    for (std::uint32_t index = 0U; index < 5U; ++index) {
        groups[index] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        groups[index].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[index].generalShader = index;
        groups[index].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[index].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[index].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[5] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[5].generalShader = VK_SHADER_UNUSED_KHR;
    groups[5].closestHitShader = 5U;
    groups[5].anyHitShader = VK_SHADER_UNUSED_KHR;
    groups[5].intersectionShader = VK_SHADER_UNUSED_KHR;

    VkRayTracingPipelineCreateInfoKHR pipelineInfo{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    pipelineInfo.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stageInfos.size());
    pipelineInfo.pStages = stageInfos.data();
    pipelineInfo.groupCount = static_cast<std::uint32_t>(groups.size());
    pipelineInfo.pGroups = groups.data();
    pipelineInfo.maxPipelineRayRecursionDepth = 1U;
    pipelineInfo.layout = pipelineLayout_;
    const VkResult pipelineResult = vkCreateRayTracingPipelinesKHR(
        device_, VK_NULL_HANDLE, VK_NULL_HANDLE, 1U, &pipelineInfo, nullptr, &rayTracingPipeline_);
    for (const VkShaderModule module : modules) {
        vkDestroyShaderModule(device_, module, nullptr);
    }
    vkCheck(pipelineResult, "vkCreateRayTracingPipelinesKHR");

    const std::uint32_t handleSize = rayTracingProperties_.shaderGroupHandleSize;
    const std::uint32_t handleAlignment = rayTracingProperties_.shaderGroupHandleAlignment;
    const std::uint32_t baseAlignment = rayTracingProperties_.shaderGroupBaseAlignment;
    const std::uint32_t recordStride = alignUp(handleSize, handleAlignment);
    const VkDeviceSize raygen0 = 0U;
    const VkDeviceSize raygen1 = alignUp(raygen0 + recordStride, static_cast<VkDeviceSize>(baseAlignment));
    const VkDeviceSize raygen2 = alignUp(raygen1 + recordStride, static_cast<VkDeviceSize>(baseAlignment));
    const VkDeviceSize missOffset = alignUp(raygen2 + recordStride, static_cast<VkDeviceSize>(baseAlignment));
    const VkDeviceSize hitOffset = alignUp(missOffset + recordStride * 2U,
                                           static_cast<VkDeviceSize>(baseAlignment));
    const VkDeviceSize sbtSize = hitOffset + recordStride;
    sbt_.buffer = createBuffer(sbtSize,
                               VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               "shader binding table");
    std::vector<std::byte> handles(groups.size() * handleSize);
    vkCheck(vkGetRayTracingShaderGroupHandlesKHR(device_, rayTracingPipeline_, 0U,
                                                 static_cast<std::uint32_t>(groups.size()),
                                                 handles.size(), handles.data()),
            "vkGetRayTracingShaderGroupHandlesKHR");
    auto* mapped = static_cast<std::byte*>(sbt_.buffer.mapped);
    std::memcpy(mapped + raygen0, handles.data() + 0U * handleSize, handleSize);
    std::memcpy(mapped + raygen1, handles.data() + 1U * handleSize, handleSize);
    std::memcpy(mapped + raygen2, handles.data() + 2U * handleSize, handleSize);
    std::memcpy(mapped + missOffset, handles.data() + 3U * handleSize, handleSize);
    std::memcpy(mapped + missOffset + recordStride, handles.data() + 4U * handleSize, handleSize);
    std::memcpy(mapped + hitOffset, handles.data() + 5U * handleSize, handleSize);
    const VkDeviceAddress baseAddress = bufferAddress(sbt_.buffer);
    sbt_.raygen[0] = {baseAddress + raygen0, recordStride, recordStride};
    sbt_.raygen[1] = {baseAddress + raygen1, recordStride, recordStride};
    sbt_.raygen[2] = {baseAddress + raygen2, recordStride, recordStride};
    sbt_.miss = {baseAddress + missOffset, recordStride, recordStride * 2U};
    sbt_.hit = {baseAddress + hitOffset, recordStride, recordStride};
    sbt_.callable = {};
}

VkShaderEXT Application::createShaderObject(const std::string& filename,
                                            const VkShaderStageFlagBits stage,
                                            const VkShaderStageFlags nextStage,
                                            const VkShaderCreateFlagsEXT flags) const
{
    const std::vector<std::uint32_t> code = loadSpirv(filename);
    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
            VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        0U, sizeof(PushConstants)};
    VkShaderCreateInfoEXT createInfo{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    createInfo.flags = flags;
    createInfo.stage = stage;
    createInfo.nextStage = nextStage;
    createInfo.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    createInfo.codeSize = code.size() * sizeof(std::uint32_t);
    createInfo.pCode = code.data();
    createInfo.pName = "main";
    createInfo.setLayoutCount = 1U;
    createInfo.pSetLayouts = &descriptorSetLayout_;
    createInfo.pushConstantRangeCount = 1U;
    createInfo.pPushConstantRanges = &pushRange;
    VkShaderEXT shader = VK_NULL_HANDLE;
    vkCheck(vkCreateShadersEXT(device_, 1U, &createInfo, nullptr, &shader),
            "vkCreateShadersEXT(" + filename + ")");
    return shader;
}

void Application::createShaderObjects()
{
    constexpr std::array<const char*, static_cast<std::size_t>(ComputeShader::count)> filenames{
        "clear.comp.spv",          "guide_init.comp.spv",      "restir_initial.comp.spv",
        "restir_temporal.comp.spv", "restir_spatial.comp.spv", "restir_resolve.comp.spv",
        "reset_counts.comp.spv",   "shade.comp.spv",           "prepare_args.comp.spv",
        "shadow_resolve.comp.spv", "terminal_resolve.comp.spv", "signal_resolve.comp.spv",
        "svgf_temporal.comp.spv",  "svgf_variance.comp.spv",   "atrous.comp.spv",
        "compose.comp.spv",
    };
    for (std::size_t index = 0; index < filenames.size(); ++index) {
        computeShaders_[index] = createShaderObject(filenames[index], VK_SHADER_STAGE_COMPUTE_BIT);
    }

    const std::vector<std::uint32_t> vertexCode = loadSpirv("display.vert.spv");
    const std::vector<std::uint32_t> fragmentCode = loadSpirv("display.frag.spv");
    const VkPushConstantRange pushRange{
        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
            VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        0U, sizeof(PushConstants)};
    std::array<VkShaderCreateInfoEXT, 2> createInfos{};
    createInfos[0] = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    createInfos[0].flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
    createInfos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    createInfos[0].nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
    createInfos[0].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    createInfos[0].codeSize = vertexCode.size() * sizeof(std::uint32_t);
    createInfos[0].pCode = vertexCode.data();
    createInfos[0].pName = "main";
    createInfos[0].setLayoutCount = 1U;
    createInfos[0].pSetLayouts = &descriptorSetLayout_;
    createInfos[0].pushConstantRangeCount = 1U;
    createInfos[0].pPushConstantRanges = &pushRange;
    createInfos[1] = createInfos[0];
    createInfos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    createInfos[1].nextStage = 0U;
    createInfos[1].codeSize = fragmentCode.size() * sizeof(std::uint32_t);
    createInfos[1].pCode = fragmentCode.data();
    std::array<VkShaderEXT, 2> shaders{};
    vkCheck(vkCreateShadersEXT(device_, static_cast<std::uint32_t>(createInfos.size()),
                               createInfos.data(), nullptr, shaders.data()),
            "vkCreateShadersEXT(display linked stages)");
    vertexShader_ = shaders[0];
    fragmentShader_ = shaders[1];
}

void Application::createTimelineSemaphore()
{
    VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0U;
    VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    createInfo.pNext = &typeInfo;
    vkCheck(vkCreateSemaphore(device_, &createInfo, nullptr, &timelineSemaphore_),
            "vkCreateSemaphore(timeline)");
}

void Application::createFrameSlots()
{
    const VkDeviceSize descriptorSize =
        alignUp(descriptorLayoutSize_, descriptorProperties_.descriptorBufferOffsetAlignment);
    for (std::uint32_t index = 0U; index < frameSlotCount; ++index) {
        FrameSlot& slot = frameSlots_[index];
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        vkCheck(vkCreateCommandPool(device_, &poolInfo, nullptr, &slot.commandPool),
                "vkCreateCommandPool(frame)");
        VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocateInfo.commandPool = slot.commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1U;
        vkCheck(vkAllocateCommandBuffers(device_, &allocateInfo, &slot.commandBuffer),
                "vkAllocateCommandBuffers(frame)");
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &slot.imageAvailable),
                "vkCreateSemaphore(image available)");
        slot.descriptorBuffer = createBuffer(
            descriptorSize,
            VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            "frame descriptor buffer");
    }
}

void Application::createSwapchain()
{
    VkSurfaceCapabilitiesKHR capabilities{};
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &capabilities),
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    std::uint32_t formatCount = 0U;
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr),
            "vkGetPhysicalDeviceSurfaceFormatsKHR(count)");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data()),
            "vkGetPhysicalDeviceSurfaceFormatsKHR(data)");
    const auto chooseFormat = [&formats](const VkFormat preferred) -> std::optional<VkSurfaceFormatKHR> {
        const auto iterator = std::ranges::find_if(formats, [preferred](const VkSurfaceFormatKHR& format) {
            return format.format == preferred &&
                   format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        return iterator == formats.end() ? std::nullopt : std::optional(*iterator);
    };
    std::optional<VkSurfaceFormatKHR> chosen = chooseFormat(VK_FORMAT_B8G8R8A8_SRGB);
    if (!chosen.has_value()) {
        chosen = chooseFormat(VK_FORMAT_R8G8B8A8_SRGB);
    }
    if (!chosen.has_value()) {
        throw Error("交换链没有可用的 sRGB + SRGB_NONLINEAR 格式");
    }
    int pixelWidth = 0;
    int pixelHeight = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight) || pixelWidth <= 0 ||
        pixelHeight <= 0) {
        throw Error("创建交换链时 framebuffer 像素尺寸为零或查询失败");
    }
    VkExtent2D extent{};
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = std::clamp(static_cast<std::uint32_t>(pixelWidth),
                                  capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(static_cast<std::uint32_t>(pixelHeight),
                                   capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    std::uint32_t imageCount = capabilities.minImageCount + 1U;
    if (capabilities.maxImageCount != 0U) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((capabilities.supportedCompositeAlpha & composite) == 0U) {
        for (const VkCompositeAlphaFlagBitsKHR candidate :
             {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
              VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
            if ((capabilities.supportedCompositeAlpha & candidate) != 0U) {
                composite = candidate;
                break;
            }
        }
    }
    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = chosen->format;
    createInfo.imageColorSpace = chosen->colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1U;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = composite;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;
    vkCheck(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_.handle),
            "vkCreateSwapchainKHR");
    swapchain_.format = chosen->format;
    swapchain_.extent = extent;
    vkCheck(vkGetSwapchainImagesKHR(device_, swapchain_.handle, &imageCount, nullptr),
            "vkGetSwapchainImagesKHR(count)");
    swapchain_.images.resize(imageCount);
    vkCheck(vkGetSwapchainImagesKHR(device_, swapchain_.handle, &imageCount, swapchain_.images.data()),
            "vkGetSwapchainImagesKHR(data)");
    swapchain_.views.resize(imageCount);
    swapchain_.renderFinished.resize(imageCount);
    swapchain_.initialized.assign(imageCount, false);
    for (std::uint32_t index = 0U; index < imageCount; ++index) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = swapchain_.images[index];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchain_.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
        vkCheck(vkCreateImageView(device_, &viewInfo, nullptr, &swapchain_.views[index]),
                "vkCreateImageView(swapchain)");
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                                  &swapchain_.renderFinished[index]),
                "vkCreateSemaphore(render finished)");
    }
}

void Application::createDynamicResources()
{
    const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(swapchain_.extent.width) *
                                       swapchain_.extent.height;
    if (pixelCount64 == 0U || pixelCount64 > std::numeric_limits<std::uint32_t>::max() / 2U) {
        throw Error("framebuffer 像素数超过路径队列的 uint32 容量");
    }
    dynamic_.pixelCount = static_cast<std::uint32_t>(pixelCount64);
    dynamic_.pathCapacity = dynamic_.pixelCount * 2U;
    const VkDeviceSize pixelCount = dynamic_.pixelCount;
    const VkDeviceSize pathCapacity = dynamic_.pathCapacity;
    const VkDeviceSize signalCount = pixelCount * domainCount;
    constexpr VkDeviceSize pathSize = 192U;
    constexpr VkDeviceSize hitSize = 64U;
    constexpr VkDeviceSize shadowTaskSize = 64U;
    constexpr VkDeviceSize branchResultSize = 80U;
    constexpr VkDeviceSize reservoirSize = 48U;
    const std::array<VkDeviceSize, 8> largestChecks{
        pathCapacity * pathSize, signalCount * branchResultSize, signalCount * 16U,
        pathCapacity * branchResultSize, pathCapacity * shadowTaskSize, pathCapacity * hitSize,
        pixelCount * reservoirSize, pixelCount * branchResultSize};
    for (const VkDeviceSize size : largestChecks) {
        if (size > physicalProperties_.limits.maxStorageBufferRange) {
            throw Error("framebuffer 对应的持久 storage buffer 超过 maxStorageBufferRange");
        }
    }
    constexpr VkBufferUsageFlags storageUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    dynamic_.pathA = createBuffer(pathCapacity * pathSize, storageUsage,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "paths A");
    dynamic_.pathB = createBuffer(pathCapacity * pathSize, storageUsage,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "paths B");
    dynamic_.hits = createBuffer(pathCapacity * hitSize, storageUsage,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "hits");
    dynamic_.counters = createBuffer(counterWordCount * sizeof(std::uint32_t),
                                     storageUsage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "GPU counters and indirect args");
    dynamic_.shadowTasks = createBuffer(pathCapacity * shadowTaskSize, storageUsage,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "shadow tasks");
    dynamic_.shadowVisibility = createBuffer(pathCapacity * sizeof(std::uint32_t), storageUsage,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "shadow visibility");
    dynamic_.branchResults = createBuffer(pathCapacity * branchResultSize, storageUsage,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "branch results");
    dynamic_.commonResults = createBuffer(pixelCount * branchResultSize, storageUsage,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "common prefix results");
    dynamic_.analytic = createBuffer(pixelCount * 16U, storageUsage,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "analytic signal");
    dynamic_.restirDirect = createBuffer(pixelCount * 16U, storageUsage,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "ReSTIR direct");
    dynamic_.reservoirInitial = createBuffer(pixelCount * reservoirSize, storageUsage,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "reservoir initial");
    dynamic_.reservoirTemporal = createBuffer(pixelCount * reservoirSize, storageUsage,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "reservoir temporal");
    for (std::uint32_t index = 0U; index < 2U; ++index) {
        dynamic_.reservoirHistory[index] = createBuffer(pixelCount * reservoirSize, storageUsage,
                                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                        "reservoir history");
    }
    dynamic_.rawSignals = createBuffer(signalCount * 16U, storageUsage,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "raw signals");
    dynamic_.albedo = createBuffer(signalCount * 16U, storageUsage,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "signal albedo");
    for (std::uint32_t index = 0U; index < 2U; ++index) {
        dynamic_.guides[index] = createBuffer(signalCount * branchResultSize, storageUsage,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "signal guides");
        dynamic_.historyColor[index] = createBuffer(signalCount * 16U, storageUsage,
                                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                    "SVGF temporal color");
        dynamic_.historyMoments[index] = createBuffer(signalCount * 16U, storageUsage,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                      "SVGF moments");
        dynamic_.historyLength[index] = createBuffer(signalCount * sizeof(std::uint32_t), storageUsage,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                     "SVGF history length");
    }
    dynamic_.atrousA = createBuffer(signalCount * 16U, storageUsage,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "atrous A");
    dynamic_.atrousB = createBuffer(signalCount * 16U, storageUsage,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "atrous B");
    dynamic_.finalColor = createBuffer(pixelCount * 16U, storageUsage,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "final HDR color");
    historyValid_ = false;
    historyReadIndex_ = 0U;
    for (FrameSlot& slot : frameSlots_) {
        writeDescriptorBuffer(slot, 0U, 1U);
    }
}

void Application::writeDescriptorBuffer(FrameSlot& slot, const std::uint32_t readHistory,
                                        const std::uint32_t writeHistory)
{
    std::memset(slot.descriptorBuffer.mapped, 0, static_cast<std::size_t>(slot.descriptorBuffer.size));
    auto* bytes = static_cast<std::byte*>(slot.descriptorBuffer.mapped);
    const auto writeStorage = [&](const std::uint32_t binding, const Buffer& buffer) {
        VkDescriptorAddressInfoEXT address{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        address.address = bufferAddress(buffer);
        address.range = buffer.size;
        address.format = VK_FORMAT_UNDEFINED;
        VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        info.data.pStorageBuffer = &address;
        vkGetDescriptorEXT(device_, &info, descriptorProperties_.storageBufferDescriptorSize,
                           bytes + descriptorOffsets_[binding]);
    };
    const auto writeCombinedImage = [&](const std::uint32_t binding, const Image& image) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = textureSampler_;
        imageInfo.imageView = image.view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        info.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        info.data.pCombinedImageSampler = &imageInfo;
        vkGetDescriptorEXT(device_, &info, descriptorProperties_.combinedImageSamplerDescriptorSize,
                           bytes + descriptorOffsets_[binding]);
    };
    writeStorage(0U, frameDataBuffer_);
    writeStorage(1U, vertexBuffer_);
    writeStorage(2U, indexBuffer_);
    writeStorage(3U, triangleBuffer_);
    writeStorage(4U, materialBuffer_);
    writeStorage(5U, lightBuffer_);
    VkDescriptorGetInfoEXT accelerationInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
    accelerationInfo.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    accelerationInfo.data.accelerationStructure = accelerationAddress(tlas_.handle);
    vkGetDescriptorEXT(device_, &accelerationInfo,
                       descriptorProperties_.accelerationStructureDescriptorSize,
                       bytes + descriptorOffsets_[6]);
    writeStorage(7U, dynamic_.pathA);
    writeStorage(8U, dynamic_.pathB);
    writeStorage(9U, dynamic_.hits);
    writeStorage(10U, dynamic_.counters);
    writeStorage(11U, dynamic_.shadowTasks);
    writeStorage(12U, dynamic_.shadowVisibility);
    writeStorage(13U, dynamic_.branchResults);
    writeStorage(14U, dynamic_.commonResults);
    writeStorage(15U, dynamic_.analytic);
    writeStorage(16U, dynamic_.restirDirect);
    writeStorage(17U, dynamic_.reservoirInitial);
    writeStorage(18U, dynamic_.reservoirTemporal);
    writeStorage(19U, dynamic_.reservoirHistory[writeHistory]);
    writeStorage(20U, dynamic_.reservoirHistory[readHistory]);
    writeStorage(21U, dynamic_.rawSignals);
    writeStorage(22U, dynamic_.albedo);
    writeStorage(23U, dynamic_.guides[writeHistory]);
    writeStorage(24U, dynamic_.guides[readHistory]);
    writeStorage(25U, dynamic_.historyColor[readHistory]);
    writeStorage(26U, dynamic_.historyColor[writeHistory]);
    writeStorage(27U, dynamic_.historyMoments[readHistory]);
    writeStorage(28U, dynamic_.historyMoments[writeHistory]);
    writeStorage(29U, dynamic_.historyLength[readHistory]);
    writeStorage(30U, dynamic_.historyLength[writeHistory]);
    writeStorage(31U, dynamic_.atrousA);
    writeStorage(32U, dynamic_.atrousB);
    writeStorage(33U, dynamic_.finalColor);
    writeCombinedImage(34U, colorTexture_);
    writeCombinedImage(35U, roughnessTexture_);
}

void Application::memoryBarrier(const VkCommandBuffer commandBuffer,
                                const VkPipelineStageFlags2 sourceStage,
                                const VkAccessFlags2 sourceAccess,
                                const VkPipelineStageFlags2 destinationStage,
                                const VkAccessFlags2 destinationAccess) const
{
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = sourceStage;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstStageMask = destinationStage;
    barrier.dstAccessMask = destinationAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1U;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

void Application::bindCompute(const VkCommandBuffer commandBuffer, const ComputeShader shader,
                              const PushConstants& constants) const
{
    const VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkShaderEXT shaderHandle = computeShaders_[static_cast<std::size_t>(shader)];
    vkCmdBindShadersEXT(commandBuffer, 1U, &stage, &shaderHandle);
    const std::uint32_t bufferIndex = 0U;
    const VkDeviceSize offset = 0U;
    vkCmdSetDescriptorBufferOffsetsEXT(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                       pipelineLayout_, 0U, 1U, &bufferIndex, &offset);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, usedShaderStages, 0U,
                       sizeof(PushConstants), &constants);
}

void Application::bindRayTracing(const VkCommandBuffer commandBuffer,
                                 const PushConstants& constants) const
{
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      rayTracingPipeline_);
    const std::uint32_t bufferIndex = 0U;
    const VkDeviceSize offset = 0U;
    vkCmdSetDescriptorBufferOffsetsEXT(commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                       pipelineLayout_, 0U, 1U, &bufferIndex, &offset);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, usedShaderStages, 0U,
                       sizeof(PushConstants), &constants);
}

void Application::setGraphicsState(const VkCommandBuffer commandBuffer) const
{
    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchain_.extent.width);
    viewport.height = static_cast<float>(swapchain_.extent.height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    const VkRect2D scissor{{0, 0}, swapchain_.extent};
    vkCmdSetViewportWithCount(commandBuffer, 1U, &viewport);
    vkCmdSetScissorWithCount(commandBuffer, 1U, &scissor);
    vkCmdSetPrimitiveTopology(commandBuffer, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(commandBuffer, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(commandBuffer, VK_FALSE);
    vkCmdSetCullMode(commandBuffer, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(commandBuffer, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthBiasEnable(commandBuffer, VK_FALSE);
    vkCmdSetDepthClampEnableEXT(commandBuffer, VK_FALSE);
    vkCmdSetPolygonModeEXT(commandBuffer, VK_POLYGON_MODE_FILL);
    vkCmdSetLineWidth(commandBuffer, 1.0F);
    vkCmdSetRasterizationSamplesEXT(commandBuffer, VK_SAMPLE_COUNT_1_BIT);
    const VkSampleMask sampleMask = 1U;
    vkCmdSetSampleMaskEXT(commandBuffer, VK_SAMPLE_COUNT_1_BIT, &sampleMask);
    vkCmdSetAlphaToCoverageEnableEXT(commandBuffer, VK_FALSE);
    vkCmdSetDepthTestEnable(commandBuffer, VK_FALSE);
    vkCmdSetDepthWriteEnable(commandBuffer, VK_FALSE);
    vkCmdSetDepthCompareOp(commandBuffer, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBoundsTestEnable(commandBuffer, VK_FALSE);
    vkCmdSetStencilTestEnable(commandBuffer, VK_FALSE);
    vkCmdSetLogicOpEnableEXT(commandBuffer, VK_FALSE);
    const VkBool32 blendEnable = VK_FALSE;
    vkCmdSetColorBlendEnableEXT(commandBuffer, 0U, 1U, &blendEnable);
    VkColorBlendEquationEXT blendEquation{};
    blendEquation.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendEquation.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendEquation.colorBlendOp = VK_BLEND_OP_ADD;
    blendEquation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendEquation.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendEquation.alphaBlendOp = VK_BLEND_OP_ADD;
    vkCmdSetColorBlendEquationEXT(commandBuffer, 0U, 1U, &blendEquation);
    const VkColorComponentFlags writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    vkCmdSetColorWriteMaskEXT(commandBuffer, 0U, 1U, &writeMask);
    vkCmdSetVertexInputEXT(commandBuffer, 0U, nullptr, 0U, nullptr);
}

void Application::updateFrameData(const std::uint32_t slotIndex, const bool cameraMoved,
                                  const bool cameraCut)
{
    const std::uint64_t sampleIndex = renderedFrameIndex_ + 1U;
    currentJitter_ = {radicalInverse(sampleIndex, 2U) - 0.5F,
                      radicalInverse(sampleIndex, 3U) - 0.5F};
    const Vec3 forward = camera_.forward();
    const Vec3 right = camera_.right();
    const Vec3 up = camera_.up();
    const Vec3 previousForward = previousCamera_.forward();
    const Vec3 previousRight = previousCamera_.right();
    const Vec3 previousUp = previousCamera_.up();
    const float aspect = static_cast<float>(swapchain_.extent.width) /
                         static_cast<float>(swapchain_.extent.height);
    const float tanHalfFov = std::tan(camera_.fovRadians * 0.5F);
    const float previousTanHalfFov = std::tan(previousCamera_.fovRadians * 0.5F);
    std::uint32_t flags = historyValid_ ? 1U : 0U;
    flags |= cameraMoved ? 2U : 0U;
    flags |= cameraCut ? 4U : 0U;
    FrameGpu frame;
    frame.cameraPosition = {camera_.position.x, camera_.position.y, camera_.position.z, 0.0F};
    frame.cameraRight = {right.x, right.y, right.z, 0.0F};
    frame.cameraUp = {up.x, up.y, up.z, 0.0F};
    frame.cameraForward = {forward.x, forward.y, forward.z, 0.0F};
    frame.previousPosition = {previousCamera_.position.x, previousCamera_.position.y,
                              previousCamera_.position.z, 0.0F};
    frame.previousRight = {previousRight.x, previousRight.y, previousRight.z, 0.0F};
    frame.previousUp = {previousUp.x, previousUp.y, previousUp.z, 0.0F};
    frame.previousForward = {previousForward.x, previousForward.y, previousForward.z, 0.0F};
    frame.extentFrameFlags = {swapchain_.extent.width, swapchain_.extent.height,
                              static_cast<std::uint32_t>(renderedFrameIndex_), flags};
    frame.jitter = {currentJitter_.x, currentJitter_.y, previousJitter_.x, previousJitter_.y};
    frame.projection = {tanHalfFov, aspect, previousTanHalfFov, aspect};
    frame.counts = {dynamic_.pixelCount, dynamic_.pathCapacity,
                    static_cast<std::uint32_t>(scene_.lights.size()),
                    static_cast<std::uint32_t>(materials_.materials().size())};
    const Material& glass = materials_.at(materials_.indexOf("glass"));
    frame.glassMinimum = {7.0F, 0.125F, 7.0F, glass.ior};
    frame.glassMaximum = {9.0F, 2.125F, 9.0F, 0.0F};
    auto* frames = static_cast<FrameGpu*>(frameDataBuffer_.mapped);
    frames[slotIndex] = frame;
}

void Application::recordFrame(const VkCommandBuffer commandBuffer, const std::uint32_t slotIndex,
                              const std::uint32_t imageIndex)
{
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(frame)");
    const VkDescriptorBufferBindingInfoEXT descriptorBinding{
        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, nullptr,
        bufferAddress(frameSlots_[slotIndex].descriptorBuffer),
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT};
    vkCmdBindDescriptorBuffersEXT(commandBuffer, 1U, &descriptorBinding);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT);

    PushConstants constants{slotIndex, 0U, 0U, 0U};
    const std::uint32_t maximumClear = std::max(dynamic_.pathCapacity,
                                                dynamic_.pixelCount * domainCount);
    bindCompute(commandBuffer, ComputeShader::clear, constants);
    vkCmdDispatch(commandBuffer, (maximumClear + 255U) / 256U, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    bindRayTracing(commandBuffer, constants);
    vkCmdTraceRaysKHR(commandBuffer, &sbt_.raygen[0], &sbt_.miss, &sbt_.hit, &sbt_.callable,
                      swapchain_.extent.width, swapchain_.extent.height, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const std::uint32_t pixelGroups256 = (dynamic_.pixelCount + 255U) / 256U;
    bindCompute(commandBuffer, ComputeShader::guideInit, constants);
    vkCmdDispatch(commandBuffer, pixelGroups256, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    bindCompute(commandBuffer, ComputeShader::restirInitial, constants);
    vkCmdDispatch(commandBuffer, pixelGroups256, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    bindCompute(commandBuffer, ComputeShader::restirTemporal, constants);
    vkCmdDispatch(commandBuffer, pixelGroups256, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    bindCompute(commandBuffer, ComputeShader::restirSpatial, constants);
    vkCmdDispatch(commandBuffer, pixelGroups256, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    constants.auxiliary = 0U;
    bindRayTracing(commandBuffer, constants);
    vkCmdTraceRaysKHR(commandBuffer, &sbt_.raygen[2], &sbt_.miss, &sbt_.hit, &sbt_.callable,
                      dynamic_.pixelCount, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    bindCompute(commandBuffer, ComputeShader::restirResolve, constants);
    vkCmdDispatch(commandBuffer, pixelGroups256, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    const VkDeviceAddress counterAddress = bufferAddress(dynamic_.counters);
    for (std::uint32_t bounce = 0U; bounce < 8U; ++bounce) {
        constants.passIndex = bounce;
        constants.auxiliary = 0U;
        bindCompute(commandBuffer, ComputeShader::resetCounts, constants);
        vkCmdDispatch(commandBuffer, 1U, 1U, 1U);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        bindCompute(commandBuffer, ComputeShader::shade, constants);
        if (bounce == 0U) {
            vkCmdDispatch(commandBuffer, (dynamic_.pixelCount + 63U) / 64U, 1U, 1U);
        } else {
            vkCmdDispatchIndirect(commandBuffer, dynamic_.counters.handle, shadeDispatchOffset);
        }
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        bindCompute(commandBuffer, ComputeShader::prepareArgs, constants);
        vkCmdDispatch(commandBuffer, 1U, 1U, 1U);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                          VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

        constants.auxiliary = 1U;
        bindRayTracing(commandBuffer, constants);
        vkCmdTraceRaysIndirectKHR(commandBuffer, &sbt_.raygen[2], &sbt_.miss, &sbt_.hit,
                                 &sbt_.callable, counterAddress + shadowTraceCommandOffset);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        bindCompute(commandBuffer, ComputeShader::shadowResolve, constants);
        vkCmdDispatchIndirect(commandBuffer, dynamic_.counters.handle, shadowDispatchOffset);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                          VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        constants.passIndex = bounce + 1U;
        constants.auxiliary = 0U;
        bindRayTracing(commandBuffer, constants);
        vkCmdTraceRaysIndirectKHR(commandBuffer, &sbt_.raygen[1], &sbt_.miss, &sbt_.hit,
                                 &sbt_.callable, counterAddress + traceCommandOffset);
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                          VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }

    constants.passIndex = 8U;
    bindCompute(commandBuffer, ComputeShader::terminalResolve, constants);
    vkCmdDispatchIndirect(commandBuffer, dynamic_.counters.handle, shadeDispatchOffset);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    constants.passIndex = 0U;
    constants.auxiliary = 0U;
    bindCompute(commandBuffer, ComputeShader::signalResolve, constants);
    vkCmdDispatch(commandBuffer, pixelGroups256, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const std::uint32_t signalCount = dynamic_.pixelCount * domainCount;
    const std::uint32_t signalGroups = (signalCount + 255U) / 256U;
    bindCompute(commandBuffer, ComputeShader::svgfTemporal, constants);
    vkCmdDispatch(commandBuffer, signalGroups, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    bindCompute(commandBuffer, ComputeShader::svgfVariance, constants);
    vkCmdDispatch(commandBuffer, signalGroups, 1U, 1U);
    for (std::uint32_t iteration = 0U; iteration < 5U; ++iteration) {
        memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        constants.passIndex = iteration;
        bindCompute(commandBuffer, ComputeShader::atrous, constants);
        vkCmdDispatch(commandBuffer, signalGroups, 1U, 1U);
    }
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    constants.passIndex = 0U;
    bindCompute(commandBuffer, ComputeShader::compose, constants);
    vkCmdDispatch(commandBuffer, pixelGroups256, 1U, 1U);
    memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    VkImageMemoryBarrier2 toAttachment{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.srcAccessMask = 0U;
    toAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toAttachment.oldLayout = swapchain_.initialized[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                                : VK_IMAGE_LAYOUT_UNDEFINED;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toAttachment.image = swapchain_.images[imageIndex];
    toAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U};
    VkDependencyInfo imageDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    imageDependency.imageMemoryBarrierCount = 1U;
    imageDependency.pImageMemoryBarriers = &toAttachment;
    vkCmdPipelineBarrier2(commandBuffer, &imageDependency);

    const std::array<VkShaderStageFlagBits, 2> graphicsStages{
        VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    const std::array<VkShaderEXT, 2> graphicsShaders{vertexShader_, fragmentShader_};
    vkCmdBindShadersEXT(commandBuffer, static_cast<std::uint32_t>(graphicsStages.size()),
                        graphicsStages.data(), graphicsShaders.data());
    const std::uint32_t descriptorIndex = 0U;
    const VkDeviceSize descriptorOffset = 0U;
    vkCmdSetDescriptorBufferOffsetsEXT(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       pipelineLayout_, 0U, 1U, &descriptorIndex, &descriptorOffset);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, usedShaderStages, 0U,
                       sizeof(PushConstants), &constants);
    setGraphicsState(commandBuffer);
    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = swapchain_.views[imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, swapchain_.extent};
    renderingInfo.layerCount = 1U;
    renderingInfo.colorAttachmentCount = 1U;
    renderingInfo.pColorAttachments = &colorAttachment;
    vkCmdBeginRendering(commandBuffer, &renderingInfo);
    vkCmdDraw(commandBuffer, 3U, 1U, 0U, 0U);
    vkCmdEndRendering(commandBuffer);
    VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    toPresent.dstAccessMask = 0U;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.image = swapchain_.images[imageIndex];
    toPresent.subresourceRange = toAttachment.subresourceRange;
    imageDependency.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(commandBuffer, &imageDependency);
    swapchain_.initialized[imageIndex] = true;
    vkCheck(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(frame)");
}

bool Application::drawFrame(const bool cameraMoved, const bool cameraCut)
{
    FrameSlot& slot = frameSlots_[frameSlotIndex_];
    if (slot.completionValue != 0U) {
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1U;
        waitInfo.pSemaphores = &timelineSemaphore_;
        waitInfo.pValues = &slot.completionValue;
        vkCheck(vkWaitSemaphores(device_, &waitInfo, std::numeric_limits<std::uint64_t>::max()),
                "vkWaitSemaphores(frame slot)");
    }
    if (validationFailure_.load(std::memory_order_relaxed)) {
        throw Error("Vulkan Validation Layer 报告了 warning/error 诊断");
    }

    std::uint32_t imageIndex = 0U;
    const VkResult acquire = vkAcquireNextImageKHR(device_, swapchain_.handle,
                                                   std::numeric_limits<std::uint64_t>::max(),
                                                   slot.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        rebuildForResize();
        return false;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        vkCheck(acquire, "vkAcquireNextImageKHR");
    }

    const std::uint32_t writeHistory = 1U - historyReadIndex_;
    writeDescriptorBuffer(slot, historyReadIndex_, writeHistory);
    updateFrameData(frameSlotIndex_, cameraMoved, cameraCut);
    vkCheck(vkResetCommandPool(device_, slot.commandPool, 0U), "vkResetCommandPool(frame)");
    recordFrame(slot.commandBuffer, frameSlotIndex_, imageIndex);

    std::array<VkSemaphoreSubmitInfo, 2> waitInfos{};
    std::uint32_t waitCount = 0U;
    if (timelineValue_ != 0U) {
        waitInfos[waitCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitInfos[waitCount].semaphore = timelineSemaphore_;
        waitInfos[waitCount].value = timelineValue_;
        waitInfos[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++waitCount;
    }
    waitInfos[waitCount] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfos[waitCount].semaphore = slot.imageAvailable;
    waitInfos[waitCount].value = 0U;
    waitInfos[waitCount].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    ++waitCount;
    ++timelineValue_;
    std::array<VkSemaphoreSubmitInfo, 2> signalInfos{};
    signalInfos[0] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfos[0].semaphore = timelineSemaphore_;
    signalInfos[0].value = timelineValue_;
    signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfos[1] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfos[1].semaphore = swapchain_.renderFinished[imageIndex];
    signalInfos[1].value = 0U;
    signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandInfo.commandBuffer = slot.commandBuffer;
    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount = waitCount;
    submitInfo.pWaitSemaphoreInfos = waitInfos.data();
    submitInfo.commandBufferInfoCount = 1U;
    submitInfo.pCommandBufferInfos = &commandInfo;
    submitInfo.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signalInfos.size());
    submitInfo.pSignalSemaphoreInfos = signalInfos.data();
    vkCheck(vkQueueSubmit2(queue_, 1U, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit2(frame)");
    slot.completionValue = timelineValue_;

    historyReadIndex_ = writeHistory;
    historyValid_ = true;
    previousCamera_ = camera_;
    previousJitter_ = currentJitter_;
    ++renderedFrameIndex_;
    frameSlotIndex_ = (frameSlotIndex_ + 1U) % frameSlotCount;

    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1U;
    presentInfo.pWaitSemaphores = &swapchain_.renderFinished[imageIndex];
    presentInfo.swapchainCount = 1U;
    presentInfo.pSwapchains = &swapchain_.handle;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult present = vkQueuePresentKHR(queue_, &presentInfo);
    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR ||
        acquire == VK_SUBOPTIMAL_KHR) {
        updateStatistics(false);
        rebuildForResize();
        return false;
    }
    vkCheck(present, "vkQueuePresentKHR");
    updateStatistics(true);
    return true;
}

void Application::processEvents(bool& running, bool& resizeRequested, bool& cameraCut,
                                bool& timingReset)
{
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            running = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            resizeRequested = true;
            timingReset = true;
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            minimized_ = true;
            timingReset = true;
            if (mouseCaptured_) {
                SDL_SetWindowRelativeMouseMode(window_, false);
                mouseCaptured_ = false;
            }
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            minimized_ = false;
            resizeRequested = true;
            timingReset = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            focused_ = true;
            timingReset = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            focused_ = false;
            timingReset = true;
            if (mouseCaptured_) {
                SDL_SetWindowRelativeMouseMode(window_, false);
                mouseCaptured_ = false;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_RIGHT && focused_ && !mouseCaptured_) {
                if (SDL_SetWindowRelativeMouseMode(window_, true)) {
                    mouseCaptured_ = true;
                    timingReset = true;
                }
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_RIGHT && mouseCaptured_) {
                SDL_SetWindowRelativeMouseMode(window_, false);
                mouseCaptured_ = false;
                timingReset = true;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (mouseCaptured_) {
                camera_.yaw += event.motion.xrel * 0.002F;
                camera_.pitch = clamp(camera_.pitch - event.motion.yrel * 0.002F,
                                      -89.0F * 3.14159265358979323846F / 180.0F,
                                      89.0F * 3.14159265358979323846F / 180.0F);
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_R) {
                camera_.reset();
                cameraCut = true;
                timingReset = true;
            } else if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) {
                if (mouseCaptured_) {
                    SDL_SetWindowRelativeMouseMode(window_, false);
                    mouseCaptured_ = false;
                    timingReset = true;
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

bool Application::updateCamera(const float deltaSeconds)
{
    if (!focused_ || minimized_ || deltaSeconds <= 0.0F) {
        return false;
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    Vec3 movement{};
    Vec3 horizontalForward = camera_.forward();
    horizontalForward.y = 0.0F;
    if (length(horizontalForward) > 0.0F) {
        horizontalForward = normalize(horizontalForward);
    }
    const Vec3 horizontalRight = camera_.right();
    if (keys[SDL_SCANCODE_W]) {
        movement = movement + horizontalForward;
    }
    if (keys[SDL_SCANCODE_S]) {
        movement = movement - horizontalForward;
    }
    if (keys[SDL_SCANCODE_D]) {
        movement = movement + horizontalRight;
    }
    if (keys[SDL_SCANCODE_A]) {
        movement = movement - horizontalRight;
    }
    if (keys[SDL_SCANCODE_E]) {
        movement.y += 1.0F;
    }
    if (keys[SDL_SCANCODE_Q]) {
        movement.y -= 1.0F;
    }
    if (length(movement) <= 0.0F) {
        return false;
    }
    const float multiplier = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 3.0F : 1.0F;
    camera_.position = camera_.position + normalize(movement) * (3.0F * multiplier * deltaSeconds);
    return true;
}

void Application::rebuildForResize()
{
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(window_, &width, &height) || width <= 0 || height <= 0) {
        minimized_ = true;
        return;
    }
    vkCheck(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(swapchain rebuild)");
    destroyDynamicResources();
    destroySwapchain();
    createSwapchain();
    createDynamicResources();
    previousCamera_ = camera_;
    previousJitter_ = {};
    currentJitter_ = {};
    historyValid_ = false;
    minimized_ = false;
    resetStatistics();
}

void Application::updateStatistics(const bool presented)
{
    if (!presented) {
        resetStatistics();
        return;
    }
    ++statisticsFrames_;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - statisticsStart_).count();
    if (seconds >= 0.5 && statisticsFrames_ > 0U) {
        const double fps = static_cast<double>(statisticsFrames_) / seconds;
        const double milliseconds = 1000.0 / fps;
        std::ostringstream title;
        title.setf(std::ios::fixed);
        title.precision(1);
        title << "VoxelPathTracer — " << fps << " FPS / " << milliseconds << " ms/frame";
        SDL_SetWindowTitle(window_, title.str().c_str());
        statisticsStart_ = now;
        statisticsFrames_ = 0U;
    }
}

void Application::resetStatistics()
{
    statisticsStart_ = std::chrono::steady_clock::now();
    statisticsFrames_ = 0U;
    if (window_ != nullptr) {
        SDL_SetWindowTitle(window_, "VoxelPathTracer — -- FPS / -- ms/frame");
    }
}

int Application::run()
{
    bool running = true;
    auto previousTime = std::chrono::steady_clock::now();
    bool paused = false;
    while (running) {
        const Camera before = camera_;
        bool resizeRequested = false;
        bool cameraCut = false;
        bool timingReset = false;
        processEvents(running, resizeRequested, cameraCut, timingReset);
        if (!running) {
            break;
        }
        int pixelWidth = 0;
        int pixelHeight = 0;
        const bool hasSize = SDL_GetWindowSizeInPixels(window_, &pixelWidth, &pixelHeight) &&
                             pixelWidth > 0 && pixelHeight > 0;
        if (!hasSize || minimized_) {
            if (!paused) {
                resetStatistics();
                paused = true;
            }
            previousTime = std::chrono::steady_clock::now();
            SDL_Delay(16U);
            continue;
        }
        if (paused) {
            paused = false;
            timingReset = true;
            resizeRequested = true;
        }
        if (resizeRequested &&
            (static_cast<std::uint32_t>(pixelWidth) != swapchain_.extent.width ||
             static_cast<std::uint32_t>(pixelHeight) != swapchain_.extent.height)) {
            rebuildForResize();
            timingReset = true;
            cameraCut = true;
        }
        const auto now = std::chrono::steady_clock::now();
        float deltaSeconds = timingReset
                                 ? 0.0F
                                 : static_cast<float>(std::chrono::duration<double>(now - previousTime).count());
        previousTime = now;
        deltaSeconds = std::min(deltaSeconds, 0.1F);
        const bool translated = updateCamera(deltaSeconds);
        const bool changedByEvent = before.yaw != camera_.yaw || before.pitch != camera_.pitch ||
                                    before.position.x != camera_.position.x ||
                                    before.position.y != camera_.position.y ||
                                    before.position.z != camera_.position.z;
        const bool cameraMoved = translated || changedByEvent || cameraCut;
        static_cast<void>(drawFrame(cameraMoved, cameraCut));
    }
    vkCheck(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle(exit)");
    if (validationFailure_.load(std::memory_order_relaxed)) {
        throw Error("运行期间 Vulkan Validation Layer 报告了 warning/error 诊断");
    }
    return 0;
}

void Application::destroyBuffer(Buffer& buffer)
{
    if (device_ == VK_NULL_HANDLE) {
        buffer = {};
        return;
    }
    if (buffer.mapped != nullptr && buffer.memory != VK_NULL_HANDLE) {
        vkUnmapMemory(device_, buffer.memory);
    }
    if (buffer.handle != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.handle, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
    }
    buffer = {};
}

void Application::destroyImage(Image& image)
{
    if (device_ == VK_NULL_HANDLE) {
        image = {};
        return;
    }
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
    }
    if (image.handle != VK_NULL_HANDLE) {
        vkDestroyImage(device_, image.handle, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, image.memory, nullptr);
    }
    image = {};
}

void Application::destroyAcceleration(AccelerationStructure& acceleration)
{
    if (device_ != VK_NULL_HANDLE && acceleration.handle != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device_, acceleration.handle, nullptr);
    }
    acceleration.handle = VK_NULL_HANDLE;
    destroyBuffer(acceleration.storage);
}

void Application::destroyDynamicResources()
{
    destroyBuffer(dynamic_.pathA);
    destroyBuffer(dynamic_.pathB);
    destroyBuffer(dynamic_.hits);
    destroyBuffer(dynamic_.counters);
    destroyBuffer(dynamic_.shadowTasks);
    destroyBuffer(dynamic_.shadowVisibility);
    destroyBuffer(dynamic_.branchResults);
    destroyBuffer(dynamic_.commonResults);
    destroyBuffer(dynamic_.analytic);
    destroyBuffer(dynamic_.restirDirect);
    destroyBuffer(dynamic_.reservoirInitial);
    destroyBuffer(dynamic_.reservoirTemporal);
    for (Buffer& buffer : dynamic_.reservoirHistory) {
        destroyBuffer(buffer);
    }
    destroyBuffer(dynamic_.rawSignals);
    destroyBuffer(dynamic_.albedo);
    for (Buffer& buffer : dynamic_.guides) {
        destroyBuffer(buffer);
    }
    for (Buffer& buffer : dynamic_.historyColor) {
        destroyBuffer(buffer);
    }
    for (Buffer& buffer : dynamic_.historyMoments) {
        destroyBuffer(buffer);
    }
    for (Buffer& buffer : dynamic_.historyLength) {
        destroyBuffer(buffer);
    }
    destroyBuffer(dynamic_.atrousA);
    destroyBuffer(dynamic_.atrousB);
    destroyBuffer(dynamic_.finalColor);
    dynamic_ = {};
}

void Application::destroySwapchain()
{
    if (device_ == VK_NULL_HANDLE) {
        swapchain_ = {};
        return;
    }
    for (const VkSemaphore semaphore : swapchain_.renderFinished) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }
    }
    for (const VkImageView view : swapchain_.views) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    if (swapchain_.handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_.handle, nullptr);
    }
    swapchain_ = {};
}

void Application::shutdown() noexcept
{
    if (device_ != VK_NULL_HANDLE) {
        static_cast<void>(vkDeviceWaitIdle(device_));
        destroyDynamicResources();
        destroySwapchain();
        for (FrameSlot& slot : frameSlots_) {
            destroyBuffer(slot.descriptorBuffer);
            if (slot.imageAvailable != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, slot.imageAvailable, nullptr);
            }
            if (slot.commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, slot.commandPool, nullptr);
            }
            slot = {};
        }
        if (timelineSemaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, timelineSemaphore_, nullptr);
            timelineSemaphore_ = VK_NULL_HANDLE;
        }
        for (VkShaderEXT& shader : computeShaders_) {
            if (shader != VK_NULL_HANDLE) {
                vkDestroyShaderEXT(device_, shader, nullptr);
                shader = VK_NULL_HANDLE;
            }
        }
        if (vertexShader_ != VK_NULL_HANDLE) {
            vkDestroyShaderEXT(device_, vertexShader_, nullptr);
            vertexShader_ = VK_NULL_HANDLE;
        }
        if (fragmentShader_ != VK_NULL_HANDLE) {
            vkDestroyShaderEXT(device_, fragmentShader_, nullptr);
            fragmentShader_ = VK_NULL_HANDLE;
        }
        destroyBuffer(sbt_.buffer);
        if (rayTracingPipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, rayTracingPipeline_, nullptr);
            rayTracingPipeline_ = VK_NULL_HANDLE;
        }
        destroyAcceleration(tlas_);
        for (auto& acceleration : blas_) {
            destroyAcceleration(acceleration);
        }
        blas_.clear();
        if (textureSampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, textureSampler_, nullptr);
            textureSampler_ = VK_NULL_HANDLE;
        }
        destroyImage(colorTexture_);
        destroyImage(roughnessTexture_);
        destroyBuffer(frameDataBuffer_);
        destroyBuffer(vertexBuffer_);
        destroyBuffer(indexBuffer_);
        destroyBuffer(triangleBuffer_);
        destroyBuffer(materialBuffer_);
        destroyBuffer(lightBuffer_);
        if (pipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            pipelineLayout_ = VK_NULL_HANDLE;
        }
        if (descriptorSetLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
            descriptorSetLayout_ = VK_NULL_HANDLE;
        }
        if (immediatePool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, immediatePool_, nullptr);
            immediatePool_ = VK_NULL_HANDLE;
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE && surface_ != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE && debugMessenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
    initialized_ = false;
}

} // namespace

int runApplication()
{
    try {
        Application application;
        return application.run();
    } catch (const std::exception& exception) {
        std::cerr << "VoxelPathTracer: " << exception.what() << '\n';
        return 1;
    }
}

} // namespace arena
