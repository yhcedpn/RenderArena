// 实例/设备/交换链与通用 Vulkan 辅助实现。
#pragma once

#include "rae_common.hpp"
#include "rae_scene.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rae {

// 设备能力查询/启用使用的完整 pNext 链。
struct FeatureBundle {
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT db{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceShaderObjectFeaturesEXT so{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds1{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vid{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};

    FeatureBundle() {
        f2.pNext = &v12;
        v12.pNext = &v13;
        v13.pNext = &as;
        as.pNext = &rt;
        rt.pNext = &db;
        db.pNext = &so;
        so.pNext = &eds1;
        eds1.pNext = &eds2;
        eds2.pNext = &vid;
        vid.pNext = &eds3;
        eds3.pNext = nullptr;
    }
};

inline std::vector<const char*> requiredDeviceExtensions() {
    return {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
        VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    };
}

inline void appendMissing(bool condition, const char* name, std::vector<std::string>& missing) {
    if (!condition) {
        missing.emplace_back(name);
    }
}

inline std::vector<std::string> missingFeatures(const FeatureBundle& f) {
    std::vector<std::string> missing;
    appendMissing(f.v12.timelineSemaphore == VK_TRUE, "timelineSemaphore", missing);
    appendMissing(f.v12.bufferDeviceAddress == VK_TRUE, "bufferDeviceAddress", missing);
    appendMissing(f.v12.scalarBlockLayout == VK_TRUE, "scalarBlockLayout", missing);
    appendMissing(f.v13.dynamicRendering == VK_TRUE, "dynamicRendering", missing);
    appendMissing(f.v13.synchronization2 == VK_TRUE, "synchronization2", missing);
    appendMissing(f.as.accelerationStructure == VK_TRUE, "accelerationStructure", missing);
    appendMissing(f.rt.rayTracingPipeline == VK_TRUE, "rayTracingPipeline", missing);
    appendMissing(f.rt.rayTracingPipelineTraceRaysIndirect == VK_TRUE,
                  "rayTracingPipelineTraceRaysIndirect", missing);
    appendMissing(f.db.descriptorBuffer == VK_TRUE, "descriptorBuffer", missing);
    appendMissing(f.so.shaderObject == VK_TRUE, "shaderObject", missing);
    appendMissing(f.eds1.extendedDynamicState == VK_TRUE, "extendedDynamicState", missing);
    appendMissing(f.eds2.extendedDynamicState2 == VK_TRUE, "extendedDynamicState2", missing);
    appendMissing(f.vid.vertexInputDynamicState == VK_TRUE, "vertexInputDynamicState", missing);
    appendMissing(f.eds3.extendedDynamicState3DepthClampEnable == VK_TRUE,
                  "extendedDynamicState3DepthClampEnable", missing);
    appendMissing(f.eds3.extendedDynamicState3PolygonMode == VK_TRUE,
                  "extendedDynamicState3PolygonMode", missing);
    appendMissing(f.eds3.extendedDynamicState3RasterizationSamples == VK_TRUE,
                  "extendedDynamicState3RasterizationSamples", missing);
    appendMissing(f.eds3.extendedDynamicState3SampleMask == VK_TRUE,
                  "extendedDynamicState3SampleMask", missing);
    appendMissing(f.eds3.extendedDynamicState3AlphaToCoverageEnable == VK_TRUE,
                  "extendedDynamicState3AlphaToCoverageEnable", missing);
    appendMissing(f.eds3.extendedDynamicState3LogicOpEnable == VK_TRUE,
                  "extendedDynamicState3LogicOpEnable", missing);
    appendMissing(f.eds3.extendedDynamicState3ColorBlendEnable == VK_TRUE,
                  "extendedDynamicState3ColorBlendEnable", missing);
    appendMissing(f.eds3.extendedDynamicState3ColorBlendEquation == VK_TRUE,
                  "extendedDynamicState3ColorBlendEquation", missing);
    appendMissing(f.eds3.extendedDynamicState3ColorWriteMask == VK_TRUE,
                  "extendedDynamicState3ColorWriteMask", missing);
    return missing;
}

inline void enableRequiredFeatures(FeatureBundle& f) {
    f.v12.timelineSemaphore = VK_TRUE;
    f.v12.bufferDeviceAddress = VK_TRUE;
    f.v12.scalarBlockLayout = VK_TRUE;
    f.v13.dynamicRendering = VK_TRUE;
    f.v13.synchronization2 = VK_TRUE;
    f.as.accelerationStructure = VK_TRUE;
    f.rt.rayTracingPipeline = VK_TRUE;
    f.rt.rayTracingPipelineTraceRaysIndirect = VK_TRUE;
    f.db.descriptorBuffer = VK_TRUE;
    f.so.shaderObject = VK_TRUE;
    f.eds1.extendedDynamicState = VK_TRUE;
    f.eds2.extendedDynamicState2 = VK_TRUE;
    f.vid.vertexInputDynamicState = VK_TRUE;
    f.eds3.extendedDynamicState3DepthClampEnable = VK_TRUE;
    f.eds3.extendedDynamicState3PolygonMode = VK_TRUE;
    f.eds3.extendedDynamicState3RasterizationSamples = VK_TRUE;
    f.eds3.extendedDynamicState3SampleMask = VK_TRUE;
    f.eds3.extendedDynamicState3AlphaToCoverageEnable = VK_TRUE;
    f.eds3.extendedDynamicState3LogicOpEnable = VK_TRUE;
    f.eds3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    f.eds3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    f.eds3.extendedDynamicState3ColorWriteMask = VK_TRUE;
}

inline VKAPI_ATTR VkBool32 VKAPI_CALL validationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const bool failing =
        (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0;
    const char* level = "INFO";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        level = "ERROR";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        level = "WARNING";
    }
    std::fprintf(stderr, "[validation %s] %s: %s\n", level,
                 data->pMessageIdName != nullptr ? data->pMessageIdName : "?",
                 data->pMessage != nullptr ? data->pMessage : "?");
    std::fflush(stderr);
    if (failing) {
        g_validationFailure.store(true);
    }
    return VK_FALSE;
}

inline std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::filesystem::current_path();
        }
        if (static_cast<size_t>(length) < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::vector<char> buffer(4096);
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return std::filesystem::current_path();
    }
    buffer[static_cast<size_t>(length)] = '\0';
    return std::filesystem::path(buffer.data()).parent_path();
#endif
}

