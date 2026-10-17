// 每帧记录、相机/输入、提交/呈现、统计与销毁。
#pragma once

#include "rae_renderer_resources.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

namespace rae {

constexpr VkDeviceSize kIndirectPathTrace = 0;
constexpr VkDeviceSize kIndirectShadowTrace = 16;
constexpr VkDeviceSize kIndirectShade = 32;
constexpr VkDeviceSize kIndirectShadowResolve = 48;

constexpr VkShaderStageFlags kRtStageMask = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                            VK_SHADER_STAGE_MISS_BIT_KHR |
                                            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                            VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

inline void Renderer::bindPass(VkCommandBuffer cmd, const Pass& pass, VkPipelineBindPoint bindPoint,
                               const PushConstants& pc, bool push) {
    uint32_t bufferIndex = 0;
    VkDeviceSize offset = pass.setOffset;
    if (bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE) {
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
        vkCmdBindShadersEXT(cmd, 1, &stage, &pass.shader);
        vkCmdSetDescriptorBufferOffsetsEXT(cmd, bindPoint, pass.layout, 0, 1, &bufferIndex, &offset);
        if (push) {
            vkCmdPushConstants(cmd, pass.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        }
    } else if (bindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
        vkCmdBindPipeline(cmd, bindPoint, rtPipeline_);
        vkCmdSetDescriptorBufferOffsetsEXT(cmd, bindPoint, pass.layout, 0, 1, &bufferIndex, &offset);
        if (push) {
            vkCmdPushConstants(cmd, pass.layout, kRtStageMask, 0, sizeof(pc), &pc);
        }
    }
}

inline void Renderer::dispatchCompute(VkCommandBuffer cmd, const Pass& pass, const PushConstants& pc,
                                      uint32_t count) {
    if (count == 0) {
        return;
    }
    bindPass(cmd, pass, VK_PIPELINE_BIND_POINT_COMPUTE, pc, true);
    const uint32_t groups = (count + kThreadsPerGroup - 1) / kThreadsPerGroup;
    const uint32_t gx = std::max(1u, std::min(groups, maxGroupsX_));
    const uint32_t gy = std::max(1u, std::min((groups + gx - 1) / gx, maxGroupsY_));
    const uint32_t gz = std::max(1u, (groups + gx * gy - 1) / (gx * gy));
    vkCmdDispatch(cmd, gx, gy, gz);
}

inline void Renderer::dispatchComputeIndirect(VkCommandBuffer cmd, const Pass& pass,
                                              const PushConstants& pc, VkDeviceSize offset) {
    bindPass(cmd, pass, VK_PIPELINE_BIND_POINT_COMPUTE, pc, true);
    vkCmdDispatchIndirect(cmd, indirectBuffer_.buffer, offset);
}

inline void Renderer::traceRaysDirect(VkCommandBuffer cmd, const PushConstants& pc, uint32_t width,
                                      uint32_t height, uint32_t depth, uint32_t raygenIndex) {
    Pass rtPass;
    rtPass.layout = rtLayout_;
    rtPass.setOffset = rtSetOffset_;
    bindPass(cmd, rtPass, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pc, true);
    VkStridedDeviceAddressRegionKHR raygen = sbt_.raygen;
    raygen.deviceAddress += static_cast<VkDeviceAddress>(raygenIndex) * raygen.stride;
    raygen.size = raygen.stride;
    const VkStridedDeviceAddressRegionKHR empty{0, 0, 0};
    vkCmdTraceRaysKHR(cmd, &raygen, &sbt_.miss, &sbt_.hit, &empty, width, height, depth);
}

inline void Renderer::traceRaysIndirect(VkCommandBuffer cmd, const PushConstants& pc,
                                        VkDeviceAddress indirectAddress, uint32_t raygenIndex) {
    Pass rtPass;
    rtPass.layout = rtLayout_;
    rtPass.setOffset = rtSetOffset_;
    bindPass(cmd, rtPass, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pc, true);
    VkStridedDeviceAddressRegionKHR raygen = sbt_.raygen;
    raygen.deviceAddress += static_cast<VkDeviceAddress>(raygenIndex) * raygen.stride;
    raygen.size = raygen.stride;
    const VkStridedDeviceAddressRegionKHR empty{0, 0, 0};
    vkCmdTraceRaysIndirectKHR(cmd, &raygen, &sbt_.miss, &sbt_.hit, &empty, indirectAddress);
}

inline void Renderer::setDisplayDynamicState(VkCommandBuffer cmd) {
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent_.width),
                        static_cast<float>(extent_.height), 0.0f, 1.0f};
    vkCmdSetViewportWithCount(cmd, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissorWithCount(cmd, 1, &scissor);
    vkCmdSetRasterizerDiscardEnableEXT(cmd, VK_FALSE);
    vkCmdSetPrimitiveTopologyEXT(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetCullModeEXT(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFaceEXT(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthTestEnableEXT(cmd, VK_FALSE);
    vkCmdSetDepthWriteEnableEXT(cmd, VK_FALSE);
    vkCmdSetDepthCompareOpEXT(cmd, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBoundsTestEnableEXT(cmd, VK_FALSE);
    vkCmdSetDepthBounds(cmd, 0.0f, 1.0f);
    vkCmdSetStencilTestEnableEXT(cmd, VK_FALSE);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFFu);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFFu);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0u);
    vkCmdSetRasterizationSamplesEXT(cmd, VK_SAMPLE_COUNT_1_BIT);
    const VkSampleMask sampleMask = 0xFFFFFFFFu;
    vkCmdSetSampleMaskEXT(cmd, VK_SAMPLE_COUNT_1_BIT, &sampleMask);
    vkCmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_FILL);
    vkCmdSetAlphaToCoverageEnableEXT(cmd, VK_FALSE);
    vkCmdSetLogicOpEnableEXT(cmd, VK_FALSE);
    vkCmdSetDepthClampEnableEXT(cmd, VK_FALSE);
    vkCmdSetDepthBiasEnableEXT(cmd, VK_FALSE);
    vkCmdSetPrimitiveRestartEnableEXT(cmd, VK_FALSE);
    vkCmdSetLineWidth(cmd, 1.0f);
    vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);
    const float blendConstants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    vkCmdSetBlendConstants(cmd, blendConstants);
    const VkBool32 blendEnable = VK_FALSE;
    vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &blendEnable);
    VkColorBlendEquationEXT equation{};
    equation.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    equation.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    equation.colorBlendOp = VK_BLEND_OP_ADD;
    equation.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    equation.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    equation.alphaBlendOp = VK_BLEND_OP_ADD;
    vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &equation);
    const VkColorComponentFlags colorMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &colorMask);
    vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
}

