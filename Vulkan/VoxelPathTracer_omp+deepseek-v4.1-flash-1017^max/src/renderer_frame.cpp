#include "renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vpt {

namespace {

constexpr std::uint32_t kQueueTraceOffset = 0;
constexpr std::uint32_t kQueueDispatchOffset = 16;
constexpr std::uint32_t kShadowTraceOffset = 32;
constexpr std::uint32_t kShadowDispatchOffset = 48;

} // namespace

void Renderer::barrier(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = srcStage;
    memory.srcAccessMask = srcAccess;
    memory.dstStageMask = dstStage;
    memory.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(activeCommand_, &dep);
}

void Renderer::imageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    imageBarrier.srcStageMask = srcStage;
    imageBarrier.srcAccessMask = srcAccess;
    imageBarrier.dstStageMask = dstStage;
    imageBarrier.dstAccessMask = dstAccess;
    imageBarrier.oldLayout = oldLayout;
    imageBarrier.newLayout = newLayout;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = image;
    imageBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &imageBarrier;
    vkCmdPipelineBarrier2(activeCommand_, &dep);
}

void Renderer::clearBuffer(const GpuBuffer& buffer) {
    vkCmdFillBuffer(activeCommand_, buffer.buffer, 0, buffer.size, 0);
}

void Renderer::dispatchCompute(VkShaderEXT shader, const PushConstantsGpu& pc, std::uint32_t gx,
                               std::uint32_t gy) {
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    vkCmdBindShadersEXT(activeCommand_, 1, &stage, &shader);
    vkCmdPushConstants(activeCommand_, pipelineLayout_, VK_SHADER_STAGE_ALL, 0,
                       sizeof(PushConstantsGpu), &pc);
    vkCmdDispatch(activeCommand_, gx, gy, 1);
}

void Renderer::dispatchComputeIndirect(VkShaderEXT shader, const PushConstantsGpu& pc,
                                       VkDeviceAddress argsAddress) {
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    vkCmdBindShadersEXT(activeCommand_, 1, &stage, &shader);
    vkCmdPushConstants(activeCommand_, pipelineLayout_, VK_SHADER_STAGE_ALL, 0,
                       sizeof(PushConstantsGpu), &pc);
    const VkDeviceSize offset = argsAddress - buffers_[bind::Indirect].address;
    vkCmdDispatchIndirect(activeCommand_, buffers_[bind::Indirect].buffer, offset);
}

void Renderer::traceRaysDirect(std::uint32_t record, std::uint32_t width, std::uint32_t height) {
    VkStridedDeviceAddressRegionKHR raygen{};
    raygen.deviceAddress = sbtAddress_ + sbtRaygenOffset_ + record * sbtRaygenStride_;
    raygen.stride = sbtRaygenStride_;
    raygen.size = sbtRaygenStride_;
    VkStridedDeviceAddressRegionKHR miss{};
    miss.deviceAddress = sbtAddress_ + sbtMissOffset_;
    miss.stride = sbtMissStride_;
    miss.size = sbtMissStride_;
    VkStridedDeviceAddressRegionKHR hit{};
    hit.deviceAddress = sbtAddress_ + sbtHitOffset_;
    hit.stride = sbtHitStride_;
    hit.size = sbtHitStride_;
    VkStridedDeviceAddressRegionKHR callable{};
    vkCmdTraceRaysKHR(activeCommand_, &raygen, &miss, &hit, &callable, width, height, 1);
}

void Renderer::traceRaysIndirect(std::uint32_t record, VkDeviceAddress argsAddress) {
    VkStridedDeviceAddressRegionKHR raygen{};
    raygen.deviceAddress = sbtAddress_ + sbtRaygenOffset_ + record * sbtRaygenStride_;
    raygen.stride = sbtRaygenStride_;
    raygen.size = sbtRaygenStride_;
    VkStridedDeviceAddressRegionKHR miss{};
    miss.deviceAddress = sbtAddress_ + sbtMissOffset_;
    miss.stride = sbtMissStride_;
    miss.size = sbtMissStride_;
    VkStridedDeviceAddressRegionKHR hit{};
    hit.deviceAddress = sbtAddress_ + sbtHitOffset_;
    hit.stride = sbtHitStride_;
    hit.size = sbtHitStride_;
    VkStridedDeviceAddressRegionKHR callable{};
    vkCmdTraceRaysIndirectKHR(activeCommand_, &raygen, &miss, &hit, &callable, argsAddress);
}