inline std::string joinNames(const std::vector<const char*>& names) {
    std::string s;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            s += ", ";
        }
        s += names[i];
    }
    return s;
}

inline void logMissing(const char* deviceName, const std::vector<std::string>& missing) {
    if (missing.empty()) {
        return;
    }
    std::fprintf(stderr, "设备 %s 缺少必需能力:\n", deviceName);
    for (const std::string& m : missing) {
        std::fprintf(stderr, "  - %s\n", m.c_str());
    }
}

inline int Renderer::run() {
    if (!init()) {
        shutdown();
        return EXIT_FAILURE;
    }
    runLoop();
    shutdown();
    if (g_validationFailure.load() || fatalError_) {
        std::fprintf(stderr, "程序以失败状态退出（可能存在验证层或运行期诊断）。\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

inline bool Renderer::init() {
    exeDir_ = executableDirectory();
    shadersDir_ = exeDir_ / "shaders";
    const std::filesystem::path materialsPath = exeDir_ / "materials.json";
    if (!scene_.load(materialsPath)) {
        std::fprintf(stderr, "场景/材质初始化失败: %s\n", scene_.error.c_str());
        return false;
    }
    if (!initWindow()) {
        return false;
    }
    if (!initVulkan()) {
        return false;
    }
    if (!createSwapchain()) {
        return false;
    }
    if (!createStaticResources()) {
        return false;
    }
    if (!buildAccelerationStructures()) {
        return false;
    }
    if (!compileShaders()) {
        return false;
    }
    if (!createPasses()) {
        return false;
    }
    if (!createRayTracingPipeline()) {
        return false;
    }
    if (!createRenderResources()) {
        return false;
    }
    if (!createFrameResources()) {
        return false;
    }
    resetCamera();
    historyReset_ = true;
    prevCameraValid_ = false;
    lastInputTime_ = glfwGetTime();
    resetStats();
    updateTitle(true);
    initialized_ = true;
    return true;
}

inline bool Renderer::initWindow() {
#ifdef _WIN32
    // 让 1280x720 客户区按物理像素创建，避免高 DPI 下被系统缩放。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
    glfwSetErrorCallback([](int code, const char* description) {
        std::fprintf(stderr, "GLFW 错误 %d: %s\n", code, description != nullptr ? description : "?");
    });
    if (glfwInit() != GLFW_TRUE) {
        std::fprintf(stderr, "glfwInit 失败\n");
        return false;
    }
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(1280, 720, "MiniVoxelPathTracer", nullptr, nullptr);
    if (window_ == nullptr) {
        std::fprintf(stderr, "创建 GLFW 窗口失败\n");
        return false;
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferSizeCallback);
    glfwSetKeyCallback(window_, keyCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
    glfwSetCursorPosCallback(window_, cursorPosCallback);
    glfwSetWindowFocusCallback(window_, focusCallback);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
    return true;
}

inline bool Renderer::initVulkan() {
    if (volkInitialize() != VK_SUCCESS) {
        std::fprintf(stderr, "volkInitialize 失败\n");
        return false;
    }
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    const auto enumerateVersion =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerateVersion != nullptr) {
        if (enumerateVersion(&loaderVersion) != VK_SUCCESS ||
            VK_API_VERSION_MAJOR(loaderVersion) < 1 ||
            (VK_API_VERSION_MAJOR(loaderVersion) == 1 && VK_API_VERSION_MINOR(loaderVersion) < 4)) {
            std::fprintf(stderr, "Vulkan loader 版本不足 1.4: %u.%u.%u\n",
                         VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion),
                         VK_API_VERSION_PATCH(loaderVersion));
            return false;
        }
    } else {
        std::fprintf(stderr, "无法查询 Vulkan loader 版本\n");
        return false;
    }
    if (!createInstance()) {
        return false;
    }
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 Vulkan surface 失败\n");
        return false;
    }
    if (!pickPhysicalDevice()) {
        return false;
    }
    if (!createDevice()) {
        return false;
    }
    if (!createAllocator()) {
        return false;
    }
    if (!createTempResources()) {
        return false;
    }
    return true;
}

inline bool Renderer::createInstance() {
    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
        std::fprintf(stderr, "GLFW 未返回所需的 Vulkan surface 扩展\n");
        return false;
    }
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
#ifndef NDEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    uint32_t extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, availableExtensions.data());
    std::set<std::string> available;
    for (const VkExtensionProperties& e : availableExtensions) {
        available.emplace(e.extensionName);
    }
    for (const char* name : extensions) {
        if (available.count(name) == 0) {
            std::fprintf(stderr, "缺少 Vulkan 实例扩展: %s\n", name);
            return false;
        }
    }

    std::vector<const char*> layers;
#ifndef NDEBUG
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layerProps(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layerProps.data());
    bool haveValidation = false;
    for (const VkLayerProperties& l : layerProps) {
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            haveValidation = true;
            break;
        }
    }
    if (!haveValidation) {
        std::fprintf(stderr, "Debug 构建需要 VK_LAYER_KHRONOS_validation，但该层不可用\n");
        return false;
    }
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MiniVoxelPathTracer";
    app.applicationVersion = 1;
    app.pEngineName = "MiniVoxelPathTracer";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_4;

    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    create.enabledLayerCount = static_cast<uint32_t>(layers.size());
    create.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