inline void Renderer::drawFullscreen(VkCommandBuffer cmd, VkImage swapImage, VkImageView swapView,
                                     const PushConstants& pc) {
    transitionImage(cmd, swapImage, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1, 1);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = swapView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);

    VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkShaderEXT shaders[2] = {displayVert_, displayFrag_};
    vkCmdBindShadersEXT(cmd, 2, stages, shaders);
    uint32_t bufferIndex = 0;
    VkDeviceSize offset = displaySetOffset_;
    vkCmdSetDescriptorBufferOffsetsEXT(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, displayLayout_, 0, 1,
                                       &bufferIndex, &offset);
    vkCmdPushConstants(cmd, displayLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pc), &pc);
    setDisplayDynamicState(cmd);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    transitionImage(cmd, swapImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, 1, 1);
}

inline void Renderer::updateGlobals() {
    if (globalsBufferForWrite_.mapped == nullptr) {
        return;
    }
    GpuGlobals g{};
    const Camera& cur = camera_;
    const Camera& prev = prevCameraValid_ ? prevCamera_ : camera_;
    g.camPos[0] = cur.pos.x;
    g.camPos[1] = cur.pos.y;
    g.camPos[2] = cur.pos.z;
    g.camPos[3] = cur.tanHalfFovY;
    g.camFwd[0] = cur.forward.x;
    g.camFwd[1] = cur.forward.y;
    g.camFwd[2] = cur.forward.z;
    g.camFwd[3] = cur.aspect;
    g.camRight[0] = cur.right.x;
    g.camRight[1] = cur.right.y;
    g.camRight[2] = cur.right.z;
    g.camUp[0] = cur.up.x;
    g.camUp[1] = cur.up.y;
    g.camUp[2] = cur.up.z;
    g.prevCamPos[0] = prev.pos.x;
    g.prevCamPos[1] = prev.pos.y;
    g.prevCamPos[2] = prev.pos.z;
    g.prevCamPos[3] = prev.tanHalfFovY;
    g.prevCamFwd[0] = prev.forward.x;
    g.prevCamFwd[1] = prev.forward.y;
    g.prevCamFwd[2] = prev.forward.z;
    g.prevCamFwd[3] = prev.aspect;
    g.prevCamRight[0] = prev.right.x;
    g.prevCamRight[1] = prev.right.y;
    g.prevCamRight[2] = prev.right.z;
    g.prevCamUp[0] = prev.up.x;
    g.prevCamUp[1] = prev.up.y;
    g.prevCamUp[2] = prev.up.z;
    g.extent[0] = static_cast<float>(extent_.width);
    g.extent[1] = static_cast<float>(extent_.height);
    g.extent[2] = 1.0f / static_cast<float>(extent_.width);
    g.extent[3] = 1.0f / static_cast<float>(extent_.height);
    g.glassMin[0] = scene_.glassMin.x;
    g.glassMin[1] = scene_.glassMin.y;
    g.glassMin[2] = scene_.glassMin.z;
    g.glassMin[3] = static_cast<float>(scene_.glassMaterial);
    g.glassMax[0] = scene_.glassMax.x;
    g.glassMax[1] = scene_.glassMax.y;
    g.glassMax[2] = scene_.glassMax.z;
    g.glassMax[3] = scene_.totalLightPower;
    const MaterialDesc& glassMat = scene_.materials[scene_.glassMaterial];
    g.glassAbsorption[0] = glassMat.absorption.x;
    g.glassAbsorption[1] = glassMat.absorption.y;
    g.glassAbsorption[2] = glassMat.absorption.z;
    g.glassAbsorption[3] = static_cast<float>(scene_.lights.size());
    const bool historyAvailable = prevCameraValid_ && !historyReset_ && !paused_;
    const bool cameraStatic = historyAvailable && sameCamera(camera_, prevCamera_);
    // x=历史可用, y=历史有效（R/cut 时清零）, z=无 jitter 相机静止。透射域只在 z 有效时复用历史。
    g.flags[0] = historyAvailable ? 1u : 0u;
    g.flags[1] = historyAvailable ? 1u : 0u;
    g.flags[2] = cameraStatic ? 1u : 0u;
    g.flags[3] = frameCounter_;
    g.misc[0] = maxGroupsX_;
    g.misc[1] = maxGroupsY_;
    g.misc[2] = maxGroupsZ_;
    g.misc[3] = 0;
    auto* dst = reinterpret_cast<GpuGlobals*>(static_cast<uint8_t*>(globalsBufferForWrite_.mapped) +
                                               static_cast<size_t>(frameSlot_) *
                                                   sizeof(GpuGlobals));
    std::memcpy(dst, &g, sizeof(GpuGlobals));
    vmaFlushAllocation(allocator_, globalsBufferForWrite_.allocation, 0, VK_WHOLE_SIZE);
}

