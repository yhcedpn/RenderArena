#pragma once

#include <volk.h>
#include <vma/vk_mem_alloc.h>

#include <atomic>
#include <cstdint>
#include <string>

struct SDL_Window;

namespace vpt {

extern std::atomic<bool> g_validationFailure;

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    std::uint32_t graphicsFamily = 0;
    std::uint32_t presentFamily = 0;
    VmaAllocator allocator = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceLimits limits{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorBufferProperties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};

    bool validationEnabled = false;
    bool initialized = false;

    bool init(SDL_Window* window, bool debugBuild);
    void shutdown();

    std::string deviceName() const;
};

} // namespace vpt
