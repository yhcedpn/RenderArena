#pragma once

#include <atomic>
#include <cstdint>
#include <volk.h>

struct SDL_Window;

namespace arena {
struct VulkanContext {
    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics = VK_NULL_HANDLE, present = VK_NULL_HANDLE;
    std::uint32_t graphicsFamily = 0, presentFamily = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayProperties{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties{};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT descriptorProperties{};
    std::atomic_bool validationFailed{false};

    VulkanContext() = default;
    void initialize();
    void shutdown();
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
};
}