inline void Renderer::recordCommandBuffer(VkCommandBuffer cmd, VkImage swapImage,
                                          VkImageView swapView) {
    VkDescriptorBufferBindingInfoEXT bindingInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
    bindingInfo.address = descriptorBufferBindingAddress_;
    bindingInfo.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT;
    vkCmdBindDescriptorBuffersEXT(cmd, 1, &bindingInfo);

    // 主机写入 Globals 对后续 shader 可见。
    barrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    PushConstants pc{};
    pc.slot = frameSlot_;
    pc.width = extent_.width;
    pc.height = extent_.height;
    pc.frameIndex = frameCounter_;
    pc.queueCur = 0;
    pc.queueNext = 1;
    pc.count = pixelCount_;

    // 1) 主射线路径生成。
    dispatchCompute(cmd, passPrimary_, pc, pixelCount_);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT);
    {
        uint32_t width = std::max(1u, std::min(pixelCount_, maxGroupsX_));
        uint32_t height = std::max(1u, std::min((pixelCount_ + width - 1) / width, maxGroupsY_));
        uint32_t depth = std::max(1u, (pixelCount_ + width * height - 1) / (width * height));
        traceRaysDirect(cmd, pc, width, height, depth, 0);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // 2) 主表面信息（ReSTIR 的 target/guide）。
    dispatchCompute(cmd, passPrimaryInfo_, pc, pixelCount_);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    // 3) wavefront 路径循环。bounce 0..8：最后一次只处理 emissive/miss。
    uint32_t queueCur = 0;
    for (uint32_t bounce = 0; bounce <= kMaxScatterEvents; ++bounce) {
        pc.queueCur = queueCur;
        pc.queueNext = 1u - queueCur;
        pc.count = 0;
        if (bounce == 0) {
            dispatchCompute(cmd, passShade_, pc, pixelCount_);
        } else {
            dispatchComputeIndirect(cmd, passShade_, pc, kIndirectShade);
        }
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                    VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        if (bounce == kMaxScatterEvents) {
            break;
        }
        // GPU 依据活跃计数生成 trace/shade/shadow 间接命令。
        pc.flags = 0;
        dispatchCompute(cmd, passBuildIndirect_, pc, 1);
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
        // NEE shadow rays。
        traceRaysIndirect(cmd, pc, indirectBuffer_.address + kIndirectShadowTrace, 1);
        barrier(cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        dispatchComputeIndirect(cmd, passShadowResolve_, pc, kIndirectShadowResolve);
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_READ_BIT);
        // 下一反弹的路径 trace。
        pc.queueCur = 1u - queueCur;
        traceRaysIndirect(cmd, pc, indirectBuffer_.address + kIndirectPathTrace, 0);
        barrier(cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        queueCur = 1u - queueCur;
    }

    // 4) ReSTIR DI：initial -> temporal -> spatial -> visibility。
    pc.queueCur = 0;
    pc.queueNext = 1;
    pc.count = pixelCount_;
    dispatchCompute(cmd, passRestirInit_, pc, pixelCount_);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    dispatchCompute(cmd, passRestirTemporal_, pc, pixelCount_);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    dispatchCompute(cmd, passRestirSpatial_, pc, pixelCount_);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    // ReSTIR shadow 任务队列归零。
    vkCmdFillBuffer(cmd, countsBuffer_.buffer, offsetof(GpuCounts, shadowCount), sizeof(uint32_t), 0);
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    dispatchCompute(cmd, passRestirShadow_, pc, pixelCount_);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    pc.flags = 1;
    dispatchCompute(cmd, passBuildIndirect_, pc, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
    traceRaysIndirect(cmd, pc, indirectBuffer_.address + kIndirectShadowTrace, 1);
    barrier(cmd, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    dispatchComputeIndirect(cmd, passShadowResolve_, pc, kIndirectShadowResolve);
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    // 5) SVGF：时间重投影（含初始空间方差）-> 5 轮 à-trous。
    pc.count = pixelCount_ * kSlotsPerPixel;
    dispatchCompute(cmd, passSvgfTemporal_, pc, static_cast<uint32_t>(pc.count));
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    const uint32_t steps[kAtrousIterations] = {1, 2, 4, 8, 16};
    for (uint32_t iteration = 0; iteration < kAtrousIterations; ++iteration) {
        pc.step = steps[iteration];
        pc.flags = iteration;
        dispatchCompute(cmd, passSvgfAtrous_, pc, static_cast<uint32_t>(pc.count));
        barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    }

    // 6) 分域滤波结果可见于片元着色器。
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

    // 7) 全屏三角形合成/色调映射；最终附件写入 -> present。
    drawFullscreen(cmd, swapImage, swapView, pc);
}

inline bool Renderer::acquireImage(uint32_t& imageIndex, VkResult& result) {
    result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                   frames_[frameSlot_].imageAvailable, VK_NULL_HANDLE, &imageIndex);
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

inline void Renderer::drawFrame() {
    const int aspectWidth = static_cast<int>(extent_.width);
    const int aspectHeight = static_cast<int>(extent_.height);
    const float aspect =
        aspectHeight > 0 ? static_cast<float>(aspectWidth) / static_cast<float>(aspectHeight) : 1.0f;
    const Vec3 forward{std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_),
                       -std::cos(yaw_) * std::cos(pitch_)};
    camera_ = makeCamera(cameraPos_, cameraPos_ + forward, 60.0f, aspect);

    Frame& frame = frames_[frameSlot_];
    if (vkWaitForFences(device_, 1, &frame.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        fatalError_ = true;
        return;
    }
    updateGlobals();

    uint32_t imageIndex = 0;
    VkResult acquireResult = VK_SUCCESS;
    if (!acquireImage(imageIndex, acquireResult)) {
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_ERROR_SURFACE_LOST_KHR) {
            swapchainSuboptimal_ = true;
            return;
        }
        std::fprintf(stderr, "vkAcquireNextImageKHR 失败: %d\n", static_cast<int>(acquireResult));
        fatalError_ = true;
        return;
    }
    if (acquireResult == VK_SUBOPTIMAL_KHR) {
        swapchainSuboptimal_ = true;
    }
    if (vkResetFences(device_, 1, &frame.fence) != VK_SUCCESS) {
        fatalError_ = true;
        return;
    }
    if (vkResetCommandBuffer(frame.cmd, 0) != VK_SUCCESS) {
        fatalError_ = true;
        return;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(frame.cmd, &begin) != VK_SUCCESS) {
        fatalError_ = true;
        return;
    }
    recordCommandBuffer(frame.cmd, swapImages_[static_cast<size_t>(imageIndex)],
                        swapViews_[static_cast<size_t>(imageIndex)]);
    if (vkEndCommandBuffer(frame.cmd) != VK_SUCCESS) {
        fatalError_ = true;
        return;
    }
    submitAndPresent(imageIndex);
}

inline void Renderer::submitAndPresent(uint32_t imageIndex) {
    Frame& frame = frames_[frameSlot_];
    VkSemaphoreSubmitInfo waits[2]{};
    uint32_t waitCount = 0;
    waits[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[waitCount].semaphore = frame.imageAvailable;
    waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    ++waitCount;
    if (timelineValue_ > 0) {
        waits[waitCount].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[waitCount].semaphore = timeline_;
        waits[waitCount].value = timelineValue_;
        waits[waitCount].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++waitCount;
    }
    VkSemaphoreSubmitInfo signals[2]{};
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = renderFinished_[static_cast<size_t>(imageIndex)];
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = timeline_;
    signals[1].value = timelineValue_ + 1;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = frame.cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = waitCount;
    submit.pWaitSemaphoreInfos = waits;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos = signals;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cb;
    if (vkQueueSubmit2(queue_, 1, &submit, frame.fence) != VK_SUCCESS) {
        std::fprintf(stderr, "vkQueueSubmit2 失败\n");
        fatalError_ = true;
        return;
    }
    ++timelineValue_;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[static_cast<size_t>(imageIndex)];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const VkResult presentResult = vkQueuePresentKHR(queue_, &present);

    const bool presented = presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR;
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_ERROR_SURFACE_LOST_KHR) {
        swapchainSuboptimal_ = true;
    } else if (presentResult == VK_SUBOPTIMAL_KHR) {
        swapchainSuboptimal_ = true;
    } else if (!presented) {
        std::fprintf(stderr, "vkQueuePresentKHR 失败: %d\n", static_cast<int>(presentResult));
        fatalError_ = true;
    }

    prevCamera_ = camera_;
    prevCameraValid_ = true;
    ++frameCounter_;
    frameSlot_ = frameCounter_ & 1u;
    if (presented) {
        historyReset_ = false;
        beginStatsFrame();
    } else {
        historyReset_ = true;
    }
}

inline void Renderer::updateTitle(bool placeholder) {
    if (placeholder || forceTitlePlaceholder_) {
        glfwSetWindowTitle(window_, "MiniVoxelPathTracer | FPS: -- | -- ms/frame");
        forceTitlePlaceholder_ = true;
    }
}

inline void Renderer::beginStatsFrame() {
    const double now = glfwGetTime();
    if (statsFrames_ == 0) {
        statsStartTime_ = now;
    }
    ++statsFrames_;
    const double elapsed = now - statsStartTime_;
    if (elapsed >= 0.5 && statsFrames_ > 0) {
        const double fps = static_cast<double>(statsFrames_) / elapsed;
        const double ms = 1000.0 * elapsed / static_cast<double>(statsFrames_);
        char title[256];
        std::snprintf(title, sizeof(title),
                      "MiniVoxelPathTracer | FPS: %.1f | %.2f ms/frame", fps, ms);
        glfwSetWindowTitle(window_, title);
        statsFrames_ = 0;
        statsStartTime_ = 0.0;
        forceTitlePlaceholder_ = false;
    }
}

inline void Renderer::resetStats() {
    statsFrames_ = 0;
    statsStartTime_ = 0.0;
    forceTitlePlaceholder_ = true;
}

inline void Renderer::handleResize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    vkDeviceWaitIdle(device_);
    destroyRenderResources();
    destroySwapchain();
    if (!createSwapchain()) {
        std::fprintf(stderr, "重建交换链失败\n");
        fatalError_ = true;
        return;
    }
    if (!createRenderResources()) {
        std::fprintf(stderr, "重建帧尺寸资源失败\n");
        fatalError_ = true;
        return;
    }
    swapchainSuboptimal_ = false;
    historyReset_ = true;
    resetStats();
    updateTitle(true);
    lastInputTime_ = glfwGetTime();
    resetInputTime_ = true;
}

inline void Renderer::releaseMouse() {
    if (mouseCaptured_) {
        mouseCaptured_ = false;
        firstMouse_ = true;
        if (window_ != nullptr) {
            glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
                glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
            }
        }
    }
}