#ifndef NDEBUG
    const VkValidationFeatureEnableEXT enabled[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = enabled;
    VkDebugUtilsMessengerCreateInfoEXT debugCreate{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debugCreate.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    debugCreate.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    debugCreate.pfnUserCallback = validationCallback;
    validation.pNext = &debugCreate;
    create.pNext = &validation;
#endif
    if (vkCreateInstance(&create, nullptr, &instance_) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateInstance 失败\n");
        return false;
    }
    volkLoadInstance(instance_);
#ifndef NDEBUG
    if (vkCreateDebugUtilsMessengerEXT(instance_, &debugCreate, nullptr, &debugMessenger_) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "创建 debug messenger 失败\n");
        return false;
    }
#endif
    return true;
}

inline bool Renderer::pickPhysicalDevice() {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || count == 0) {
        std::fprintf(stderr, "没有可用的 Vulkan 物理设备\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    const std::vector<const char*> requiredExt = requiredDeviceExtensions();
    int bestScore = -1;
    VkPhysicalDevice best = VK_NULL_HANDLE;
    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        std::vector<std::string> missing;
        if (VK_API_VERSION_MAJOR(props.apiVersion) < 1 ||
            (VK_API_VERSION_MAJOR(props.apiVersion) == 1 &&
             VK_API_VERSION_MINOR(props.apiVersion) < 4)) {
            missing.emplace_back("Vulkan 1.4 instance API");
        }
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> extProps(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, extProps.data());
        std::set<std::string> haveExt;
        for (const VkExtensionProperties& e : extProps) {
            haveExt.emplace(e.extensionName);
        }
        for (const char* name : requiredExt) {
            if (haveExt.count(name) == 0) {
                missing.emplace_back(std::string("扩展 ") + name);
            }
        }
        if (!missing.empty()) {
            logMissing(props.deviceName, missing);
            continue;
        }
        FeatureBundle features;
        vkGetPhysicalDeviceFeatures2(dev, &features.f2);
        for (const std::string& m : missingFeatures(features)) {
            missing.push_back(m);
        }
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceDescriptorBufferPropertiesEXT dbProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        props2.pNext = &rtProps;
        rtProps.pNext = &asProps;
        asProps.pNext = &dbProps;
        vkGetPhysicalDeviceProperties2(dev, &props2);
        if (props.limits.maxComputeWorkGroupInvocations < kThreadsPerGroup) {
            missing.emplace_back("maxComputeWorkGroupInvocations >= 64");
        }
        if (props.limits.maxComputeWorkGroupCount[0] == 0 ||
            props.limits.maxComputeWorkGroupCount[1] == 0 ||
            props.limits.maxComputeWorkGroupCount[2] == 0) {
            missing.emplace_back("maxComputeWorkGroupCount");
        }
        if (rtProps.shaderGroupHandleSize == 0 || rtProps.shaderGroupBaseAlignment == 0 ||
            rtProps.shaderGroupHandleAlignment == 0 ||
            rtProps.maxShaderGroupStride < rtProps.shaderGroupHandleSize) {
            missing.emplace_back("ray tracing shader group properties");
        }
        if (asProps.minAccelerationStructureScratchOffsetAlignment == 0) {
            missing.emplace_back("acceleration structure scratch alignment");
        }
        if (dbProps.descriptorBufferOffsetAlignment == 0 ||
            dbProps.storageBufferDescriptorSize == 0 ||
            dbProps.combinedImageSamplerDescriptorSize == 0 ||
            dbProps.storageImageDescriptorSize == 0 ||
            dbProps.accelerationStructureDescriptorSize == 0 ||
            dbProps.maxDescriptorBufferBindings < 1) {
            missing.emplace_back("descriptor buffer properties");
        }
        if (!missing.empty()) {
            logMissing(props.deviceName, missing);
            continue;
        }
        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score = 2;
        } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            score = 1;
        }
        if (score > bestScore) {
            bestScore = score;
            best = dev;
            deviceProps_ = props;
            limits_ = props.limits;
            rtProps_ = rtProps;
            asProps_ = asProps;
            dbProps_ = dbProps;
        }
    }
    if (best == VK_NULL_HANDLE) {
        std::fprintf(stderr, "没有满足 Vulkan 1.4 + 硬件 RT + descriptor buffer + shader object 的设备\n");
        return false;
    }
    physicalDevice_ = best;
    maxGroupsX_ = std::max(1u, std::min(limits_.maxComputeWorkGroupCount[0], 65535u));
    maxGroupsY_ = std::max(1u, std::min(limits_.maxComputeWorkGroupCount[1], 65535u));
    maxGroupsZ_ = std::max(1u, std::min(limits_.maxComputeWorkGroupCount[2], 65535u));
    std::fprintf(stderr,
                 "使用设备: %s (API %u.%u.%u, descriptor offset alignment %llu, SBT base alignment %u)\n",
                 deviceProps_.deviceName, VK_API_VERSION_MAJOR(deviceProps_.apiVersion),
                 VK_API_VERSION_MINOR(deviceProps_.apiVersion),
                 VK_API_VERSION_PATCH(deviceProps_.apiVersion),
                 static_cast<unsigned long long>(dbProps_.descriptorBufferOffsetAlignment),
                 rtProps_.shaderGroupBaseAlignment);
    return true;
}

