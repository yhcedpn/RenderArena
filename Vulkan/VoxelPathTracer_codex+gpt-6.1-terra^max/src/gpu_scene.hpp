#pragma once

#include "scene.hpp"
#include "vulkan.hpp"

#include <vector>

namespace arena {
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
struct GpuMaterial {
    float absorption[3], ior;
    float emission[3];
    std::uint32_t type;
};
struct GpuLight {
    std::uint32_t triangle;
    float area, cumulativePower;
};
struct BottomLevel {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer storage;
    std::uint32_t firstTriangle = 0;
};
struct GpuScene {
    explicit GpuScene(VulkanContext& context) : vk(context) {}
    ~GpuScene();
    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;
    void initialize(const Scene& scene);

    VulkanContext& vk;
    Buffer vertices, triangleInfo, materials, lights;
    Image baseColorTexture, roughnessTexture;
    VkSampler sampler = VK_NULL_HANDLE;
    std::uint32_t lightCount = 0;
    float lightPower = 0;
    std::vector<BottomLevel> bottom;
    VkAccelerationStructureKHR top = VK_NULL_HANDLE;
    Buffer topStorage;
    Buffer makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    void destroyBuffer(Buffer& buffer);
    void upload(const Buffer& buffer, const void* data, VkDeviceSize size);
private:
    std::vector<Buffer> temporary;
    std::vector<BottomLevel> uncompacted;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkQueryPool query = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool fencePending = false;
    Image makeImage(std::uint32_t size, std::uint32_t levels, std::uint32_t layers, VkFormat format);
    void destroyImage(Image& image);
    VkAccelerationStructureKHR makeAS(const Buffer& buffer, VkAccelerationStructureTypeKHR type);
    VkCommandBuffer begin();
    void execute(VkCommandBuffer command);
};
}