inline void Renderer::captureMouse() {
    if (window_ == nullptr) {
        return;
    }
    mouseCaptured_ = true;
    firstMouse_ = true;
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
    glfwGetCursorPos(window_, &lastMouseX_, &lastMouseY_);
}

inline void Renderer::resetCamera() {
    cameraPos_ = {8.0f, 2.5f, 20.0f};
    const Vec3 target{8.0f, 2.0f, 6.0f};
    const Vec3 dir = normalize(target - cameraPos_);
    pitch_ = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
    yaw_ = std::atan2(dir.x, -dir.z);
    historyReset_ = true;
}

inline void Renderer::onKey(int key, int action) {
    if (action != GLFW_PRESS) {
        return;
    }
    if (key == GLFW_KEY_ESCAPE) {
        if (mouseCaptured_) {
            releaseMouse();
        } else {
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }
    } else if (key == GLFW_KEY_R) {
        resetCamera();
    }
}

inline void Renderer::updateCamera(double dt) {
    if (!windowFocused_ || dt <= 0.0) {
        return;
    }
    Vec3 horizontal{0.0f};
    const Vec3 forwardH{std::sin(yaw_), 0.0f, -std::cos(yaw_)};
    const Vec3 rightH{std::cos(yaw_), 0.0f, std::sin(yaw_)};
    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) {
        horizontal += forwardH;
    }
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) {
        horizontal -= forwardH;
    }
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) {
        horizontal += rightH;
    }
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) {
        horizontal -= rightH;
    }
    if (length(horizontal) > 0.0f) {
        horizontal = normalize(horizontal);
    }
    const bool shift = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                       glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    const float speed = 3.0f * (shift ? 3.0f : 1.0f);
    float vertical = 0.0f;
    if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS) {
        vertical += 1.0f;
    }
    if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) {
        vertical -= 1.0f;
    }
    const float step = speed * static_cast<float>(dt);
    cameraPos_ += horizontal * step;
    cameraPos_.y += vertical * step;
}

