#pragma once

#include "math.hpp"
#include "scene.hpp"
#include "shared.hpp"
#include "vk_context.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vpt {

struct CameraState {
    Vec3 position{8.0f, 2.5f, 20.0f};
    float yaw = 0.0f;
    float pitch = -0.0357f;
};

enum class FrameStatus { Presented, Skipped, Fatal };

struct GpuBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
};

struct GpuImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::uint32_t mipLevels = 1;
    std::uint32_t layers = 1;
};

struct FrameSlot {
    VkCommandBuffer command = VK_NULL_HANDLE;
    GpuBuffer camera;
    GpuBuffer descriptors;
    VkSemaphore acquire = VK_NULL_HANDLE;
    std::uint64_t lastSignal = 0;
};

class Renderer {
public:
    bool init(VulkanContext& context, SceneData&& scene, SDL_Window* window,
              const std::filesystem::path& executableDir);
    void shutdown();

    FrameStatus renderFrame(const CameraState& camera, bool resetCamera);
    void requestResize() { resizeRequested_ = true; }
    bool takeStatsReset() {
        const bool r = statsReset_;
        statsReset_ = false;
        return r;
    }
    VkExtent2D extent() const { return extent_; }

private:
    bool createSwapchain(VkSwapchainKHR old = VK_NULL_HANDLE);
    void destroySwapchain();
    bool createSizedResources();
    void destroySizedResources();
    bool createDescriptors(bool full = true);
    bool createPipelines();
    bool loadShaderModule(const char* relative, std::vector<std::uint32_t>& code);
    bool createShaderObject(const char* name, VkShaderStageFlagBits stage, VkShaderEXT& out);
    bool buildAccelerationStructures();
    void destroyAccelerationStructures();

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                      GpuBuffer& out);
    void destroyBuffer(GpuBuffer& buffer);
    void uploadBuffer(const void* data, VkDeviceSize size, GpuBuffer& buffer);
    bool createTextureArray(VkFormat format, const std::vector<TextureLevel>& levels,
                            GpuImage& out);
    bool createStorageImage(std::uint32_t width, std::uint32_t height, VkFormat format,
                            GpuImage& out);
    void destroyImage(GpuImage& image);

    void writeDescriptorBuffer(FrameSlot& slot);
    void bindPassResources(VkCommandBuffer cmd, FrameSlot& slot);
    void barrier(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                 VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
    void imageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                      VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
    void clearBuffer(const GpuBuffer& buffer);
    void dispatchCompute(VkShaderEXT shader, const PushConstantsGpu& pc, std::uint32_t gx,
                         std::uint32_t gy);
    void dispatchComputeIndirect(VkShaderEXT shader, const PushConstantsGpu& pc,
                                 VkDeviceAddress argsAddress);
    void traceRaysDirect(std::uint32_t record, std::uint32_t width, std::uint32_t height);
    void traceRaysIndirect(std::uint32_t record, VkDeviceAddress argsAddress);
    void recordFrame(VkCommandBuffer cmd, std::uint32_t imageIndex, const CameraGpu& cameraData);
    bool createSbt();
    void destroySbt();

    VulkanContext* context_ = nullptr;
    SceneData scene_;
    SDL_Window* window_ = nullptr;
    std::filesystem::path executableDir_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding bindings_[bind::Count]{};
    VkDeviceSize bindingOffsets_[bind::Count]{};
    VkDeviceSize descriptorLayoutSize_ = 0;

    std::array<GpuBuffer, bind::Count> buffers_{};
    GpuImage baseColorTexture_;
    GpuImage roughnessTexture_;
    VkSampler textureSampler_ = VK_NULL_HANDLE;

    VkSampler compositeSampler_ = VK_NULL_HANDLE;

    VkShaderEXT shaderSetup_ = VK_NULL_HANDLE;
    VkShaderEXT shaderShade_ = VK_NULL_HANDLE;
    VkShaderEXT shaderPrepare_ = VK_NULL_HANDLE;
    VkShaderEXT shaderRestirInitial_ = VK_NULL_HANDLE;
    VkShaderEXT shaderRestirTemporal_ = VK_NULL_HANDLE;
    VkShaderEXT shaderRestirSpatial_ = VK_NULL_HANDLE;
    VkShaderEXT shaderRestirShadow_ = VK_NULL_HANDLE;
    VkShaderEXT shaderShadowResolve_ = VK_NULL_HANDLE;
    VkShaderEXT shaderResolve_ = VK_NULL_HANDLE;
    VkShaderEXT shaderSvgfTemporal_ = VK_NULL_HANDLE;
    VkShaderEXT shaderSvgfVariance_ = VK_NULL_HANDLE;
    VkShaderEXT shaderAtrous_ = VK_NULL_HANDLE;
    VkShaderEXT shaderFinalize_ = VK_NULL_HANDLE;
    VkShaderEXT shaderComposite_ = VK_NULL_HANDLE;
    VkShaderEXT shaderDisplayVert_ = VK_NULL_HANDLE;
    VkShaderEXT shaderDisplayFrag_ = VK_NULL_HANDLE;

    VkPipeline rtPipeline_ = VK_NULL_HANDLE;
    GpuBuffer sbtBuffer_;
    VkDeviceAddress sbtAddress_ = 0;
    std::uint32_t sbtRaygenStride_ = 0;
    std::uint32_t sbtMissStride_ = 0;
    std::uint32_t sbtHitStride_ = 0;
    std::uint32_t sbtRaygenOffset_ = 0;
    std::uint32_t sbtMissOffset_ = 0;
    std::uint32_t sbtHitOffset_ = 0;

    std::vector<VkAccelerationStructureKHR> blas_;
    std::vector<GpuBuffer> blasBuffers_;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    GpuBuffer tlasBuffer_;
    GpuBuffer asVertexBuffer_;
    GpuBuffer asIndexBuffer_;
    GpuBuffer tlasInstanceBuffer_;
    std::vector<std::uint64_t> groupVertexOffsets_;
    std::vector<std::uint64_t> groupIndexOffsets_;
    std::vector<std::uint32_t> groupIndexCounts_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer transientCommand_ = VK_NULL_HANDLE;
    VkCommandBuffer activeCommand_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::array<FrameSlot, 2> slots_{};
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{0, 0};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkSemaphore> renderFinished_;
    std::uint32_t imageCount_ = 0;

    std::uint32_t frameIndex_ = 0;
    std::uint32_t pixelCount_ = 0;
    Mat4 previousViewProj_ = Mat4::identity();
    CameraState previousCamera_{};
    bool hasPreviousFrame_ = false;
    bool resetHistory_ = true;
    bool resizeRequested_ = false;
    bool statsReset_ = true;
    bool fatal_ = false;
    VkExtent2D pendingExtent_{0, 0};
};

} // namespace vpt
