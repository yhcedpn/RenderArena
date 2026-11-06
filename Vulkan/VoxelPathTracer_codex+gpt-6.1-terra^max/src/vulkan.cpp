#include "vulkan.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace arena {
namespace {
void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" :
        ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WARNING" : "INFO");
    std::fprintf(stderr, "Vulkan %s: %s\n", level, data->pMessage);
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0)
        static_cast<std::atomic_bool*>(user)->store(true);
    return VK_FALSE;
}
bool contains(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [&](const auto& p) { return std::strcmp(p.extensionName, name) == 0; });
}
std::vector<VkExtensionProperties> instanceExtensions(const char* layer = nullptr)
{
    std::uint32_t count = 0; check(vkEnumerateInstanceExtensionProperties(layer, &count, nullptr), "instance extension count");
    std::vector<VkExtensionProperties> result(count);
    check(vkEnumerateInstanceExtensionProperties(layer, &count, result.data()), "instance extension enumeration");
    result.resize(count); return result;
}
std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice physical)
{
    std::uint32_t count = 0; check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "device extension count");
    std::vector<VkExtensionProperties> result(count);
    check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, result.data()), "device extension enumeration");
    result.resize(count); return result;
}

struct FeatureChain {
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceShaderObjectFeaturesEXT shader{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    FeatureChain() {
        core.pNext = &v12; v12.pNext = &v13; v13.pNext = &as; as.pNext = &ray;
        ray.pNext = &descriptor; descriptor.pNext = &shader; shader.pNext = &vertex; vertex.pNext = &dynamic3;
    }
};
const std::vector<const char*> requiredExtensions{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME
};

struct Candidate {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t graphics = 0, present = 0;
    std::vector<std::string> missing;
};
Candidate inspect(VkPhysicalDevice physical, VkSurfaceKHR surface)
{
    Candidate c{}; c.physical = physical;
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(physical, &props);
    if (props.apiVersion < VK_API_VERSION_1_4) c.missing.emplace_back("Vulkan 1.4 device API");
    const auto extensions = deviceExtensions(physical);
    for (const char* e : requiredExtensions) if (!contains(extensions, e)) c.missing.emplace_back(e);
    std::uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count); vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    bool graphics = false, present = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (queues[i].queueCount && (queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            c.graphics = i; graphics = true;
        }
        VkBool32 supported = VK_FALSE; check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported), "surface support query");
        if (queues[i].queueCount && supported) { c.present = i; present = true; }
        if (graphics && present && c.graphics == c.present) break;
    }
    if (!graphics) c.missing.emplace_back("graphics and compute queue");
    if (!present) c.missing.emplace_back("present queue");
    std::uint32_t formatCount = 0; check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr), "surface format count");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount) check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data()), "surface formats");
    const bool srgb = std::any_of(formats.begin(), formats.end(), [](const auto& f) {
        return (f.format == VK_FORMAT_UNDEFINED || f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (!srgb) c.missing.emplace_back("sRGB swapchain format");
    const auto format = [&](VkFormat f, VkFormatFeatureFlags flags, const char* label) {
        VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(physical, f, &p);
        if ((p.optimalTilingFeatures & flags) != flags) c.missing.emplace_back(label);
    };
    const auto sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    format(VK_FORMAT_R8G8B8A8_SRGB, sampled, "RGBA8 sRGB sampled linear filtering");
    format(VK_FORMAT_R8_UNORM, sampled, "R8 sampled linear filtering");
    VkFormatProperties vertexFormat{}; vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32G32B32_SFLOAT, &vertexFormat);
    if (!(vertexFormat.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))
        c.missing.emplace_back("RGB32F acceleration structure vertex format");
    FeatureChain f; vkGetPhysicalDeviceFeatures2(physical, &f.core);
    const auto require = [&](VkBool32 available, const char* label) { if (!available) c.missing.emplace_back(label); };
    require(f.v12.timelineSemaphore, "timelineSemaphore"); require(f.v12.bufferDeviceAddress, "bufferDeviceAddress");
    require(f.v12.scalarBlockLayout, "scalarBlockLayout"); require(f.v13.dynamicRendering, "dynamicRendering");
    require(f.v13.synchronization2, "synchronization2"); require(f.as.accelerationStructure, "accelerationStructure");
    require(f.ray.rayTracingPipeline, "rayTracingPipeline"); require(f.ray.rayTracingPipelineTraceRaysIndirect, "rayTracingPipelineTraceRaysIndirect");
    require(f.descriptor.descriptorBuffer, "descriptorBuffer"); require(f.shader.shaderObject, "shaderObject");
    require(f.vertex.vertexInputDynamicState, "vertexInputDynamicState");
    require(f.dynamic3.extendedDynamicState3DepthClampEnable, "dynamic depth clamp");
    require(f.dynamic3.extendedDynamicState3PolygonMode, "dynamic polygon mode");
    require(f.dynamic3.extendedDynamicState3RasterizationSamples, "dynamic rasterization samples");
    require(f.dynamic3.extendedDynamicState3SampleMask, "dynamic sample mask");
    require(f.dynamic3.extendedDynamicState3AlphaToCoverageEnable, "dynamic alpha to coverage");
    require(f.dynamic3.extendedDynamicState3LogicOpEnable, "dynamic logic op enable");
    require(f.dynamic3.extendedDynamicState3ColorBlendEnable, "dynamic color blend enable");
    require(f.dynamic3.extendedDynamicState3ColorBlendEquation, "dynamic color blend equation");
    require(f.dynamic3.extendedDynamicState3ColorWriteMask, "dynamic color write mask");
    return c;
}
}
void VulkanContext::initialize()
{
    check(volkInitialize(), "volkInitialize");
    std::uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) check(vkEnumerateInstanceVersion(&version), "vkEnumerateInstanceVersion");
    if (version < VK_API_VERSION_1_4) throw std::runtime_error("Vulkan loader does not support 1.4");
    window = SDL_CreateWindow("VoxelPathTracer — FPS: -- | ms/frame: --", 1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    std::uint32_t sdlCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlCount);
    if (!sdlExtensions) throw std::runtime_error(std::string("SDL Vulkan extensions: ") + SDL_GetError());
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlCount);
    std::vector<const char*> layers;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    VkValidationFeatureEnableEXT enabledValidation = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