inline bool Renderer::createDevice() {
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
    queueFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_, &present);
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0 && (flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
            (flags & VK_QUEUE_TRANSFER_BIT) != 0 && present == VK_TRUE) {
            queueFamily_ = i;
            break;
        }
    }
    if (queueFamily_ == UINT32_MAX) {
        std::fprintf(stderr, "找不到同时支持 graphics/compute/transfer/present 的队列族\n");
        return false;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    FeatureBundle features;
    enableRequiredFeatures(features);

    const std::vector<const char*> extensions = requiredDeviceExtensions();
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queueInfo;
    create.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    create.pNext = &features.f2;
    if (vkCreateDevice(physicalDevice_, &create, nullptr, &device_) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateDevice 失败\n");
        return false;
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

inline bool Renderer::createAllocator() {
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo info{};
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    info.physicalDevice = physicalDevice_;
    info.device = device_;
    info.instance = instance_;
    info.vulkanApiVersion = VK_API_VERSION_1_4;
    info.pVulkanFunctions = &functions;
    if (vmaCreateAllocator(&info, &allocator_) != VK_SUCCESS) {
        std::fprintf(stderr, "vmaCreateAllocator 失败\n");
        return false;
    }
    return true;
}

inline bool Renderer::createTempResources() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &tempPool_) != VK_SUCCESS) {
        std::fprintf(stderr, "创建临时 command pool 失败\n");
        return false;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fenceInfo, nullptr, &tempFence_) != VK_SUCCESS) {
        std::fprintf(stderr, "创建临时 fence 失败\n");
        return false;
    }
    return true;
}

