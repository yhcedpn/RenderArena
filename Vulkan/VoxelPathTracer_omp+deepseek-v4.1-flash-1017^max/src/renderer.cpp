#include "renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace vpt {

namespace {

std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

std::uint64_t alignUp64(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

VkDeviceSize bytesFor(std::uint64_t count) { return static_cast<VkDeviceSize>(count); }

} // namespace

bool Renderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                            GpuBuffer& out) {
    if (size == 0) size = 4;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                          VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    if ((usage & VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT) != 0) {
        allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(context_->allocator, &info, &allocInfo, &out.buffer, &out.allocation,
                        &result) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 buffer 失败 (size=%llu)\n",
                     static_cast<unsigned long long>(size));
        return false;
    }
    out.size = size;
    out.mapped = result.pMappedData;
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = out.buffer;
        out.address = vkGetBufferDeviceAddress(context_->device, &addressInfo);
    }
    return true;
}

void Renderer::destroyBuffer(GpuBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(context_->allocator, buffer.buffer, buffer.allocation);
    }
    buffer = GpuBuffer{};
}

void Renderer::uploadBuffer(const void* data, VkDeviceSize size, GpuBuffer& buffer) {
    if (buffer.mapped != nullptr) {
        std::memcpy(buffer.mapped, data, static_cast<size_t>(size));
        vmaFlushAllocation(context_->allocator, buffer.allocation, 0, VK_WHOLE_SIZE);
    }
}

void Renderer::destroyImage(GpuImage& image) {
    if (image.view != VK_NULL_HANDLE) vkDestroyImageView(context_->device, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) {
        vmaDestroyImage(context_->allocator, image.image, image.allocation);
    }
    image = GpuImage{};
}

bool Renderer::init(VulkanContext& context, SceneData&& scene, SDL_Window* window,
                    const std::filesystem::path& executableDir) {
    context_ = &context;
    scene_ = std::move(scene);
    window_ = window;
    executableDir_ = executableDir;

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context_->graphicsFamily;
    if (vkCreateCommandPool(context_->device, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 command pool 失败\n");
        return false;
    }
    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = commandPool_;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(context_->device, &cmdInfo, &transientCommand_) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 分配 transient command buffer 失败\n");
        return false;
    }

    VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineType.initialValue = 0;
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreInfo.pNext = &timelineType;
    if (vkCreateSemaphore(context_->device, &semaphoreInfo, nullptr, &timeline_) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 timeline semaphore 失败\n");
        return false;
    }
    for (FrameSlot& slot : slots_) {
        semaphoreInfo.pNext = nullptr;
        if (vkCreateSemaphore(context_->device, &semaphoreInfo, nullptr, &slot.acquire) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan 错误: 创建 acquire semaphore 失败\n");
            return false;
        }
    }

    // 静态场景 buffer（host-visible，小数据）。
    const auto createStatic = [&](std::uint32_t binding, const void* data, VkDeviceSize size) {
        if (!createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          true, buffers_[binding])) {
            return false;
        }
        uploadBuffer(data, size, buffers_[binding]);
        return true;
    };
    if (!createStatic(bind::Materials, scene_.gpuMaterials.data(),
                      bytesFor(scene_.gpuMaterials.size() * sizeof(MaterialGpu)))) return false;
    if (!createStatic(bind::Triangles, scene_.triangles.data(),
                      bytesFor(scene_.triangles.size() * sizeof(TriangleGpu)))) return false;
    if (!createStatic(bind::Instances, scene_.instances.data(),
                      bytesFor(scene_.instances.size() * sizeof(InstanceDescGpu)))) return false;
    if (!createStatic(bind::Lights, scene_.lights.data(),
                      bytesFor(scene_.lights.size() * sizeof(LightGpu)))) return false;
    if (!createStatic(bind::Planes, scene_.planes.data(),
                      bytesFor(scene_.planes.size() * sizeof(PlaneGpu)))) return false;

    if (!createTextureArray(VK_FORMAT_R8G8B8A8_SRGB, scene_.baseColorLevels, baseColorTexture_)) {
        return false;
    }
    if (!createTextureArray(VK_FORMAT_R8_UNORM, scene_.roughnessLevels, roughnessTexture_)) {
        return false;
    }
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(scene_.mipLevels - 1u);
    if (vkCreateSampler(context_->device, &samplerInfo, nullptr, &textureSampler_) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 sampler 失败\n");
        return false;
    }

    // 描述符布局（单一 set，全部资源经 descriptor buffer 绑定）。
    for (std::uint32_t b = 0; b < bind::Count; ++b) {
        VkDescriptorSetLayoutBinding& binding = bindings_[b];
        binding = VkDescriptorSetLayoutBinding{};
        binding.binding = b;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        if (b == bind::BaseColor || b == bind::Roughness) {
            binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        } else if (b == bind::Camera) {
            binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        } else if (b == bind::Tlas) {
            binding.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        } else {
            binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    layoutInfo.bindingCount = bind::Count;
    layoutInfo.pBindings = bindings_;
    if (vkCreateDescriptorSetLayout(context_->device, &layoutInfo, nullptr, &setLayout_) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 descriptor set layout 失败\n");
        return false;
    }
    vkGetDescriptorSetLayoutSizeEXT(context_->device, setLayout_, &descriptorLayoutSize_);
    for (std::uint32_t b = 0; b < bind::Count; ++b) {
        vkGetDescriptorSetLayoutBindingOffsetEXT(context_->device, setLayout_, b,
                                                 &bindingOffsets_[b]);
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_ALL;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstantsGpu);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(context_->device, &pipelineLayoutInfo, nullptr, &pipelineLayout_) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 pipeline layout 失败\n");
        return false;
    }

    if (!createSwapchain()) return false;
    if (!createSizedResources()) return false;

    const std::uint32_t descriptorSize = static_cast<std::uint32_t>(
        alignUp64(descriptorLayoutSize_,
                  std::max<VkDeviceSize>(64, context_->descriptorBufferProperties.descriptorBufferOffsetAlignment)));
    for (FrameSlot& slot : slots_) {
        if (!createBuffer(sizeof(CameraGpu),
                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          true, slot.camera)) {
            return false;
        }
        if (!createBuffer(descriptorSize,
                          VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          true, slot.descriptors)) {
            return false;
        }
    }

    if (!buildAccelerationStructures()) return false;
    if (!createPipelines()) return false;
    if (!createDescriptors()) return false;

    VkCommandBufferAllocateInfo slotCmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    slotCmdInfo.commandPool = commandPool_;
    slotCmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    slotCmdInfo.commandBufferCount = 2;
    VkCommandBuffer commandBuffers[2]{};
    if (vkAllocateCommandBuffers(context_->device, &slotCmdInfo, commandBuffers) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 分配 frame command buffer 失败\n");
        return false;
    }
    slots_[0].command = commandBuffers[0];
    slots_[1].command = commandBuffers[1];

    statsReset_ = true;
    return true;
}