void Renderer::bindPassResources(VkCommandBuffer cmd, FrameSlot& slot) {
    VkDescriptorBufferBindingInfoEXT binding{
        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
    binding.address = slot.descriptors.address;
    binding.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
    vkCmdBindDescriptorBuffersEXT(cmd, 1, &binding);
    VkDeviceSize offset = 0;
    uint32_t bufferIndex = 0;
    vkCmdSetDescriptorBufferOffsetsEXT(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1,
                                       &bufferIndex, &offset);
    vkCmdSetDescriptorBufferOffsetsEXT(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                       &bufferIndex, &offset);
    vkCmdSetDescriptorBufferOffsetsEXT(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                       pipelineLayout_, 0, 1, &bufferIndex, &offset);
}

FrameStatus Renderer::renderFrame(const CameraState& camera, bool resetCamera) {
    if (fatal_ || swapchain_ == VK_NULL_HANDLE) return FrameStatus::Fatal;
    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSizeInPixels(window_, &windowWidth, &windowHeight);
    if (windowWidth <= 0 || windowHeight <= 0 ||
        (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0) {
        statsReset_ = true;
        return FrameStatus::Skipped;
    }
    if (resizeRequested_ || static_cast<std::uint32_t>(windowWidth) != extent_.width ||
        static_cast<std::uint32_t>(windowHeight) != extent_.height) {
        resizeRequested_ = false;
        vkDeviceWaitIdle(context_->device);
        destroySizedResources();
        destroySwapchain();
        if (!createSwapchain()) return FrameStatus::Fatal;
        if (!createSizedResources()) {
            fatal_ = true;
            return FrameStatus::Fatal;
        }
        if (!createDescriptors(false)) {
            fatal_ = true;
            return FrameStatus::Fatal;
        }
        resetHistory_ = true;
        statsReset_ = true;
    }

    const std::uint32_t slotIndex = frameIndex_ & 1u;
    FrameSlot& slot = slots_[slotIndex];
    if (slot.lastSignal != 0) {
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timeline_;
        waitInfo.pValues = &slot.lastSignal;
        if (vkWaitSemaphores(context_->device, &waitInfo, UINT64_MAX) != VK_SUCCESS) {
            fatal_ = true;
            return FrameStatus::Fatal;
        }
    }

    std::uint32_t imageIndex = 0;
    const VkResult acquire = vkAcquireNextImageKHR(context_->device, swapchain_, UINT64_MAX,
                                                   slot.acquire, VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        resizeRequested_ = true;
        statsReset_ = true;
        return FrameStatus::Skipped;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "Vulkan 错误: acquire 交换链图像失败 (%d)\n", static_cast<int>(acquire));
        fatal_ = true;
        return FrameStatus::Fatal;
    }

    const float aspect = static_cast<float>(extent_.width) / static_cast<float>(extent_.height);
    const Vec3 forward(std::cos(camera.pitch) * std::sin(camera.yaw),
                       std::sin(camera.pitch),
                       -std::cos(camera.pitch) * std::cos(camera.yaw));
    const Vec3 worldUp(0.0f, 1.0f, 0.0f);
    const Vec3 right = normalize(cross(forward, worldUp));
    const Vec3 up = cross(right, forward);
    const Mat4 view = lookAt(camera.position, camera.position + forward, up);
    const Mat4 projection = perspective(60.0f * kPi / 180.0f, aspect, kNearPlane, 1000.0f);
    const Mat4 viewProj = multiply(projection, view);

    bool cameraMoved = !hasPreviousFrame_;
    if (hasPreviousFrame_) {
        const Vec3 delta = camera.position - previousCamera_.position;
        if (length(delta) > 1e-5f || std::abs(camera.yaw - previousCamera_.yaw) > 1e-6f ||
            std::abs(camera.pitch - previousCamera_.pitch) > 1e-6f) {
            cameraMoved = true;
        }
    }
    const bool reset = resetHistory_ || resetCamera || !hasPreviousFrame_;
    if (reset) cameraMoved = true;

    CameraGpu cameraData{};
    std::memcpy(cameraData.viewProjCurr, viewProj.m, sizeof(viewProj.m));
    std::memcpy(cameraData.viewProjPrev, previousViewProj_.m, sizeof(previousViewProj_.m));
    cameraData.position[0] = camera.position.x;
    cameraData.position[1] = camera.position.y;
    cameraData.position[2] = camera.position.z;
    cameraData.position[3] = std::tan(60.0f * kPi / 360.0f);
    cameraData.right[0] = right.x;
    cameraData.right[1] = right.y;
    cameraData.right[2] = right.z;
    cameraData.right[3] = aspect;
    cameraData.up[0] = up.x;
    cameraData.up[1] = up.y;
    cameraData.up[2] = up.z;
    cameraData.up[3] = static_cast<float>(frameIndex_ & 1u);
    cameraData.forward[0] = forward.x;
    cameraData.forward[1] = forward.y;
    cameraData.forward[2] = forward.z;
    cameraData.forward[3] = 0.0f;
    cameraData.extent[0] = static_cast<float>(extent_.width);
    cameraData.extent[1] = static_cast<float>(extent_.height);
    cameraData.extent[2] = 1.0f / static_cast<float>(extent_.width);
    cameraData.extent[3] = 1.0f / static_cast<float>(extent_.height);
    cameraData.flags[0] = cameraMoved ? 1.0f : 0.0f;
    cameraData.flags[1] = reset ? 1.0f : 0.0f;
    cameraData.glassMin[0] = scene_.glassMin.x;
    cameraData.glassMin[1] = scene_.glassMin.y;
    cameraData.glassMin[2] = scene_.glassMin.z;
    cameraData.glassMin[3] = 0.0f;
    cameraData.glassMax[0] = scene_.glassMax.x;
    cameraData.glassMax[1] = scene_.glassMax.y;
    cameraData.glassMax[2] = scene_.glassMax.z;
    cameraData.glassMax[3] = static_cast<float>(scene_.mipLevels - 1u);
    if (frameIndex_ > 0) {
        const std::uint64_t previousValue = frameIndex_;
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timeline_;
        waitInfo.pValues = &previousValue;
        vkWaitSemaphores(context_->device, &waitInfo, UINT64_MAX);
    }
    std::memcpy(slots_[0].camera.mapped, &cameraData, sizeof(cameraData));
    vmaFlushAllocation(context_->allocator, slots_[0].camera.allocation, 0, VK_WHOLE_SIZE);

    vkResetCommandBuffer(slot.command, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(slot.command, &beginInfo) != VK_SUCCESS) {
        fatal_ = true;
        return FrameStatus::Fatal;
    }
    activeCommand_ = slot.command;
    bindPassResources(slot.command, slots_[0]);
    recordFrame(slot.command, imageIndex, cameraData);
    if (vkEndCommandBuffer(slot.command) != VK_SUCCESS) {
        fatal_ = true;
        return FrameStatus::Fatal;
    }

    VkSemaphoreSubmitInfo waits[2]{};
    std::uint32_t waitCount = 0;
    if (frameIndex_ > 0) {
        waits[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[waitCount].semaphore = timeline_;
        waits[waitCount].value = frameIndex_;
        waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++waitCount;
    }
    waits[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[waitCount].semaphore = slot.acquire;
    waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    ++waitCount;

    VkSemaphoreSubmitInfo signals[2]{};
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = renderFinished_[imageIndex];
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = timeline_;
    signals[1].value = frameIndex_ + 1u;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    commandInfo.commandBuffer = slot.command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = waitCount;
    submit.pWaitSemaphoreInfos = waits;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos = signals;
    if (vkQueueSubmit2(context_->graphicsQueue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: queue submit 失败\n");
        fatal_ = true;
        return FrameStatus::Fatal;
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const VkResult presentResult =
        context_->presentQueue == context_->graphicsQueue
            ? vkQueuePresentKHR(context_->presentQueue, &present)
            : vkQueuePresentKHR(context_->presentQueue, &present);

    slot.lastSignal = frameIndex_ + 1u;
    ++frameIndex_;
    previousViewProj_ = viewProj;
    previousCamera_ = camera;
    hasPreviousFrame_ = true;
    resetHistory_ = false;

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
        resizeRequested_ = true;
        statsReset_ = true;
        return FrameStatus::Skipped;
    }
    if (presentResult == VK_SUBOPTIMAL_KHR) {
        resizeRequested_ = true;
        statsReset_ = true;
    } else if (presentResult != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: present 失败 (%d)\n", static_cast<int>(presentResult));
        fatal_ = true;
        return FrameStatus::Fatal;
    }
    return FrameStatus::Presented;
}

void Renderer::recordFrame(VkCommandBuffer cmd, std::uint32_t imageIndex,
                           const CameraGpu& cameraData) {
    (void)cameraData;
    const std::uint32_t width = extent_.width;
    const std::uint32_t height = extent_.height;
    const std::uint32_t groupX = (width + 7u) / 8u;
    const std::uint32_t groupY = (height + 7u) / 8u;
    const VkDeviceAddress indirectBase = buffers_[bind::Indirect].address;

    clearBuffer(buffers_[bind::Counters]);
    clearBuffer(buffers_[bind::BranchContrib]);
    clearBuffer(buffers_[bind::DomainRadiance]);
    clearBuffer(buffers_[bind::Analytic]);
    clearBuffer(buffers_[bind::RestirOut]);
    barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipeline_);

    PushConstantsGpu pc{};
    pc.pass = pass::Setup;
    pc.frame = frameIndex_;
    pc.width = width;
    pc.height = height;
    dispatchCompute(shaderSetup_, pc, groupX, groupY);
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    pc = PushConstantsGpu{};
    pc.pass = pass::Prepare;
    pc.frame = frameIndex_;
    pc.width = width;
    pc.height = height;
    pc.queue = 0;
    dispatchCompute(shaderPrepare_, pc, 1, 1);
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);

    pc = PushConstantsGpu{};
    pc.pass = 0u;
    pc.frame = frameIndex_;
    pc.width = width;
    pc.height = height;
    pc.queue = 0;
    pc.flags = 1;
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
    traceRaysDirect(0, width, height);
    barrier(VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    pc = PushConstantsGpu{};
    pc.pass = pass::Shade;
    pc.frame = frameIndex_;
    pc.width = width;
    pc.height = height;
    pc.queue = 0;
    pc.flags = 1;
    dispatchComputeIndirect(shaderShade_, pc, indirectBase + kQueueDispatchOffset);
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    const auto restirPass = [&](std::uint32_t passId, VkShaderEXT shader) {
        PushConstantsGpu local{};
        local.pass = passId;
        local.frame = frameIndex_;
        local.width = width;
        local.height = height;
        dispatchCompute(shader, local, groupX, groupY);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    };
    restirPass(pass::RestirInitial, shaderRestirInitial_);
    restirPass(pass::RestirTemporal, shaderRestirTemporal_);
    restirPass(pass::RestirSpatial, shaderRestirSpatial_);
    restirPass(pass::RestirShadowGen, shaderRestirShadow_);

    const auto prepareQueue = [&](std::uint32_t queue) {
        PushConstantsGpu local{};
        local.pass = pass::Prepare;
        local.frame = frameIndex_;
        local.width = width;
        local.height = height;
        local.queue = queue;
        dispatchCompute(shaderPrepare_, local, 1, 1);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    };
    const auto shadowPass = [&]() {
        PushConstantsGpu local{};
        local.pass = 0u;
        local.frame = frameIndex_;
        local.width = width;
        local.height = height;
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(local), &local);
        traceRaysIndirect(1, indirectBase + kShadowTraceOffset);
        barrier(VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        PushConstantsGpu resolve{};
        resolve.pass = pass::ShadowResolve;
        resolve.frame = frameIndex_;
        resolve.width = width;
        resolve.height = height;
        dispatchComputeIndirect(shaderShadowResolve_, resolve,
                                indirectBase + kShadowDispatchOffset);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    };

    prepareQueue(1);
    shadowPass();

    for (std::uint32_t bounce = 1; bounce <= 8u; ++bounce) {
        const std::uint32_t queue = bounce & 1u;
        PushConstantsGpu trace{};
        trace.pass = 0u;
        trace.frame = frameIndex_;
        trace.width = width;
        trace.height = height;
        trace.queue = queue;
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(trace), &trace);
        traceRaysIndirect(0, indirectBase + kQueueTraceOffset);
        barrier(VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

        PushConstantsGpu shade{};
        shade.pass = pass::Shade;
        shade.frame = frameIndex_;
        shade.width = width;
        shade.height = height;
        shade.queue = queue;
        shade.flags = 0;
        dispatchComputeIndirect(shaderShade_, shade, indirectBase + kQueueDispatchOffset);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

        prepareQueue((bounce + 1u) & 1u);
        shadowPass();
    }

    PushConstantsGpu resolve{};
    resolve.pass = pass::Resolve;
    resolve.frame = frameIndex_;
    resolve.width = width;
    resolve.height = height;
    dispatchCompute(shaderResolve_, resolve, groupX, groupY);
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    for (std::uint32_t domain = 0; domain < domain::Count; ++domain) {
        PushConstantsGpu temporal{};
        temporal.pass = pass::SvgfTemporal;
        temporal.frame = frameIndex_;
        temporal.width = width;
        temporal.height = height;
        temporal.domainIndex = domain;
        dispatchCompute(shaderSvgfTemporal_, temporal, groupX, groupY);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

        PushConstantsGpu variance{};
        variance.pass = pass::SvgfVariance;
        variance.frame = frameIndex_;
        variance.width = width;
        variance.height = height;
        variance.domainIndex = domain;
        dispatchCompute(shaderSvgfVariance_, variance, groupX, groupY);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

        for (std::uint32_t iteration = 0; iteration < kAtrousIterations; ++iteration) {
            PushConstantsGpu atrous{};
            atrous.pass = pass::Atrous;
            atrous.frame = frameIndex_;
            atrous.width = width;
            atrous.height = height;
            atrous.domainIndex = domain;
            atrous.iteration = iteration;
            dispatchCompute(shaderAtrous_, atrous, groupX, groupY);
            barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        }

        PushConstantsGpu finalize{};
        finalize.pass = pass::Finalize;
        finalize.frame = frameIndex_;
        finalize.width = width;
        finalize.height = height;
        finalize.domainIndex = domain;
        dispatchCompute(shaderFinalize_, finalize, groupX, groupY);
        barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    PushConstantsGpu composite{};
    composite.pass = pass::Composite;
    composite.frame = frameIndex_;
    composite.width = width;
    composite.height = height;
    dispatchCompute(shaderComposite_, composite, groupX, groupY);
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    imageBarrier(swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colorAttachment{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAttachment.imageView = swapchainViews_[imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0, 0}, extent_};
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;
    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent_.width);
    viewport.height = static_cast<float>(extent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewportWithCount(cmd, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissorWithCount(cmd, 1, &scissor);
    vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
    vkCmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_FILL);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthTestEnable(cmd, VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
    vkCmdSetStencilTestEnable(cmd, VK_FALSE);
    vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    vkCmdSetDepthClampEnableEXT(cmd, VK_FALSE);
    vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);
    vkCmdSetRasterizationSamplesEXT(cmd, VK_SAMPLE_COUNT_1_BIT);
    VkSampleMask sampleMask = 0xFFFFFFFFu;
    vkCmdSetSampleMaskEXT(cmd, VK_SAMPLE_COUNT_1_BIT, &sampleMask);
    vkCmdSetAlphaToCoverageEnableEXT(cmd, VK_FALSE);
    VkBool32 blendEnable = VK_FALSE;
    vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &blendEnable);
    VkColorBlendEquationEXT blendEquation{};
    blendEquation.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendEquation.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendEquation.colorBlendOp = VK_BLEND_OP_ADD;
    blendEquation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendEquation.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendEquation.alphaBlendOp = VK_BLEND_OP_ADD;
    vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &blendEquation);
    VkColorComponentFlags colorMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &colorMask);
    vkCmdSetLogicOpEnableEXT(cmd, VK_FALSE);
    vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
    vkCmdSetLineWidth(cmd, 1.0f);
    const float blendConstants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    vkCmdSetBlendConstants(cmd, blendConstants);

    VkShaderStageFlagBits stages[5] = {
        VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT};
    VkShaderEXT shaders[5] = {shaderDisplayVert_, VK_NULL_HANDLE, VK_NULL_HANDLE,
                              VK_NULL_HANDLE, shaderDisplayFrag_};
    vkCmdBindShadersEXT(cmd, 5, stages, shaders);
    PushConstantsGpu displayConstants{};
    displayConstants.frame = frameIndex_;
    displayConstants.width = width;
    displayConstants.height = height;
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_ALL, 0, sizeof(displayConstants),
                       &displayConstants);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    imageBarrier(swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
}

} // namespace vpt
