#define VMA_IMPLEMENTATION
#include "vk_context.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vpt {

std::atomic<bool> g_validationFailure{false};

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData) {
    (void)userData;
    const bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool isWarning = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
    const char* level = isError ? "ERROR" : (isWarning ? "WARNING" : "INFO");
    const char* typeName =
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0 ? "validation" :
        ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0 ? "performance" :
         ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) != 0 ? "general" : "unknown"));
    std::fprintf(stderr, "[vk][%s][%s] %s\n", level, typeName,
                 data != nullptr && data->pMessage != nullptr ? data->pMessage : "(no message)");
    if (isError || isWarning) {
        g_validationFailure.store(true, std::memory_order_relaxed);
    }
    return VK_FALSE;
}

void appendExtension(std::vector<const char*>& extensions, const char* name) {
    for (const char* e : extensions) {
        if (std::strcmp(e, name) == 0) return;
    }
    extensions.push_back(name);
}

bool hasDeviceExtension(const std::vector<VkExtensionProperties>& available, const char* name) {
    for (const VkExtensionProperties& e : available) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

bool hasLayer(const std::vector<VkLayerProperties>& layers, const char* name) {
    for (const VkLayerProperties& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

} // namespace

std::string VulkanContext::deviceName() const {
    return properties.deviceName;
}

bool VulkanContext::init(SDL_Window* window, bool debugBuild) {
    validationEnabled = debugBuild;

    if (volkInitialize() != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: volkInitialize 失败，无法加载 Vulkan loader\n");
        return false;
    }

    std::uint32_t loaderVersion = 0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 无法查询 loader 版本\n");
        return false;
    }
    if (VK_API_VERSION_MAJOR(loaderVersion) < 1 ||
        (VK_API_VERSION_MAJOR(loaderVersion) == 1 && VK_API_VERSION_MINOR(loaderVersion) < 4)) {
        std::fprintf(stderr, "Vulkan 错误: loader 版本 %u.%u 低于要求的 1.4\n",
                     VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion));
        return false;
    }

    std::vector<const char*> instanceLayers;
    if (debugBuild) {
        std::uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        if (layerCount > 0) vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        if (!hasLayer(layers, "VK_LAYER_KHRONOS_validation")) {
            std::fprintf(stderr, "Vulkan 错误: Debug 构建需要 VK_LAYER_KHRONOS_validation，但不可用\n");
            return false;
        }
        instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
    }

    std::uint32_t sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (sdlExtensions == nullptr) {
        std::fprintf(stderr, "SDL 错误: 无法获得 Vulkan 实例扩展: %s\n", SDL_GetError());
        return false;
    }
    std::vector<const char*> instanceExtensions;
    for (std::uint32_t i = 0; i < sdlExtensionCount; ++i) {
        appendExtension(instanceExtensions, sdlExtensions[i]);
    }
    if (debugBuild) appendExtension(instanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "VoxelPathTracer";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "VoxelPathTracer";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_4;

    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    VkValidationFeatureEnableEXT enabledFeatures[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    validationFeatures.enabledValidationFeatureCount = debugBuild ? 1u : 0u;
    validationFeatures.pEnabledValidationFeatures = debugBuild ? enabledFeatures : nullptr;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pNext = debugBuild ? &validationFeatures : nullptr;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledLayerCount = static_cast<std::uint32_t>(instanceLayers.size());
    instanceInfo.ppEnabledLayerNames = instanceLayers.empty() ? nullptr : instanceLayers.data();
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: vkCreateInstance 失败\n");
        return false;
    }
    volkLoadInstance(instance);

    if (debugBuild) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
        messengerInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = debugCallback;
        if (vkCreateDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &messenger) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan 错误: 创建 debug messenger 失败\n");
            return false;
        }
    }

    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        std::fprintf(stderr, "SDL/Vulkan 错误: 创建窗口 surface 失败: %s\n", SDL_GetError());
        return false;
    }

    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::fprintf(stderr, "Vulkan 错误: 没有可用物理设备\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    const char* requiredExtensions[] = {
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

    bool found = false;
    for (VkPhysicalDevice candidate : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        if (props.apiVersion < VK_API_VERSION_1_4) {
            std::fprintf(stderr, "Vulkan 信息: 跳过 %s (apiVersion %u.%u < 1.4)\n",
                         props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
                         VK_API_VERSION_MINOR(props.apiVersion));
            continue;
        }

        std::uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        if (extCount > 0) {
            vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, exts.data());
        }
        std::vector<const char*> missing;
        for (const char* req : requiredExtensions) {
            if (!hasDeviceExtension(exts, req)) missing.push_back(req);
        }
        if (!missing.empty()) {
            std::fprintf(stderr, "Vulkan 信息: 设备 %s 缺少扩展:", props.deviceName);
            for (const char* m : missing) std::fprintf(stderr, " %s", m);
            std::fprintf(stderr, "\n");
            continue;
        }

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        std::uint32_t candidateGraphics = UINT32_MAX;
        std::uint32_t candidatePresent = UINT32_MAX;
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                candidateGraphics == UINT32_MAX) {
                candidateGraphics = i;
            }
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
            if (present == VK_TRUE && candidatePresent == UINT32_MAX) {
                candidatePresent = i;
            }
        }
        if (candidateGraphics == UINT32_MAX || candidatePresent == UINT32_MAX) {
            std::fprintf(stderr, "Vulkan 信息: 设备 %s 缺少 graphics/present 队列\n",
                         props.deviceName);
            continue;
        }

        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceDescriptorBufferFeaturesEXT dbFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
        VkPhysicalDeviceShaderObjectFeaturesEXT soFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT ds1{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT ds2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT ds3{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vid{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
        f11.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = &f14;
        f14.pNext = &asFeatures;
        asFeatures.pNext = &rtFeatures;
        rtFeatures.pNext = &dbFeatures;
        dbFeatures.pNext = &soFeatures;
        soFeatures.pNext = &ds1;
        ds1.pNext = &ds2;
        ds2.pNext = &ds3;
        ds3.pNext = &vid;
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &f11;
        vkGetPhysicalDeviceFeatures2(candidate, &features2);

        bool ok = f12.timelineSemaphore == VK_TRUE && f12.bufferDeviceAddress == VK_TRUE &&
                  f12.scalarBlockLayout == VK_TRUE && f13.dynamicRendering == VK_TRUE &&
                  f13.synchronization2 == VK_TRUE && asFeatures.accelerationStructure == VK_TRUE &&
                  rtFeatures.rayTracingPipeline == VK_TRUE &&
                  rtFeatures.rayTracingPipelineTraceRaysIndirect == VK_TRUE &&
                  dbFeatures.descriptorBuffer == VK_TRUE && soFeatures.shaderObject == VK_TRUE &&
                  ds1.extendedDynamicState == VK_TRUE && ds2.extendedDynamicState2 == VK_TRUE &&
                  ds3.extendedDynamicState3ColorBlendEnable == VK_TRUE &&
                  ds3.extendedDynamicState3ColorBlendEquation == VK_TRUE &&
                  ds3.extendedDynamicState3ColorWriteMask == VK_TRUE &&
                  ds3.extendedDynamicState3AlphaToCoverageEnable == VK_TRUE &&
                  ds3.extendedDynamicState3SampleMask == VK_TRUE &&
                  ds3.extendedDynamicState3RasterizationSamples == VK_TRUE &&
                  ds3.extendedDynamicState3DepthClampEnable == VK_TRUE &&
                  ds3.extendedDynamicState3LogicOpEnable == VK_TRUE &&
                  vid.vertexInputDynamicState == VK_TRUE;
        if (!ok) {
            std::fprintf(stderr, "Vulkan 信息: 设备 %s 缺少必需 feature\n", props.deviceName);
            continue;
        }

        physicalDevice = candidate;
        graphicsFamily = candidateGraphics;
        presentFamily = candidatePresent;
        properties = props;
        limits = props.limits;

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceDescriptorBufferPropertiesEXT dbProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
        rtProps.pNext = &asProps;
        asProps.pNext = &dbProps;
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &rtProps;
        vkGetPhysicalDeviceProperties2(candidate, &props2);
        rayTracingProperties = rtProps;
        accelerationStructureProperties = asProps;
        descriptorBufferProperties = dbProps;

        found = true;
        break;
    }

    if (!found) {
        std::fprintf(stderr, "Vulkan 错误: 没有满足 Vulkan 1.4 及 RT/DescriptorBuffer/ShaderObject "
                             "能力要求的物理设备\n");
        return false;
    }

    // 重新查询并启用所需 feature。
    VkPhysicalDeviceVulkan11Features enable11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features enable12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features enable13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan14Features enable14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enableAs{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR enableRt{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT enableDb{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceShaderObjectFeaturesEXT enableSo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT enableDs1{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT enableDs2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT enableDs3{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT enableVid{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    enable11.pNext = &enable12;
    enable12.pNext = &enable13;
    enable13.pNext = &enable14;
    enable14.pNext = &enableAs;
    enableAs.pNext = &enableRt;
    enableRt.pNext = &enableDb;
    enableDb.pNext = &enableSo;
    enableSo.pNext = &enableDs1;
    enableDs1.pNext = &enableDs2;
    enableDs2.pNext = &enableDs3;
    enableDs3.pNext = &enableVid;
    VkPhysicalDeviceFeatures2 enableFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enableFeatures.pNext = &enable11;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &enableFeatures);

    enable12.timelineSemaphore = VK_TRUE;
    enable12.bufferDeviceAddress = VK_TRUE;
    enable12.scalarBlockLayout = VK_TRUE;
    enable13.dynamicRendering = VK_TRUE;
    enable13.synchronization2 = VK_TRUE;
    enableAs.accelerationStructure = VK_TRUE;
    enableRt.rayTracingPipeline = VK_TRUE;
    enableRt.rayTracingPipelineTraceRaysIndirect = VK_TRUE;
    enableDb.descriptorBuffer = VK_TRUE;
    enableSo.shaderObject = VK_TRUE;
    enableDs1.extendedDynamicState = VK_TRUE;
    enableDs2.extendedDynamicState2 = VK_TRUE;
    enableDs3.extendedDynamicState3ColorBlendEnable = VK_TRUE;
    enableDs3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    enableDs3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    enableDs3.extendedDynamicState3AlphaToCoverageEnable = VK_TRUE;
    enableDs3.extendedDynamicState3SampleMask = VK_TRUE;
    enableDs3.extendedDynamicState3RasterizationSamples = VK_TRUE;
    enableDs3.extendedDynamicState3DepthClampEnable = VK_TRUE;
    enableDs3.extendedDynamicState3LogicOpEnable = VK_TRUE;
    enableVid.vertexInputDynamicState = VK_TRUE;

    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    VkDeviceQueueCreateInfo graphicsQueueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    graphicsQueueInfo.queueFamilyIndex = graphicsFamily;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &queuePriority;
    queueInfos.push_back(graphicsQueueInfo);
    if (presentFamily != graphicsFamily) {
        VkDeviceQueueCreateInfo presentQueueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        presentQueueInfo.queueFamilyIndex = presentFamily;
        presentQueueInfo.queueCount = 1;
        presentQueueInfo.pQueuePriorities = &queuePriority;
        queueInfos.push_back(presentQueueInfo);
    }

    std::vector<const char*> deviceExtensions;
    for (const char* req : requiredExtensions) deviceExtensions.push_back(req);

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &enableFeatures;
    deviceInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: vkCreateDevice 失败\n");
        return false;
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
    allocatorInfo.instance = instance;
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    VmaVulkanFunctions vmaFunctions{};
    if (vmaImportVulkanFunctionsFromVolk(&allocatorInfo, &vmaFunctions) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: VMA 无法导入 volk 函数表\n");
        return false;
    }
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    if (vmaCreateAllocator(&allocatorInfo, &allocator) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: VMA allocator 创建失败\n");
        return false;
    }

    initialized = true;
    std::fprintf(stderr, "Vulkan 设备: %s (API %u.%u.%u), descriptor buffer alignment %llu\n",
                 properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
                 VK_API_VERSION_MINOR(properties.apiVersion),
                 VK_API_VERSION_PATCH(properties.apiVersion),
                 static_cast<unsigned long long>(
                     descriptorBufferProperties.descriptorBufferOffsetAlignment));
    return true;
}

void VulkanContext::shutdown() {
    if (allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator);
        allocator = VK_NULL_HANDLE;
    }
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
    if (messenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        messenger = VK_NULL_HANDLE;
    }
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
    initialized = false;
}

} // namespace vpt