void Renderer::shutdown() {
    if (context_ == nullptr || context_->device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(context_->device);
    for (FrameSlot& slot : slots_) {
        if (slot.command != VK_NULL_HANDLE) slot.command = VK_NULL_HANDLE;
        if (slot.acquire != VK_NULL_HANDLE) {
            vkDestroySemaphore(context_->device, slot.acquire, nullptr);
            slot.acquire = VK_NULL_HANDLE;
        }
        destroyBuffer(slot.camera);
        destroyBuffer(slot.descriptors);
    }
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(context_->device, timeline_, nullptr);
        timeline_ = VK_NULL_HANDLE;
    }
    destroySizedResources();
    destroySwapchain();
    destroyAccelerationStructures();
    destroySbt();
    if (rtPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(context_->device, rtPipeline_, nullptr);
        rtPipeline_ = VK_NULL_HANDLE;
    }
    const VkShaderEXT shaders[] = {shaderSetup_, shaderShade_, shaderPrepare_, shaderRestirInitial_,
                                   shaderRestirTemporal_, shaderRestirSpatial_, shaderRestirShadow_,
                                   shaderShadowResolve_, shaderResolve_, shaderSvgfTemporal_,
                                   shaderSvgfVariance_, shaderAtrous_, shaderFinalize_,
                                   shaderComposite_, shaderDisplayVert_, shaderDisplayFrag_};
    for (VkShaderEXT shader : shaders) {
        if (shader != VK_NULL_HANDLE) vkDestroyShaderEXT(context_->device, shader, nullptr);
    }
    if (textureSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(context_->device, textureSampler_, nullptr);
        textureSampler_ = VK_NULL_HANDLE;
    }
    destroyImage(baseColorTexture_);
    destroyImage(roughnessTexture_);
    for (GpuBuffer& buffer : buffers_) destroyBuffer(buffer);
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(context_->device, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(context_->device, setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(context_->device, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    context_ = nullptr;
}

bool Renderer::createSwapchain(VkSwapchainKHR oldSwapchain) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_->physicalDevice, context_->surface, &caps);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(context_->physicalDevice, context_->surface, &formatCount,
                                         nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(context_->physicalDevice, context_->surface,
                                             &formatCount, formats.data());
    }
    const VkFormat preferred[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                  VK_FORMAT_A8B8G8R8_SRGB_PACK32};
    bool found = false;
    VkSurfaceFormatKHR chosen{};
    for (VkFormat pref : preferred) {
        for (const VkSurfaceFormatKHR& f : formats) {
            if (f.format == pref && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    if (!found) {
        std::fprintf(stderr, "Vulkan 错误: 交换链没有可用的 sRGB 归一化格式\n");
        fatal_ = true;
        return false;
    }
    VkFormatProperties formatProps{};
    vkGetPhysicalDeviceFormatProperties(context_->physicalDevice, chosen.format, &formatProps);
    if ((formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
        std::fprintf(stderr, "Vulkan 错误: 交换链 sRGB 格式不支持颜色附件\n");
        fatal_ = true;
        return false;
    }

    std::uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(context_->physicalDevice, context_->surface,
                                              &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    if (modeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(context_->physicalDevice, context_->surface,
                                                  &modeCount, modes.data());
    }
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) { presentMode = mode; break; }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w = 0;
        int h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        extent.width = static_cast<std::uint32_t>(std::max(w, 1));
        extent.height = static_cast<std::uint32_t>(std::max(h, 1));
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;

    std::uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = context_->surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    std::uint32_t families[2] = {context_->graphicsFamily, context_->presentFamily};
    if (context_->graphicsFamily != context_->presentFamily) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(context_->device, &info, nullptr, &newSwapchain) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建交换链失败\n");
        return false;
    }
    destroySwapchain();
    swapchain_ = newSwapchain;
    swapchainFormat_ = chosen.format;
    extent_ = extent;

    std::uint32_t count = 0;
    vkGetSwapchainImagesKHR(context_->device, swapchain_, &count, nullptr);
    swapchainImages_.resize(count);
    vkGetSwapchainImagesKHR(context_->device, swapchain_, &count, swapchainImages_.data());
    imageCount_ = count;
    swapchainViews_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = swapchainImages_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(context_->device, &viewInfo, nullptr, &swapchainViews_[i]) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan 错误: 创建交换链 image view 失败\n");
            return false;
        }
    }
    renderFinished_.resize(count);
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (std::uint32_t i = 0; i < count; ++i) {
        if (vkCreateSemaphore(context_->device, &semaphoreInfo, nullptr, &renderFinished_[i]) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan 错误: 创建 present semaphore 失败\n");
            return false;
        }
    }
    statsReset_ = true;
    return true;
}

void Renderer::destroySwapchain() {
    if (context_ == nullptr) return;
    for (VkImageView view : swapchainViews_) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(context_->device, view, nullptr);
    }
    swapchainViews_.clear();
    for (VkSemaphore semaphore : renderFinished_) {
        if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(context_->device, semaphore, nullptr);
    }
    renderFinished_.clear();
    swapchainImages_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(context_->device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool Renderer::createTextureArray(VkFormat format, const std::vector<TextureLevel>& levels,
                                  GpuImage& out) {
    if (levels.empty() || levels[0].width == 0) return false;
    const std::uint32_t mipLevels = static_cast<std::uint32_t>(levels.size());
    const std::uint32_t width = levels[0].width;
    const std::uint32_t height = levels[0].height;
    const std::uint32_t bpp = (format == VK_FORMAT_R8_UNORM) ? 1u : 4u;
    const std::uint32_t layers = static_cast<std::uint32_t>(
        levels[0].pixels.size() / (static_cast<size_t>(width) * height * bpp));
    if (layers == 0) return false;

    VkDeviceSize stagingSize = 0;
    for (const TextureLevel& level : levels) {
        stagingSize += bytesFor(static_cast<std::uint64_t>(level.width) * level.height * bpp * layers);
    }
    GpuBuffer staging;
    if (!createBuffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging)) return false;
    VkDeviceSize offset = 0;
    for (const TextureLevel& level : levels) {
        const VkDeviceSize layerSize =
            bytesFor(static_cast<std::uint64_t>(level.width) * level.height * bpp);
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            std::memcpy(static_cast<std::uint8_t*>(staging.mapped) + offset,
                        level.pixels.data() + static_cast<size_t>(layer) * layerSize,
                        static_cast<size_t>(layerSize));
            offset += layerSize;
        }
    }
    vmaFlushAllocation(context_->allocator, staging.allocation, 0, VK_WHOLE_SIZE);

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(context_->allocator, &imageInfo, &allocInfo, &out.image, &out.allocation,
                       nullptr) != VK_SUCCESS) {
        destroyBuffer(staging);
        std::fprintf(stderr, "Vulkan 错误: 创建纹理 image 失败\n");
        return false;
    }
    out.format = format;
    out.extent = {width, height};
    out.mipLevels = mipLevels;
    out.layers = layers;

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(context_->device, &fenceInfo, nullptr, &fence);
    vkResetCommandBuffer(transientCommand_, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(transientCommand_, &beginInfo);
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.image = out.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layers};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(transientCommand_, &dep);
    }
    std::vector<VkBufferImageCopy> copies;
    offset = 0;
    for (std::uint32_t mip = 0; mip < mipLevels; ++mip) {
        const TextureLevel& level = levels[mip];
        const VkDeviceSize layerSize =
            bytesFor(static_cast<std::uint64_t>(level.width) * level.height * bpp);
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            VkBufferImageCopy copy{};
            copy.bufferOffset = offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
            copy.imageExtent = {level.width, level.height, 1};
            copies.push_back(copy);
            offset += layerSize;
        }
    }
    vkCmdCopyBufferToImage(transientCommand_, staging.buffer, out.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(copies.size()), copies.data());
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = out.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layers};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(transientCommand_, &dep);
    }
    vkEndCommandBuffer(transientCommand_);
    VkCommandBufferSubmitInfo commandBufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandBufferInfo.commandBuffer = transientCommand_;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandBufferInfo;
    vkQueueSubmit2(context_->graphicsQueue, 1, &submit, fence);
    vkWaitForFences(context_->device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(context_->device, fence, nullptr);
    destroyBuffer(staging);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layers};
    if (vkCreateImageView(context_->device, &viewInfo, nullptr, &out.view) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建纹理 view 失败\n");
        return false;
    }
    return true;
}