inline bool Renderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps) != VK_SUCCESS) {
        std::fprintf(stderr, "查询 surface capabilities 失败\n");
        return false;
    }
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
    bool found = false;
    for (const VkSurfaceFormatKHR& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            if (!found || f.format == VK_FORMAT_B8G8R8A8_SRGB) {
                swapchainFormat_ = f.format;
                swapchainColorSpace_ = f.colorSpace;
                found = true;
            }
        }
    }
    if (!found) {
        std::fprintf(stderr, "找不到 sRGB swapchain 格式 (B8G8R8A8_SRGB/R8G8B8A8_SRGB + SRGB_NONLINEAR)\n");
        return false;
    }
    uint32_t presentCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentCount,
                                              presentModes.data());
    bool haveFifo = false;
    for (VkPresentModeKHR mode : presentModes) {
        if (mode == VK_PRESENT_MODE_FIFO_KHR) {
            haveFifo = true;
        }
    }
    if (!haveFifo) {
        std::fprintf(stderr, "swapchain 不支持 FIFO present mode\n");
        return false;
    }

    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(window_, &fbWidth, &fbHeight);
    VkExtent2D extent{};
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width = static_cast<uint32_t>(std::max(fbWidth, 1));
        extent.height = static_cast<uint32_t>(std::max(fbHeight, 1));
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height =
            std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        std::fprintf(stderr, "swapchain extent 为 0\n");
        return false;
    }
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }
    VkCompositeAlphaFlagBitsKHR composite = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0) {
        if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) != 0) {
            composite = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        } else if ((caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0) {
            composite = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        } else {
            composite = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
        }
    }
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create.surface = surface_;
    create.minImageCount = imageCount;
    create.imageFormat = swapchainFormat_;
    create.imageColorSpace = swapchainColorSpace_;
    create.imageExtent = extent;
    create.imageArrayLayers = 1;
    create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.preTransform = caps.currentTransform;
    create.compositeAlpha = composite;
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    create.clipped = VK_TRUE;
    create.oldSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device_, &create, nullptr, &swapchain_) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateSwapchainKHR 失败\n");
        return false;
    }
    extent_ = extent;
    if (!createSwapchainViews()) {
        return false;
    }
    swapchainSuboptimal_ = false;
    return true;
}

inline bool Renderer::createSwapchainViews() {
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapImages_.data());
    swapViews_.resize(imageCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = swapImages_[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = swapchainFormat_;
        view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &swapViews_[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "创建 swapchain image view 失败\n");
            return false;
        }
    }
    renderFinished_.resize(imageCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < imageCount; ++i) {
        if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinished_[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "创建 renderFinished semaphore 失败\n");
            return false;
        }
    }
    return true;
}

inline void Renderer::destroySwapchain() {
    for (VkSemaphore s : renderFinished_) {
        if (s != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, s, nullptr);
        }
    }
    renderFinished_.clear();
    for (VkImageView view : swapViews_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_, view, nullptr);
        }
    }
    swapViews_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
    swapImages_.clear();
}