inline void Renderer::runLoop() {
    while (glfwWindowShouldClose(window_) == GLFW_FALSE && !fatalError_) {
        glfwPollEvents();
        if (g_validationFailure.load()) {
            break;
        }
        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(window_, &fbWidth, &fbHeight);
        if (fbWidth <= 0 || fbHeight <= 0) {
            if (!paused_) {
                paused_ = true;
                historyReset_ = true;
                resetStats();
                updateTitle(true);
            }
            glfwWaitEventsTimeout(0.02);
            lastInputTime_ = glfwGetTime();
            resetInputTime_ = true;
            continue;
        }
        if (paused_) {
            paused_ = false;
            historyReset_ = true;
            resetStats();
            updateTitle(true);
            lastInputTime_ = glfwGetTime();
            resetInputTime_ = true;
        }
        if (fbWidth != static_cast<int>(extent_.width) ||
            fbHeight != static_cast<int>(extent_.height) || swapchainSuboptimal_) {
            handleResize(fbWidth, fbHeight);
            if (fatalError_) {
                break;
            }
        }
        const double now = glfwGetTime();
        double dt = 0.0;
        if (!resetInputTime_) {
            dt = std::clamp(now - lastInputTime_, 0.0, 0.1);
        } else {
            resetInputTime_ = false;
        }
        lastInputTime_ = now;
        updateCamera(dt);
        drawFrame();
        if (g_validationFailure.load()) {
            break;
        }
    }
}