bool Renderer::createSizedResources() {
    const std::uint64_t n = static_cast<std::uint64_t>(extent_.width) * extent_.height;
    if (n == 0) return false;
    pixelCount_ = static_cast<std::uint32_t>(n);
    const std::uint32_t maxRange = context_->limits.maxStorageBufferRange;

    const auto make = [&](std::uint32_t binding, std::uint64_t bytes) {
        if (bytes > maxRange) {
            std::fprintf(stderr, "Vulkan 错误: binding %u 需求 %llu 字节超过 maxStorageBufferRange %u\n",
                         binding, static_cast<unsigned long long>(bytes), maxRange);
            return false;
        }
        return createBuffer(bytesFor(bytes),
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                            false, buffers_[binding]);
    };

    const std::uint64_t pathCapacity = 2u * n;
    const std::uint64_t shadowCapacity = 3u * n;
    bool ok = true;
    ok = ok && make(bind::PathsA, pathCapacity * sizeof(PathGpu));
    ok = ok && make(bind::PathsB, pathCapacity * sizeof(PathGpu));
    ok = ok && make(bind::Hits, pathCapacity * sizeof(HitGpu));
    ok = ok && make(bind::Primary, 2u * n * sizeof(PrimarySurfaceGpu));
    ok = ok && make(bind::BranchContrib, 2u * n * 16u);
    ok = ok && make(bind::BranchInfo, 2u * n * sizeof(BranchInfoGpu));
    ok = ok && make(bind::RestirInit, n * sizeof(ReservoirGpu));
    ok = ok && make(bind::RestirTemporal, n * sizeof(ReservoirGpu));
    ok = ok && make(bind::RestirSpatial, n * sizeof(ReservoirGpu));
    ok = ok && make(bind::RestirHistory, 2u * n * sizeof(ReservoirGpu));
    ok = ok && make(bind::RestirOut, n * 16u);
    ok = ok && make(bind::ShadowQueue, shadowCapacity * sizeof(ShadowTaskGpu));
    ok = ok && make(bind::ShadowResults, shadowCapacity * 16u);
    ok = ok && make(bind::Counters, sizeof(CountersGpu));
    ok = ok && make(bind::Indirect, sizeof(IndirectArgsGpu));
    ok = ok && make(bind::DomainRadiance, 6u * n * 16u);
    ok = ok && make(bind::DomainGuide, 2u * 6u * n * 32u);
    ok = ok && make(bind::DomainM1, 2u * 6u * n * 16u);
    ok = ok && make(bind::DomainM2, 2u * 6u * n * 16u);
    ok = ok && make(bind::DomainHist, 2u * 6u * n * 4u);
    ok = ok && make(bind::DomainFiltered, 6u * n * 16u);
    ok = ok && make(bind::ScratchColorA, n * 16u);
    ok = ok && make(bind::ScratchColorB, n * 16u);
    ok = ok && make(bind::ScratchVarA, n * 4u);
    ok = ok && make(bind::ScratchVarB, n * 4u);
    ok = ok && make(bind::Analytic, n * 16u);
    ok = ok && make(bind::Composite, n * 16u);
    if (!ok) return false;
    return true;
}