inline Buffer Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                                     VkDeviceSize alignment, bool preferDevice) {
    // 该 VMA 版本没有 allocation alignment 字段；需要对齐的调用方自行分配额外空间并按地址取偏移。
    (void)alignment;
    Buffer b;
    if (size == 0) {
        size = 16;
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = preferDevice ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo allocInfo{};
    const VkResult result = vmaCreateBuffer(allocator_, &info, &alloc, &b.buffer, &b.allocation,
                                            &allocInfo);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "vmaCreateBuffer 失败 (%s, size=%llu)\n",
                     result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "OOM" : "error",
                     static_cast<unsigned long long>(size));
        return b;
    }
    b.size = size;
    if (hostVisible) {
        b.mapped = allocInfo.pMappedData;
        if (b.mapped == nullptr) {
            if (vmaMapMemory(allocator_, b.allocation, &b.mapped) != VK_SUCCESS) {
                b.mapped = nullptr;
            }
        }
    }
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device_, &addressInfo);
    }
    return b;
}

inline void Renderer::destroyBuffer(Buffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        // host-visible 缓冲使用 MAPPED_BIT 持久映射，销毁由 VMA 处理，不能再显式 unmap。
        buffer.mapped = nullptr;
        vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
        buffer = {};
    }
}

inline Image Renderer::createImage2D(VkFormat format, VkExtent2D extent, uint32_t mipLevels,
                                     uint32_t layers, VkImageUsageFlags usage,
                                     VkImageAspectFlags aspect, VkImageViewType viewType) {
    Image img;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = mipLevels;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator_, &info, &alloc, &img.image, &img.allocation, nullptr) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "vmaCreateImage 失败 (%ux%u)\n", extent.width, extent.height);
        return img;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = img.image;
    view.viewType = viewType;
    view.format = format;
    view.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    view.subresourceRange = {aspect, 0, mipLevels, 0, layers};
    if (vkCreateImageView(device_, &view, nullptr, &img.view) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 image view 失败\n");
        vmaDestroyImage(allocator_, img.image, img.allocation);
        img = {};
        return img;
    }
    img.extent = extent;
    img.format = format;
    img.mipLevels = mipLevels;
    img.layers = layers;
    return img;
}

inline void Renderer::destroyImage(Image& image) {
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, image.image, image.allocation);
        image.image = VK_NULL_HANDLE;
        image.allocation = VK_NULL_HANDLE;
    }
}

inline VkCommandBuffer Renderer::beginTempCommand() {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = tempPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &alloc, &cmd);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

inline void Renderer::endTempCommand(VkCommandBuffer cmd, bool wait) {
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cbInfo;
    if (wait) {
        vkResetFences(device_, 1, &tempFence_);
        submit.signalSemaphoreInfoCount = 0;
        if (vkQueueSubmit2(queue_, 1, &submit, tempFence_) != VK_SUCCESS) {
            std::fprintf(stderr, "临时提交失败\n");
            fatalError_ = true;
        }
        if (vkWaitForFences(device_, 1, &tempFence_, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            std::fprintf(stderr, "等待临时提交失败\n");
            fatalError_ = true;
        }
        vkResetFences(device_, 1, &tempFence_);
    } else {
        submit.signalSemaphoreInfoCount = 0;
        if (vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
            std::fprintf(stderr, "临时提交失败\n");
            fatalError_ = true;
        }
    }
    vkFreeCommandBuffers(device_, tempPool_, 1, &cmd);
}

inline void Renderer::uploadBuffer(Buffer& dst, const void* data, VkDeviceSize size) {
    if (dst.buffer == VK_NULL_HANDLE || data == nullptr || size == 0) {
        return;
    }
    if (dst.mapped != nullptr) {
        std::memcpy(dst.mapped, data, static_cast<size_t>(size));
        vmaFlushAllocation(allocator_, dst.allocation, 0, VK_WHOLE_SIZE);
        return;
    }
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, 0, false);
    if (staging.buffer == VK_NULL_HANDLE || staging.mapped == nullptr) {
        std::fprintf(stderr, "上传 staging buffer 创建失败\n");
        fatalError_ = true;
        return;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    vmaFlushAllocation(allocator_, staging.allocation, 0, VK_WHOLE_SIZE);
    VkCommandBuffer cmd = beginTempCommand();
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &copy);
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    endTempCommand(cmd, true);
    destroyBuffer(staging);
}

inline void Renderer::destroyAccel(Accel& accel) {
    if (accel.handle != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(device_, accel.handle, nullptr);
        accel.handle = VK_NULL_HANDLE;
    }
    destroyBuffer(accel.buffer);
}