inline void Renderer::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }
    for (Frame& frame : frames_) {
        destroyFrameObjects(frame);
    }
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_, nullptr);
        timeline_ = VK_NULL_HANDLE;
    }
    if (tempPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, tempPool_, nullptr);
        tempPool_ = VK_NULL_HANDLE;
    }
    if (tempFence_ != VK_NULL_HANDLE) {
        vkDestroyFence(device_, tempFence_, nullptr);
        tempFence_ = VK_NULL_HANDLE;
    }
    destroyRenderResources();
    destroyBuffer(descriptorBuffer_);
    descriptorBufferSize_ = 0;
    descriptorsCreated_ = false;
    destroySwapchain();
    destroyStaticResources();
    if (allocator_ != VK_NULL_HANDLE) {
        vmaDestroyAllocator(allocator_);
        allocator_ = VK_NULL_HANDLE;
    }
#ifndef NDEBUG
    if (debugMessenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
        debugMessenger_ = VK_NULL_HANDLE;
    }
#endif
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
    initialized_ = false;
}

inline void Renderer::framebufferSizeCallback(GLFWwindow* window, int width, int height) {
    (void)window;
    (void)width;
    (void)height;
}

inline void Renderer::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    auto* self = static_cast<Renderer*>(glfwGetWindowUserPointer(window));
    if (self != nullptr) {
        self->onKey(key, action);
    }
}