#ifndef NDEBUG
    std::uint32_t layerCount = 0; check(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "instance layer count");
    std::vector<VkLayerProperties> availableLayers(layerCount);
    check(vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data()), "instance layers");
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (std::none_of(availableLayers.begin(), availableLayers.end(), [&](const auto& p) { return std::strcmp(p.layerName, layer) == 0; }))
        throw std::runtime_error("Debug requires VK_LAYER_KHRONOS_validation");
    const auto globalExtensions = instanceExtensions(); const auto layerExtensions = instanceExtensions(layer);
    for (const char* e : {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME}) {
        if (!contains(globalExtensions, e) && !contains(layerExtensions, e)) throw std::runtime_error(std::string("Debug requires instance extension ") + e);
        extensions.push_back(e);
    }
    layers.push_back(layer);
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = diagnostic; debug.pUserData = &validationFailed;
    validation.enabledValidationFeatureCount = 1; validation.pEnabledValidationFeatures = &enabledValidation; validation.pNext = &debug;
#endif
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "VoxelPathTracer"; application.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application; create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()); create.ppEnabledExtensionNames = extensions.data();
    create.enabledLayerCount = static_cast<std::uint32_t>(layers.size()); create.ppEnabledLayerNames = layers.data();
#ifndef NDEBUG
    create.pNext = &validation;
#endif
    check(vkCreateInstance(&create, nullptr, &instance), "vkCreateInstance"); volkLoadInstance(instance);