inline void Renderer::transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                                      VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                                      VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                                      VkAccessFlags2 dstAccess, uint32_t mipLevels,
                                      uint32_t layers) {
    VkImageMemoryBarrier2 barrierInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrierInfo.srcStageMask = srcStage;
    barrierInfo.srcAccessMask = srcAccess;
    barrierInfo.dstStageMask = dstStage;
    barrierInfo.dstAccessMask = dstAccess;
    barrierInfo.oldLayout = oldLayout;
    barrierInfo.newLayout = newLayout;
    barrierInfo.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierInfo.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierInfo.image = image;
    barrierInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrierInfo;
    vkCmdPipelineBarrier2(cmd, &dep);
}

inline void Renderer::barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage,
                              VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                              VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mem{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mem.srcStageMask = srcStage;
    mem.srcAccessMask = srcAccess;
    mem.dstStageMask = dstStage;
    mem.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mem;
    vkCmdPipelineBarrier2(cmd, &dep);
}

inline VkShaderModule Renderer::createShaderModule(const std::vector<uint32_t>& spv) {
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spv.size() * sizeof(uint32_t);
    info.pCode = spv.data();
    if (vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateShaderModule 失败\n");
        return VK_NULL_HANDLE;
    }
    return module;
}

// shaderc include 回调：所有 include 都从可执行文件旁的 shaders 目录读取。
struct ShaderIncludeBox {
    shaderc_include_result result{};
    std::string name;
    std::string content;
};

inline shaderc_include_result* shaderIncludeResolve(void* userData, const char* requested,
                                                    int, const char*, size_t) {
    const auto* dir = static_cast<const std::filesystem::path*>(userData);
    auto* box = new ShaderIncludeBox();
    box->name = requested;
    const std::filesystem::path path = *dir / requested;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        box->content = "无法打开 include 文件: " + path.string();
    } else {
        std::ostringstream ss;
        ss << file.rdbuf();
        box->content = ss.str();
    }
    box->result.source_name = box->name.c_str();
    box->result.source_name_length = box->name.size();
    box->result.content = box->content.c_str();
    box->result.content_length = box->content.size();
    box->result.user_data = box;
    return &box->result;
}

inline void shaderIncludeRelease(void*, shaderc_include_result* result) {
    delete reinterpret_cast<ShaderIncludeBox*>(result);
}

inline std::vector<uint32_t> Renderer::compileShader(const std::string& file,
                                                     shaderc_shader_kind stage) {
    const std::filesystem::path path = shadersDir_ / file;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "无法读取 shader: %s\n", path.string().c_str());
        return {};
    }
    std::ostringstream ss;
    ss << input.rdbuf();
    const std::string source = ss.str();

    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    if (compiler == nullptr || options == nullptr) {
        std::fprintf(stderr, "初始化 shaderc 失败\n");
        if (compiler != nullptr) {
            shaderc_compiler_release(compiler);
        }
        if (options != nullptr) {
            shaderc_compile_options_release(options);
        }
        return {};
    }
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_6);
    shaderc_compile_options_set_warnings_as_errors(options);
#ifndef NDEBUG
    shaderc_compile_options_set_generate_debug_info(options);
    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_zero);
#else
    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);
#endif
    shaderc_compile_options_set_include_callbacks(options, shaderIncludeResolve,
                                                  shaderIncludeRelease, &shadersDir_);
    shaderc_compilation_result_t result = shaderc_compile_into_spv(
        compiler, source.c_str(), source.size(), stage, file.c_str(), "main", options);
    if (result == nullptr) {
        std::fprintf(stderr, "shaderc_compile_into_spv 返回空结果: %s\n", file.c_str());
        shaderc_compile_options_release(options);
        shaderc_compiler_release(compiler);
        return {};
    }
    std::vector<uint32_t> spv;
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
        std::fprintf(stderr, "Shader 编译失败 (%s):\n%s\n", file.c_str(),
                     shaderc_result_get_error_message(result));
    } else {
        const size_t length = shaderc_result_get_length(result);
        const char* bytes = shaderc_result_get_bytes(result);
        if (length == 0 || bytes == nullptr || (length % sizeof(uint32_t)) != 0) {
            std::fprintf(stderr, "Shader 编译结果无效: %s\n", file.c_str());
        } else {
            spv.resize(length / sizeof(uint32_t));
            std::memcpy(spv.data(), bytes, length);
        }
    }
    shaderc_result_release(result);
    shaderc_compile_options_release(options);
    shaderc_compiler_release(compiler);
    return spv;
}

} // namespace rae