void Renderer::destroySizedResources() {
    const std::uint32_t sizedBindings[] = {
        bind::PathsA, bind::PathsB, bind::Hits, bind::Primary, bind::BranchContrib,
        bind::BranchInfo, bind::RestirInit, bind::RestirTemporal, bind::RestirSpatial,
        bind::RestirHistory, bind::RestirOut, bind::ShadowQueue, bind::ShadowResults,
        bind::Counters, bind::Indirect, bind::DomainRadiance, bind::DomainGuide, bind::DomainM1,
        bind::DomainM2, bind::DomainHist, bind::DomainFiltered, bind::ScratchColorA,
        bind::ScratchColorB, bind::ScratchVarA, bind::ScratchVarB, bind::Analytic,
        bind::Composite};
    for (std::uint32_t binding : sizedBindings) destroyBuffer(buffers_[binding]);
}

bool Renderer::createDescriptors(bool full) {
    vkDeviceWaitIdle(context_->device);
    const std::uint32_t sizedBindings[] = {
        bind::PathsA, bind::PathsB, bind::Hits, bind::Primary, bind::BranchContrib,
        bind::BranchInfo, bind::RestirInit, bind::RestirTemporal, bind::RestirSpatial,
        bind::RestirHistory, bind::RestirOut, bind::ShadowQueue, bind::ShadowResults,
        bind::Counters, bind::Indirect, bind::DomainRadiance, bind::DomainGuide, bind::DomainM1,
        bind::DomainM2, bind::DomainHist, bind::DomainFiltered, bind::ScratchColorA,
        bind::ScratchColorB, bind::ScratchVarA, bind::ScratchVarB, bind::Analytic,
        bind::Composite};
    const auto isSized = [&](std::uint32_t binding) {
        for (std::uint32_t candidate : sizedBindings) {
            if (candidate == binding) return true;
        }
        return false;
    };
    const auto& dbProps = context_->descriptorBufferProperties;
    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addressInfo.accelerationStructure = tlas_;
    const VkDeviceAddress tlasAddress =
        vkGetAccelerationStructureDeviceAddressKHR(context_->device, &addressInfo);

    for (FrameSlot& slot : slots_) {
        // 单一共享 descriptor buffer：两帧都绑定 slots_[0]；相机 UBO 在 CPU 等待上一帧后更新。
        if (&slot != &slots_[0]) break;
        auto* base = static_cast<std::uint8_t*>(slot.descriptors.mapped);
        if (full) {
            std::memset(base, 0, static_cast<size_t>(slot.descriptors.size));
        }
        static constexpr std::uint32_t order[] = {
            2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u,
            19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 33u, 34u,
            35u, 0u, 1u};
        for (std::uint32_t b : order) {
            if (!full && !isSized(b)) continue;
            VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            size_t descriptorSize = 0;
            if (b == bind::BaseColor || b == bind::Roughness) {
                VkDescriptorImageInfo imageInfo{};
                imageInfo.sampler = textureSampler_;
                imageInfo.imageView = (b == bind::BaseColor) ? baseColorTexture_.view
                                                             : roughnessTexture_.view;
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                info.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                info.data.pCombinedImageSampler = &imageInfo;
                descriptorSize = dbProps.combinedImageSamplerDescriptorSize;
            } else if (b == bind::Camera) {
                VkDescriptorAddressInfoEXT addr{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
                addr.address = slot.camera.address;
                addr.range = sizeof(CameraGpu);
                addr.format = VK_FORMAT_UNDEFINED;
                info.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                info.data.pUniformBuffer = &addr;
                descriptorSize = dbProps.uniformBufferDescriptorSize;
            } else if (b == bind::Tlas) {
                info.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
                info.data.accelerationStructure = tlasAddress;
                descriptorSize = dbProps.accelerationStructureDescriptorSize;
            } else {
                VkDescriptorAddressInfoEXT addr{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
                addr.address = buffers_[b].address;
                addr.range = buffers_[b].size;
                addr.format = VK_FORMAT_UNDEFINED;
                info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                info.data.pStorageBuffer = &addr;
                descriptorSize = dbProps.storageBufferDescriptorSize;
            }
            vkGetDescriptorEXT(context_->device, &info, descriptorSize,
                               base + bindingOffsets_[b]);
        }
        vmaFlushAllocation(context_->allocator, slot.descriptors.allocation, 0, VK_WHOLE_SIZE);
    }
    return true;
}

bool Renderer::loadShaderModule(const char* relative, std::vector<std::uint32_t>& code) {
    const std::filesystem::path path = executableDir_ / "shaders" / relative;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "资源错误: 无法打开 shader %s\n", path.string().c_str());
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % 4) != 0) {
        std::fprintf(stderr, "资源错误: shader 大小非法 %s\n", path.string().c_str());
        return false;
    }
    code.resize(static_cast<size_t>(size) / sizeof(std::uint32_t));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) {
        std::fprintf(stderr, "资源错误: 读取 shader 失败 %s\n", path.string().c_str());
        return false;
    }
    return true;
}