#ifndef NDEBUG
    check(vkCreateDebugUtilsMessengerEXT(instance, &debug, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");
#endif
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) throw std::runtime_error(std::string("SDL Vulkan surface: ") + SDL_GetError());
    std::uint32_t physicalCount = 0; check(vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr), "physical device count");
    if (!physicalCount) throw std::runtime_error("no Vulkan physical device");
    std::vector<VkPhysicalDevice> devices(physicalCount); check(vkEnumeratePhysicalDevices(instance, &physicalCount, devices.data()), "physical devices");
    std::string report;
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto p = devices[index];
        const auto c = inspect(p, surface);
        if (c.missing.empty()) { physical = p; graphicsFamily = c.graphics; presentFamily = c.present; break; }
        report += "device " + std::to_string(index) + " missing:";
        for (const auto& item : c.missing) report += " " + item + ";";
        report += "\n";
    }
    if (!physical) throw std::runtime_error("No device satisfies the Vulkan capability contract:\n" + report);
    properties = {}; rayProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    asProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    descriptorProperties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &rayProperties; rayProperties.pNext = &asProperties; asProperties.pNext = &descriptorProperties;
    vkGetPhysicalDeviceProperties2(physical, &p2); properties = p2.properties;
    FeatureChain request;
    request.v12.timelineSemaphore = request.v12.bufferDeviceAddress = request.v12.scalarBlockLayout = VK_TRUE;
    request.v13.dynamicRendering = request.v13.synchronization2 = VK_TRUE;
    request.as.accelerationStructure = VK_TRUE; request.ray.rayTracingPipeline = request.ray.rayTracingPipelineTraceRaysIndirect = VK_TRUE;
    request.descriptor.descriptorBuffer = VK_TRUE; request.shader.shaderObject = VK_TRUE; request.vertex.vertexInputDynamicState = VK_TRUE;
    request.dynamic3.extendedDynamicState3DepthClampEnable = request.dynamic3.extendedDynamicState3PolygonMode = VK_TRUE;
    request.dynamic3.extendedDynamicState3RasterizationSamples = request.dynamic3.extendedDynamicState3SampleMask = VK_TRUE;
    request.dynamic3.extendedDynamicState3AlphaToCoverageEnable = request.dynamic3.extendedDynamicState3LogicOpEnable = VK_TRUE;
    request.dynamic3.extendedDynamicState3ColorBlendEnable = request.dynamic3.extendedDynamicState3ColorBlendEquation = VK_TRUE;
    request.dynamic3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex = graphicsFamily; q.queueCount = 1; q.pQueuePriorities = &priority; queues.push_back(q);
    if (presentFamily != graphicsFamily) { q.queueFamilyIndex = presentFamily; queues.push_back(q); }
    VkDeviceCreateInfo deviceCreate{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceCreate.pNext = &request.core; deviceCreate.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size()); deviceCreate.pQueueCreateInfos = queues.data();
    deviceCreate.enabledExtensionCount = static_cast<std::uint32_t>(requiredExtensions.size()); deviceCreate.ppEnabledExtensionNames = requiredExtensions.data();
    check(vkCreateDevice(physical, &deviceCreate, nullptr, &device), "vkCreateDevice"); volkLoadDevice(device);
    vkGetDeviceQueue(device, graphicsFamily, 0, &graphics); vkGetDeviceQueue(device, presentFamily, 0, &present);
}
void VulkanContext::shutdown()
{
    if (device) { vkDeviceWaitIdle(device); vkDestroyDevice(device, nullptr); device = VK_NULL_HANDLE; }
    if (surface) { SDL_Vulkan_DestroySurface(instance, surface, nullptr); surface = VK_NULL_HANDLE; }
    if (messenger) { vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr); messenger = VK_NULL_HANDLE; }
    if (instance) { vkDestroyInstance(instance, nullptr); instance = VK_NULL_HANDLE; }
    if (window) { SDL_DestroyWindow(window); window = nullptr; }
}
VulkanContext::~VulkanContext() { shutdown(); }
}