inline void Renderer::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    auto* self = static_cast<Renderer*>(glfwGetWindowUserPointer(window));
    if (self == nullptr || button != GLFW_MOUSE_BUTTON_RIGHT) {
        return;
    }
    if (action == GLFW_PRESS) {
        self->captureMouse();
    } else if (action == GLFW_RELEASE) {
        self->releaseMouse();
    }
}

inline void Renderer::cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
    auto* self = static_cast<Renderer*>(glfwGetWindowUserPointer(window));
    if (self == nullptr || !self->mouseCaptured_ || !self->windowFocused_) {
        return;
    }
    if (self->firstMouse_) {
        self->lastMouseX_ = xpos;
        self->lastMouseY_ = ypos;
        self->firstMouse_ = false;
        return;
    }
    const double dx = xpos - self->lastMouseX_;
    const double dy = ypos - self->lastMouseY_;
    self->lastMouseX_ = xpos;
    self->lastMouseY_ = ypos;
    constexpr double sensitivity = 0.0025;
    self->yaw_ += static_cast<float>(dx * sensitivity);
    self->pitch_ -= static_cast<float>(dy * sensitivity);
    constexpr float pitchLimit = 1.553343034f; // 89 度
    self->pitch_ = std::clamp(self->pitch_, -pitchLimit, pitchLimit);
    if (self->yaw_ > 6.28318530718) {
        self->yaw_ -= 6.28318530718f;
    } else if (self->yaw_ < -6.28318530718) {
        self->yaw_ += 6.28318530718f;
    }
}

inline void Renderer::focusCallback(GLFWwindow* window, int focused) {
    auto* self = static_cast<Renderer*>(glfwGetWindowUserPointer(window));
    if (self == nullptr) {
        return;
    }
    self->windowFocused_ = focused == GLFW_TRUE;
    if (!self->windowFocused_) {
        self->releaseMouse();
        self->resetInputTime_ = true;
    } else {
        self->resetInputTime_ = true;
    }
}

} // namespace rae