bool Renderer::createShaderObject(const char* name, VkShaderStageFlagBits stage,
                                  VkShaderEXT& out) {
    std::vector<std::uint32_t> code;
    if (!loadShaderModule(name, code)) return false;
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_ALL;
    range.offset = 0;
    range.size = sizeof(PushConstantsGpu);
    VkShaderCreateInfoEXT info{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    info.stage = stage;
    info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    info.codeSize = code.size() * sizeof(std::uint32_t);
    info.pCode = code.data();
    info.pName = "main";
    info.nextStage = (stage == VK_SHADER_STAGE_VERTEX_BIT) ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
    info.setLayoutCount = 1;
    info.pSetLayouts = &setLayout_;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    if (vkCreateShadersEXT(context_->device, 1, &info, nullptr, &out) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 shader object 失败 %s\n", name);
        return false;
    }
    return true;
}

bool Renderer::createSbt() {
    const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& props = context_->rayTracingProperties;
    const std::uint32_t handleSize = props.shaderGroupHandleSize;
    const std::uint32_t baseAlignment = props.shaderGroupBaseAlignment;
    const std::uint32_t stride = alignUp(handleSize, baseAlignment);
    sbtRaygenStride_ = stride;
    sbtMissStride_ = stride;
    sbtHitStride_ = stride;
    sbtRaygenOffset_ = 0;
    sbtMissOffset_ = alignUp(sbtRaygenOffset_ + 2u * stride, baseAlignment);
    sbtHitOffset_ = alignUp(sbtMissOffset_ + stride, baseAlignment);
    const std::uint32_t total = alignUp(sbtHitOffset_ + stride, baseAlignment) + baseAlignment;
    if (!createBuffer(total, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      true, sbtBuffer_)) {
        return false;
    }
    std::vector<std::uint8_t> handles(static_cast<size_t>(handleSize) * 4u);
    if (vkGetRayTracingShaderGroupHandlesKHR(context_->device, rtPipeline_, 0, 4,
                                             handles.size(), handles.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 获取 RT shader group handles 失败\n");
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        std::uint64_t h = 0;
        std::memcpy(&h, handles.data() + static_cast<size_t>(i) * handleSize,
                    std::min<size_t>(8, handleSize));
    }
    const VkDeviceAddress alignedBase = alignUp64(sbtBuffer_.address, baseAlignment);
    const VkDeviceSize baseOffset = alignedBase - sbtBuffer_.address;
    auto* base = static_cast<std::uint8_t*>(sbtBuffer_.mapped) + baseOffset;
    std::memset(base, 0, static_cast<size_t>(sbtBuffer_.size - baseOffset));
    std::memcpy(base + sbtRaygenOffset_, handles.data(), handleSize);
    std::memcpy(base + sbtRaygenOffset_ + stride, handles.data() + handleSize, handleSize);
    std::memcpy(base + sbtMissOffset_, handles.data() + handleSize * 2u, handleSize);
    std::memcpy(base + sbtHitOffset_, handles.data() + handleSize * 3u, handleSize);
    vmaFlushAllocation(context_->allocator, sbtBuffer_.allocation, 0, VK_WHOLE_SIZE);
    sbtAddress_ = alignedBase;
    return true;
}

void Renderer::destroySbt() { destroyBuffer(sbtBuffer_); }

bool Renderer::createPipelines() {
    if (!createShaderObject("setup.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT, shaderSetup_) ||
        !createShaderObject("shade.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT, shaderShade_) ||
        !createShaderObject("prepare.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT, shaderPrepare_) ||
        !createShaderObject("restir_initial.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT,
                            shaderRestirInitial_) ||
        !createShaderObject("restir_temporal.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT,
                            shaderRestirTemporal_) ||
        !createShaderObject("restir_spatial.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT,
                            shaderRestirSpatial_) ||
        !createShaderObject("restir_shadow.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT,
                            shaderRestirShadow_) ||
        !createShaderObject("shadow_resolve.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT,
                            shaderShadowResolve_) ||
        !createShaderObject("resolve.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT, shaderResolve_) ||
        !createShaderObject("svgf_temporal.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT,
                            shaderSvgfTemporal_) ||
        !createShaderObject("svgf_variance.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT,
                            shaderSvgfVariance_) ||
        !createShaderObject("atrous.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT, shaderAtrous_) ||
        !createShaderObject("svgf_finalize.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT,
                            shaderFinalize_) ||
        !createShaderObject("composite.comp.spv", VK_SHADER_STAGE_COMPUTE_BIT, shaderComposite_) ||
        !createShaderObject("display.vert.spv", VK_SHADER_STAGE_VERTEX_BIT, shaderDisplayVert_) ||
        !createShaderObject("display.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT, shaderDisplayFrag_)) {
        return false;
    }

    const auto createModule = [&](const char* name, VkShaderModule& module) {
        std::vector<std::uint32_t> code;
        if (!loadShaderModule(name, code)) return false;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size() * sizeof(std::uint32_t);
        info.pCode = code.data();
        if (vkCreateShaderModule(context_->device, &info, nullptr, &module) != VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan 错误: 创建 shader module 失败 %s\n", name);
            return false;
        }
        return true;
    };
    VkShaderModule traceRgen = VK_NULL_HANDLE;
    VkShaderModule shadowRgen = VK_NULL_HANDLE;
    VkShaderModule miss = VK_NULL_HANDLE;
    VkShaderModule chit = VK_NULL_HANDLE;
    if (!createModule("trace.rgen.spv", traceRgen) ||
        !createModule("shadow.rgen.spv", shadowRgen) ||
        !createModule("trace.rmiss.spv", miss) ||
        !createModule("trace.rchit.spv", chit)) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[4]{};
    const VkShaderModule modules[4] = {traceRgen, shadowRgen, miss, chit};
    const VkShaderStageFlagBits stageBits[4] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
    for (int i = 0; i < 4; ++i) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = stageBits[i];
        stages[i].module = modules[i];
        stages[i].pName = "main";
    }
    VkRayTracingShaderGroupCreateInfoKHR groups[4]{};
    for (int i = 0; i < 4; ++i) {
        groups[i].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[i].generalShader = VK_SHADER_UNUSED_KHR;
        groups[i].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[2].generalShader = 2;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[3].closestHitShader = 3;

    VkRayTracingPipelineCreateInfoKHR rtInfo{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    rtInfo.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    rtInfo.stageCount = 4;
    rtInfo.pStages = stages;
    rtInfo.groupCount = 4;
    rtInfo.pGroups = groups;
    rtInfo.maxPipelineRayRecursionDepth = 1;
    rtInfo.layout = pipelineLayout_;
    const VkResult result = vkCreateRayTracingPipelinesKHR(
        context_->device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rtInfo, nullptr, &rtPipeline_);
    vkDestroyShaderModule(context_->device, traceRgen, nullptr);
    vkDestroyShaderModule(context_->device, shadowRgen, nullptr);
    vkDestroyShaderModule(context_->device, miss, nullptr);
    vkDestroyShaderModule(context_->device, chit, nullptr);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 RT pipeline 失败\n");
        return false;
    }
    return createSbt();
}

bool Renderer::buildAccelerationStructures() {
    const std::uint32_t groupCount = static_cast<std::uint32_t>(scene_.groupPositions.size());
    if (groupCount == 0) {
        std::fprintf(stderr, "场景错误: 没有 BLAS 分组\n");
        return false;
    }
    groupVertexOffsets_.resize(groupCount);
    groupIndexOffsets_.resize(groupCount);
    groupIndexCounts_.resize(groupCount);

    VkDeviceSize vertexBytes = 0;
    VkDeviceSize indexBytes = 0;
    for (std::uint32_t i = 0; i < groupCount; ++i) {
        vertexBytes = alignUp64(vertexBytes, 16);
        groupVertexOffsets_[i] = vertexBytes;
        vertexBytes += bytesFor(scene_.groupPositions[i].size() * sizeof(Vec3));
        indexBytes = alignUp64(indexBytes, 16);
        groupIndexOffsets_[i] = indexBytes;
        groupIndexCounts_[i] = static_cast<std::uint32_t>(scene_.groupIndices[i].size());
        indexBytes += bytesFor(scene_.groupIndices[i].size() * sizeof(std::uint32_t));
    }
    if (!createBuffer(vertexBytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      true, asVertexBuffer_)) {
        return false;
    }
    if (!createBuffer(indexBytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     true, asIndexBuffer_)) {
        return false;
    }
    for (std::uint32_t i = 0; i < groupCount; ++i) {
        if (!scene_.groupPositions[i].empty()) {
            std::memcpy(static_cast<std::uint8_t*>(asVertexBuffer_.mapped) + groupVertexOffsets_[i],
                        scene_.groupPositions[i].data(),
                        scene_.groupPositions[i].size() * sizeof(Vec3));
        }
        if (!scene_.groupIndices[i].empty()) {
            std::memcpy(static_cast<std::uint8_t*>(asIndexBuffer_.mapped) + groupIndexOffsets_[i],
                        scene_.groupIndices[i].data(),
                        scene_.groupIndices[i].size() * sizeof(std::uint32_t));
        }
    }
    vmaFlushAllocation(context_->allocator, asVertexBuffer_.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(context_->allocator, asIndexBuffer_.allocation, 0, VK_WHOLE_SIZE);

    std::vector<GpuBuffer> scratch(groupCount);
    std::vector<GpuBuffer> originalBuffers(groupCount);
    std::vector<VkAccelerationStructureKHR> originalAs(groupCount, VK_NULL_HANDLE);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(groupCount);
    std::vector<VkAccelerationStructureGeometryKHR> geometries(groupCount);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(groupCount);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR*> rangePtrs(groupCount);
    std::vector<VkDeviceSize> compactedSizes(groupCount, 0);
    GpuBuffer compactSizeBuffer;
    GpuBuffer tlasScratch;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    const auto cleanup = [&]() {
        for (VkAccelerationStructureKHR as : originalAs) {
            if (as != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(context_->device, as, nullptr);
        }
        for (GpuBuffer& buffer : originalBuffers) destroyBuffer(buffer);
        for (GpuBuffer& buffer : scratch) destroyBuffer(buffer);
        destroyBuffer(compactSizeBuffer);
        destroyBuffer(tlasScratch);
        if (queryPool != VK_NULL_HANDLE) vkDestroyQueryPool(context_->device, queryPool, nullptr);
        if (fence != VK_NULL_HANDLE) vkDestroyFence(context_->device, fence, nullptr);
        return false;
    };

    for (std::uint32_t i = 0; i < groupCount; ++i) {
        const std::uint32_t triCount = groupIndexCounts_[i] / 3u;
        VkAccelerationStructureGeometryTrianglesDataKHR triangles{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = asVertexBuffer_.address + groupVertexOffsets_[i];
        triangles.vertexStride = sizeof(Vec3);
        triangles.maxVertex = static_cast<std::uint32_t>(scene_.groupPositions[i].size()) - 1u;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = asIndexBuffer_.address + groupIndexOffsets_[i];
        VkAccelerationStructureGeometryKHR& geometry = geometries[i];
        geometry = VkAccelerationStructureGeometryKHR{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = triangles;

        VkAccelerationStructureBuildGeometryInfoKHR& build = buildInfos[i];
        build = VkAccelerationStructureBuildGeometryInfoKHR{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometries[i];

        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        const std::uint32_t primCount = triCount;
        vkGetAccelerationStructureBuildSizesKHR(
            context_->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primCount,
            &sizes);
        if (!createBuffer(sizes.accelerationStructureSize,
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          false, originalBuffers[i])) {
            return cleanup();
        }
        VkAccelerationStructureCreateInfoKHR asInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        asInfo.buffer = originalBuffers[i].buffer;
        asInfo.size = sizes.accelerationStructureSize;
        asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(context_->device, &asInfo, nullptr,
                                             &originalAs[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan 错误: 创建 BLAS 失败\n");
            return cleanup();
        }
        const VkDeviceSize scratchAlignment = std::max<VkDeviceSize>(
            1, context_->accelerationStructureProperties
                   .minAccelerationStructureScratchOffsetAlignment);
        if (!createBuffer(sizes.buildScratchSize + scratchAlignment,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          false, scratch[i])) {
            return cleanup();
        }
        build.dstAccelerationStructure = originalAs[i];
        build.scratchData.deviceAddress = alignUp64(scratch[i].address, scratchAlignment);
        ranges[i].primitiveCount = primCount;
        ranges[i].primitiveOffset = 0;
        ranges[i].firstVertex = 0;
        ranges[i].transformOffset = 0;
        rangePtrs[i] = &ranges[i];
    }

    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    queryInfo.queryCount = groupCount;
    if (vkCreateQueryPool(context_->device, &queryInfo, nullptr, &queryPool) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 AS compact query pool 失败\n");
        return cleanup();
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(context_->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        return cleanup();
    }
    const auto submitAndWait = [&]() {
        vkEndCommandBuffer(transientCommand_);
        VkCommandBufferSubmitInfo commandBufferInfo{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandBufferInfo.commandBuffer = transientCommand_;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &commandBufferInfo;
        vkQueueSubmit2(context_->graphicsQueue, 1, &submit, fence);
        vkWaitForFences(context_->device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(context_->device, 1, &fence);
    };
    const auto beginTransient = [&]() {
        vkResetCommandBuffer(transientCommand_, 0);
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(transientCommand_, &beginInfo);
    };
    const auto fullBarrier = [&]() {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(transientCommand_, &dep);
    };

    beginTransient();
    fullBarrier();
    vkCmdBuildAccelerationStructuresKHR(transientCommand_, groupCount, buildInfos.data(),
                                        rangePtrs.data());
    submitAndWait();

    if (!createBuffer(bytesFor(groupCount * 2u * sizeof(VkDeviceSize)),
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, compactSizeBuffer)) {
        return cleanup();
    }
    vkResetQueryPool(context_->device, queryPool, 0, groupCount);
    beginTransient();
    {
        VkMemoryBarrier2 asBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        asBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        asBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &asBarrier;
        vkCmdPipelineBarrier2(transientCommand_, &dep);
    }
    vkCmdWriteAccelerationStructuresPropertiesKHR(
        transientCommand_, groupCount, originalAs.data(),
        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, queryPool, 0);
    fullBarrier();
    vkCmdCopyQueryPoolResults(transientCommand_, queryPool, 0, groupCount,
                              compactSizeBuffer.buffer, 0, 2u * sizeof(VkDeviceSize),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    submitAndWait();

    vmaInvalidateAllocation(context_->allocator, compactSizeBuffer.allocation, 0, VK_WHOLE_SIZE);
    const auto* queryData = static_cast<const std::uint64_t*>(compactSizeBuffer.mapped);
    bool queryAvailable = true;
    for (std::uint32_t i = 0; i < groupCount; ++i) {
        compactedSizes[i] = queryData[i * 2u];
        if (queryData[i * 2u + 1u] == 0u || compactedSizes[i] == 0u ||
            compactedSizes[i] == UINT64_MAX) {
            queryAvailable = false;
        }
    }
    if (!queryAvailable) {
        // 该 AMD 驱动对 compacted size 查询返回 unavailable；不执行 compact copy，
        // 直接使用原始 BLAS，避免向尺寸未知的目标 AS 写入。
        blas_ = originalAs;
        blasBuffers_ = originalBuffers;
        for (std::uint32_t i = 0; i < groupCount; ++i) {
            originalAs[i] = VK_NULL_HANDLE;
            originalBuffers[i] = GpuBuffer{};
        }
    } else {
        blas_.assign(groupCount, VK_NULL_HANDLE);
        blasBuffers_.assign(groupCount, GpuBuffer{});
        for (std::uint32_t i = 0; i < groupCount; ++i) {
            if (!createBuffer(compactedSizes[i],
                              VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              false, blasBuffers_[i])) {
                return cleanup();
            }
            VkAccelerationStructureCreateInfoKHR asInfo{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            asInfo.buffer = blasBuffers_[i].buffer;
            asInfo.size = compactedSizes[i];
            asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            if (vkCreateAccelerationStructureKHR(context_->device, &asInfo, nullptr, &blas_[i]) !=
                VK_SUCCESS) {
                std::fprintf(stderr, "Vulkan 错误: 创建 compacted BLAS 失败\n");
                return cleanup();
            }
        }
    }

    std::vector<VkAccelerationStructureInstanceKHR> instances(groupCount);
    for (std::uint32_t i = 0; i < groupCount; ++i) {
        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addrInfo.accelerationStructure = blas_[i];
        instances[i] = VkAccelerationStructureInstanceKHR{};
        instances[i].transform.matrix[0][0] = 1.0f;
        instances[i].transform.matrix[1][1] = 1.0f;
        instances[i].transform.matrix[2][2] = 1.0f;
        instances[i].instanceCustomIndex = i;
        instances[i].mask = 0xFF;
        instances[i].instanceShaderBindingTableRecordOffset = 0;
        instances[i].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instances[i].accelerationStructureReference =
            vkGetAccelerationStructureDeviceAddressKHR(context_->device, &addrInfo);
    }
    if (!createBuffer(bytesFor(instances.size() * sizeof(VkAccelerationStructureInstanceKHR)),
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      true, tlasInstanceBuffer_)) {
        return cleanup();
    }
    std::memcpy(tlasInstanceBuffer_.mapped, instances.data(),
                instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    vmaFlushAllocation(context_->allocator, tlasInstanceBuffer_.allocation, 0, VK_WHOLE_SIZE);

    VkAccelerationStructureGeometryInstancesDataKHR instancesData{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = tlasInstanceBuffer_.address;
    VkAccelerationStructureGeometryKHR tlasGeometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = instancesData;
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.geometryCount = 1;
    tlasBuild.pGeometries = &tlasGeometry;
    const std::uint32_t instanceCount = groupCount;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(
        context_->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
        &instanceCount, &tlasSizes);
    if (!createBuffer(tlasSizes.accelerationStructureSize,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      false, tlasBuffer_)) {
        return cleanup();
    }
    VkAccelerationStructureCreateInfoKHR tlasInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    tlasInfo.buffer = tlasBuffer_.buffer;
    tlasInfo.size = tlasSizes.accelerationStructureSize;
    tlasInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(context_->device, &tlasInfo, nullptr, &tlas_) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: 创建 TLAS 失败\n");
        return cleanup();
    }
    const VkDeviceSize tlasScratchAlignment = std::max<VkDeviceSize>(
        1, context_->accelerationStructureProperties
               .minAccelerationStructureScratchOffsetAlignment);
    if (!createBuffer(tlasSizes.buildScratchSize + tlasScratchAlignment,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      false, tlasScratch)) {
        return cleanup();
    }
    tlasBuild.dstAccelerationStructure = tlas_;
    tlasBuild.scratchData.deviceAddress =
        alignUp64(tlasScratch.address, tlasScratchAlignment);
    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = instanceCount;
    VkAccelerationStructureBuildRangeInfoKHR* tlasRangePtr = &tlasRange;

    beginTransient();
    fullBarrier();
    if (queryAvailable) {
        for (std::uint32_t i = 0; i < groupCount; ++i) {
            VkCopyAccelerationStructureInfoKHR copy{
                VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
            copy.src = originalAs[i];
            copy.dst = blas_[i];
            copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkCmdCopyAccelerationStructureKHR(transientCommand_, &copy);
        }
    }
    fullBarrier();
    vkCmdBuildAccelerationStructuresKHR(transientCommand_, 1, &tlasBuild, &tlasRangePtr);
    fullBarrier();
    submitAndWait();

    for (std::uint32_t i = 0; i < groupCount; ++i) {
        if (originalAs[i] != VK_NULL_HANDLE) {
            vkDestroyAccelerationStructureKHR(context_->device, originalAs[i], nullptr);
            originalAs[i] = VK_NULL_HANDLE;
        }
        destroyBuffer(originalBuffers[i]);
        destroyBuffer(scratch[i]);
    }
    destroyBuffer(tlasScratch);
    vkDestroyQueryPool(context_->device, queryPool, nullptr);
    vkDestroyFence(context_->device, fence, nullptr);
    return true;
}

void Renderer::destroyAccelerationStructures() {
    for (VkAccelerationStructureKHR as : blas_) {
        if (as != VK_NULL_HANDLE) vkDestroyAccelerationStructureKHR(context_->device, as, nullptr);
    }
    blas_.clear();
    for (GpuBuffer& buffer : blasBuffers_) destroyBuffer(buffer);
    blasBuffers_.clear();
    if (tlas_ != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(context_->device, tlas_, nullptr);
        tlas_ = VK_NULL_HANDLE;
    }
    destroyBuffer(tlasBuffer_);
    destroyBuffer(tlasInstanceBuffer_);
    destroyBuffer(asVertexBuffer_);
    destroyBuffer(asIndexBuffer_);
}

} // namespace vpt
