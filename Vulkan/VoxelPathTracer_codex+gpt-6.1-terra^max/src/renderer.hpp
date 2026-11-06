#pragma once

#include "gpu_scene.hpp"

#include <array>
#include <filesystem>
#include <vector>

namespace arena {
class Renderer {
public:
    Renderer(VulkanContext& context, GpuScene& scene, std::filesystem::path base);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void initialize();
    void run();
private:
    struct FrameSlot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };
    struct Push {
        Vec3 position; float tanHalfFov;
        Vec3 forward; std::uint32_t width;
        Vec3 right; std::uint32_t height;
        Vec3 up; std::uint32_t frame;
        std::uint32_t mode, inputQueue, maxTraceWidth, maxGroupWidth;
        float lightPower; std::uint32_t lightCount, cameraSlot, reserved1;
    };
    struct CameraState {
        Vec3 position; float tanHalfFov;
        Vec3 forward; std::uint32_t width;
        Vec3 right; std::uint32_t height;
        Vec3 up; std::uint32_t frame;
    };
    struct CameraPair {
        CameraState current, previous;
        std::uint32_t valid, reserved0, reserved1, reserved2;
    };
    VulkanContext& vk;
    GpuScene& scene;
    std::filesystem::path basePath;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline rayPipeline = VK_NULL_HANDLE;
    VkShaderEXT primary = VK_NULL_HANDLE, scatter = VK_NULL_HANDLE, prepare = VK_NULL_HANDLE, shadowResolve = VK_NULL_HANDLE;
    VkShaderEXT restirInit = VK_NULL_HANDLE, restirTemporal = VK_NULL_HANDLE, restirSpatial = VK_NULL_HANDLE;
    VkShaderEXT svgfTemporal = VK_NULL_HANDLE, svgfVariance = VK_NULL_HANDLE, svgfAtrous = VK_NULL_HANDLE;
    VkShaderEXT opticalTemporal = VK_NULL_HANDLE, opticalVariance = VK_NULL_HANDLE, opticalAtrous = VK_NULL_HANDLE;
    VkShaderEXT vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    Buffer sbt, descriptor, paths0, paths1, hits, counters, radiance, shadows, reservoirs, guides, cameras, signalFlags, histories, filters;
    Buffer opticalPaths, opticalGuides0, opticalGuides1, opticalMetadata, opticalHistories, opticalFilters, opticalChains, prefixRadiance, prefixOptical;
    VkDeviceSize descriptorOffset = 0;
    VkStridedDeviceAddressRegionKHR raygenRegion{}, missRegion{}, hitRegion{}, callableRegion{};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkSemaphore> presented;
    std::vector<bool> imageUsed;
    std::array<FrameSlot, 2> slots{};
    std::uint32_t frameSlot = 0, frameNumber = 0;
    Vec3 position{8.0f, 2.5f, 20.0f};
    float yaw = -1.57079632679f, pitch = -0.0356991127f;
    bool captured = false, historyValid = false, cameraCut = true;
    CameraState previousCamera{};
    void createPipeline();
    void createFrames();
    void recreate();
    void destroySwapchain();
    void writeDescriptors();
    void record(VkCommandBuffer command, std::uint32_t imageIndex, const Push& push);
    bool render(const Push& push);
    Push camera() const;
};
}
