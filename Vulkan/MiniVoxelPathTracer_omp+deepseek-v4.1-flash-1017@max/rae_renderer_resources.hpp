// 静态场景 GPU 资源、AS、shader/pipeline、描述符缓冲与帧尺寸资源。
#pragma once

#include "rae_renderer_setup.hpp"

#include <cassert>

namespace rae {

inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return alignment > 1 ? ((value + alignment - 1) / alignment) * alignment : value;
}

inline std::vector<VkDescriptorSetLayoutBinding> sceneSetBindings(VkShaderStageFlags stage) {
    std::vector<VkDescriptorSetLayoutBinding> b;
    auto add = [&](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
        VkDescriptorSetLayoutBinding v{};
        v.binding = binding;
        v.descriptorType = type;
        v.descriptorCount = 1;
        v.stageFlags = stages;
        b.push_back(v);
    };
    add(bindings::kGlobals, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage);
    add(bindings::kMaterials, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage);
    add(bindings::kTriangles, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage);
    add(bindings::kInstanceGroups, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage);
    add(bindings::kLights, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage);
    add(bindings::kBaseTex, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
    add(bindings::kRoughTex, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage);
    return b;
}

inline std::vector<VkDescriptorSetLayoutBinding> globalsBindings(VkShaderStageFlags stage) {
    std::vector<VkDescriptorSetLayoutBinding> b;
    VkDescriptorSetLayoutBinding v{};
    v.binding = bindings::kGlobals;
    v.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    v.descriptorCount = 1;
    v.stageFlags = stage;
    b.push_back(v);
    return b;
}

inline void appendBinding(std::vector<VkDescriptorSetLayoutBinding>& b, uint32_t binding,
                          VkDescriptorType type, VkShaderStageFlags stage) {
    VkDescriptorSetLayoutBinding v{};
    v.binding = binding;
    v.descriptorType = type;
    v.descriptorCount = 1;
    v.stageFlags = stage;
    b.push_back(v);
}

inline bool Renderer::createStaticResources() {
    // 采样器。
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.minLod = 0.0f;
    sampler.maxLod = static_cast<float>(scene_.mipLevels > 0 ? scene_.mipLevels - 1 : 0);
    sampler.maxAnisotropy = 1.0f;
    sampler.anisotropyEnable = VK_FALSE;
    sampler.compareEnable = VK_FALSE;
    if (vkCreateSampler(device_, &sampler, nullptr, &baseSampler_) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 base sampler 失败\n");
        return false;
    }
    if (!uploadTextures()) {
        return false;
    }

    const VkDeviceSize materialBytes =
        static_cast<VkDeviceSize>(scene_.gpuMaterials.size() * sizeof(GpuMaterial));
    materialBuffer_ = createBuffer(materialBytes,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   false);
    if (materialBuffer_.buffer == VK_NULL_HANDLE) {
        return false;
    }
    uploadBuffer(materialBuffer_, scene_.gpuMaterials.data(), materialBytes);

    const VkDeviceSize triangleBytes =
        static_cast<VkDeviceSize>(scene_.triangles.size() * sizeof(GpuTriangle));
    triangleBuffer_ = createBuffer(triangleBytes,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   false);
    if (triangleBuffer_.buffer == VK_NULL_HANDLE) {
        return false;
    }
    uploadBuffer(triangleBuffer_, scene_.triangles.data(), triangleBytes);

    std::vector<GpuInstanceGroup> groups(scene_.groups.size());
    for (size_t gi = 0; gi < scene_.groups.size(); ++gi) {
        groups[gi].data[0] = scene_.groupTriOffset[gi];
        groups[gi].data[1] = scene_.groups[gi].material;
        groups[gi].data[2] = scene_.groups[gi].glassBody ? 1u : 0u;
        groups[gi].data[3] = 0;
    }
    const VkDeviceSize groupBytes = static_cast<VkDeviceSize>(groups.size() * sizeof(GpuInstanceGroup));
    instanceGroupBuffer_ = createBuffer(groupBytes,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        false);
    if (instanceGroupBuffer_.buffer == VK_NULL_HANDLE) {
        return false;
    }
    uploadBuffer(instanceGroupBuffer_, groups.data(), groupBytes);

    std::vector<GpuLightEntry> lights = scene_.lights;
    if (lights.empty()) {
        lights.resize(1);
    }
    const VkDeviceSize lightBytes = static_cast<VkDeviceSize>(lights.size() * sizeof(GpuLightEntry));
    lightBuffer_ = createBuffer(lightBytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false);
    if (lightBuffer_.buffer == VK_NULL_HANDLE) {
        return false;
    }
    uploadBuffer(lightBuffer_, lights.data(), lightBytes);

    countsBuffer_ = createBuffer(sizeof(GpuCounts),
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 false);
    if (countsBuffer_.buffer == VK_NULL_HANDLE) {
        return false;
    }

    groupVertexBuffers_.resize(scene_.groups.size());
    groupIndexBuffers_.resize(scene_.groups.size());
    groupVertexCounts_.resize(scene_.groups.size());
    groupIndexCounts_.resize(scene_.groups.size());
    for (size_t gi = 0; gi < scene_.groups.size(); ++gi) {
        const CpuGroup& g = scene_.groups[gi];
        const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(g.vertices.size() * sizeof(Vec3));
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(g.indices.size() * sizeof(uint32_t));
        groupVertexCounts_[gi] = static_cast<uint32_t>(g.vertices.size());
        groupIndexCounts_[gi] = static_cast<uint32_t>(g.indices.size());
        Buffer vb = createBuffer(vertexBytes,
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 false);
        Buffer ib = createBuffer(indexBytes,
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 false);
        if (vb.buffer == VK_NULL_HANDLE || ib.buffer == VK_NULL_HANDLE) {
            return false;
        }
        uploadBuffer(vb, g.vertices.data(), vertexBytes);
        uploadBuffer(ib, g.indices.data(), indexBytes);
        groupVertexBuffers_[gi] = vb;
        groupIndexBuffers_[gi] = ib;
    }
    return !fatalError_;
}

inline void Renderer::destroyStaticResources() {
    for (Buffer& b : groupVertexBuffers_) {
        destroyBuffer(b);
    }
    groupVertexBuffers_.clear();
    for (Buffer& b : groupIndexBuffers_) {
        destroyBuffer(b);
    }
    groupIndexBuffers_.clear();
    destroyBuffer(triangleBuffer_);
    destroyBuffer(instanceGroupBuffer_);
    destroyBuffer(materialBuffer_);
    destroyBuffer(lightBuffer_);
    destroyBuffer(countsBuffer_);
    destroyBuffer(tlasInstanceBuffer_);
    destroyBuffer(sbtBuffer_);
    destroyBuffer(descriptorBuffer_);
    descriptorBufferSize_ = 0;
    descriptorBufferBaseOffset_ = 0;
    descriptorBufferBindingAddress_ = 0;
    descriptorsCreated_ = false;
    for (Accel& a : blasAccels_) {
        destroyAccel(a);
    }
    blasAccels_.clear();
    destroyAccel(tlas_);
    destroyImage(baseTex_);
    destroyImage(roughTex_);
    if (baseSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, baseSampler_, nullptr);
        baseSampler_ = VK_NULL_HANDLE;
    }

    if (rtPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, rtPipeline_, nullptr);
        rtPipeline_ = VK_NULL_HANDLE;
    }
    auto destroyPass = [&](Pass& p) {
        if (p.shader != VK_NULL_HANDLE) {
            vkDestroyShaderEXT(device_, p.shader, nullptr);
            p.shader = VK_NULL_HANDLE;
        }
        if (p.layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, p.layout, nullptr);
            p.layout = VK_NULL_HANDLE;
        }
    };
    destroyPass(passPrimary_);
    destroyPass(passPrimaryInfo_);
    destroyPass(passShade_);
    destroyPass(passShadowResolve_);
    destroyPass(passBuildIndirect_);
    destroyPass(passRestirInit_);
    destroyPass(passRestirTemporal_);
    destroyPass(passRestirSpatial_);
    destroyPass(passRestirShadow_);
    destroyPass(passSvgfTemporal_);
    destroyPass(passSvgfAtrous_);
    if (displayVert_ != VK_NULL_HANDLE) {
        vkDestroyShaderEXT(device_, displayVert_, nullptr);
        displayVert_ = VK_NULL_HANDLE;
    }
    if (displayFrag_ != VK_NULL_HANDLE) {
        vkDestroyShaderEXT(device_, displayFrag_, nullptr);
        displayFrag_ = VK_NULL_HANDLE;
    }
    if (displayLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, displayLayout_, nullptr);
        displayLayout_ = VK_NULL_HANDLE;
    }
    if (rtLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, rtLayout_, nullptr);
        rtLayout_ = VK_NULL_HANDLE;
    }
    for (auto& entry : layoutRefs_) {
        if (entry.first != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, entry.first, nullptr);
        }
    }
    layoutRefs_.clear();
    rtSetLayout_ = VK_NULL_HANDLE;
    displaySetLayout_ = VK_NULL_HANDLE;
}

inline bool Renderer::uploadTextures() {
    const uint32_t layers = static_cast<uint32_t>(scene_.materials.size());
    const uint32_t mips = scene_.mipLevels;
    const uint32_t size = scene_.textureSize;
    baseTex_ = createImage2D(VK_FORMAT_R8G8B8A8_SRGB, {size, size}, mips, layers,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    roughTex_ = createImage2D(VK_FORMAT_R8_UNORM, {size, size}, mips, layers,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    if (baseTex_.image == VK_NULL_HANDLE || roughTex_.image == VK_NULL_HANDLE) {
        return false;
    }

    std::vector<VkDeviceSize> baseOffsets(static_cast<size_t>(layers) * mips);
    std::vector<VkDeviceSize> roughOffsets(static_cast<size_t>(layers) * mips);
    std::vector<uint8_t> baseData;
    std::vector<uint8_t> roughData;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        const TextureLayer& tex = scene_.textures[layer];
        for (uint32_t mip = 0; mip < mips; ++mip) {
            const size_t index = static_cast<size_t>(layer) * mips + mip;
            baseOffsets[index] = static_cast<VkDeviceSize>(baseData.size());
            baseData.insert(baseData.end(), tex.base[mip].begin(), tex.base[mip].end());
            roughOffsets[index] = static_cast<VkDeviceSize>(roughData.size());
            roughData.insert(roughData.end(), tex.rough[mip].begin(), tex.rough[mip].end());
            // 保证下一个 mip 的 bufferOffset 至少 4 字节对齐。
            while ((roughData.size() & 3u) != 0u) {
                roughData.push_back(0);
            }
        }
    }
    Buffer baseStaging = createBuffer(baseData.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, 0, false);
    Buffer roughStaging = createBuffer(roughData.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, 0, false);
    if (baseStaging.buffer == VK_NULL_HANDLE || roughStaging.buffer == VK_NULL_HANDLE ||
        baseStaging.mapped == nullptr || roughStaging.mapped == nullptr) {
        destroyBuffer(baseStaging);
        destroyBuffer(roughStaging);
        return false;
    }
    std::memcpy(baseStaging.mapped, baseData.data(), baseData.size());
    std::memcpy(roughStaging.mapped, roughData.data(), roughData.size());
    vmaFlushAllocation(allocator_, baseStaging.allocation, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(allocator_, roughStaging.allocation, 0, VK_WHOLE_SIZE);

    VkCommandBuffer cmd = beginTempCommand();
    transitionImage(cmd, baseTex_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, mips,
                    layers);
    transitionImage(cmd, roughTex_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, mips,
                    layers);
    std::vector<VkBufferImageCopy> baseCopies;
    std::vector<VkBufferImageCopy> roughCopies;
    uint32_t mipSize = size;
    for (uint32_t mip = 0; mip < mips; ++mip) {
        for (uint32_t layer = 0; layer < layers; ++layer) {
            const size_t index = static_cast<size_t>(layer) * mips + mip;
            VkBufferImageCopy copy{};
            copy.bufferOffset = baseOffsets[index];
            copy.bufferRowLength = 0;
            copy.bufferImageHeight = 0;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
            copy.imageOffset = {0, 0, 0};
            copy.imageExtent = {mipSize, mipSize, 1};
            baseCopies.push_back(copy);
            copy.bufferOffset = roughOffsets[index];
            roughCopies.push_back(copy);
        }
        mipSize = std::max(1u, mipSize / 2);
    }
    vkCmdCopyBufferToImage(cmd, baseStaging.buffer, baseTex_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(baseCopies.size()), baseCopies.data());
    vkCmdCopyBufferToImage(cmd, roughStaging.buffer, roughTex_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(roughCopies.size()), roughCopies.data());
    barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    transitionImage(cmd, baseTex_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, mips, layers);
    transitionImage(cmd, roughTex_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, mips, layers);
    endTempCommand(cmd, true);
    destroyBuffer(baseStaging);
    destroyBuffer(roughStaging);
    return !fatalError_;
}

inline bool Renderer::buildAccelerationStructures() {
    if (scene_.groups.empty()) {
        std::fprintf(stderr, "场景没有可构建的几何分组\n");
        return false;
    }
    const uint32_t groupCount = static_cast<uint32_t>(scene_.groups.size());
    std::vector<VkAccelerationStructureKHR> oldHandles(groupCount, VK_NULL_HANDLE);
    std::vector<Buffer> oldBuffers(groupCount);
    std::vector<Buffer> scratchBuffers(groupCount);
    VkCommandBuffer cmd = beginTempCommand();
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos;
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    buildInfos.reserve(groupCount);
    geometries.reserve(groupCount);
    ranges.reserve(groupCount);
    bool ok = true;
    for (uint32_t gi = 0; gi < groupCount; ++gi) {
        const uint32_t vertexCount = groupVertexCounts_[gi];
        const uint32_t indexCount = groupIndexCounts_[gi];
        if (vertexCount == 0 || indexCount < 3) {
            ok = false;
            break;
        }
        VkAccelerationStructureGeometryTrianglesDataKHR triangles{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = groupVertexBuffers_[gi].address;
        triangles.vertexStride = sizeof(Vec3);
        triangles.maxVertex = vertexCount - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = groupIndexBuffers_[gi].address;
        triangles.transformData = {};
        VkAccelerationStructureGeometryKHR geometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = 0;
        geometry.geometry.triangles = triangles;
        geometries.push_back(geometry);

        VkAccelerationStructureBuildGeometryInfoKHR build{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometries.back();
        buildInfos.push_back(build);

        const uint32_t primitiveCount = indexCount / 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device_,
                                                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &buildInfos.back(), &primitiveCount, &sizes);
        Buffer asBuffer = createBuffer(sizes.accelerationStructureSize,
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                       false);
        const VkDeviceSize scratchAlignment =
            asProps_.minAccelerationStructureScratchOffsetAlignment;
        Buffer scratch = createBuffer(sizes.buildScratchSize + scratchAlignment,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      false);
        if (asBuffer.buffer == VK_NULL_HANDLE || scratch.buffer == VK_NULL_HANDLE) {
            ok = false;
            break;
        }
        const VkDeviceAddress scratchAddress = alignUp(scratch.address, scratchAlignment);
        VkAccelerationStructureCreateInfoKHR asCreate{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        asCreate.buffer = asBuffer.buffer;
        asCreate.offset = 0;
        asCreate.size = sizes.accelerationStructureSize;
        asCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        if (vkCreateAccelerationStructureKHR(device_, &asCreate, nullptr, &handle) != VK_SUCCESS) {
            ok = false;
            break;
        }
        oldHandles[gi] = handle;
        oldBuffers[gi] = asBuffer;
        scratchBuffers[gi] = scratch;
        buildInfos.back().dstAccelerationStructure = handle;
        buildInfos.back().scratchData.deviceAddress = scratchAddress;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = primitiveCount;
        range.primitiveOffset = 0;
        range.firstVertex = 0;
        range.transformOffset = 0;
        ranges.push_back(range);
        const VkAccelerationStructureBuildRangeInfoKHR* rangePtr = &ranges.back();
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfos.back(), &rangePtr);
    }
    if (!ok) {
        endTempCommand(cmd, true);
        for (uint32_t i = 0; i < groupCount; ++i) {
            if (oldHandles[i] != VK_NULL_HANDLE) {
                vkDestroyAccelerationStructureKHR(device_, oldHandles[i], nullptr);
            }
            destroyBuffer(oldBuffers[i]);
            destroyBuffer(scratchBuffers[i]);
        }
        return false;
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    queryInfo.queryCount = groupCount;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(device_, &queryInfo, nullptr, &queryPool) != VK_SUCCESS) {
        endTempCommand(cmd, true);
        for (uint32_t i = 0; i < groupCount; ++i) {
            if (oldHandles[i] != VK_NULL_HANDLE) {
                vkDestroyAccelerationStructureKHR(device_, oldHandles[i], nullptr);
            }
            destroyBuffer(oldBuffers[i]);
            destroyBuffer(scratchBuffers[i]);
        }
        return false;
    }
    vkCmdResetQueryPool(cmd, queryPool, 0, groupCount);
    vkCmdWriteAccelerationStructuresPropertiesKHR(
        cmd, groupCount, oldHandles.data(), VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
        queryPool, 0);
    endTempCommand(cmd, true);
    if (fatalError_) {
        vkDestroyQueryPool(device_, queryPool, nullptr);
        return false;
    }
    std::vector<uint64_t> compactSizes(groupCount, 0);
    const VkResult queryResult = vkGetQueryPoolResults(
        device_, queryPool, 0, groupCount, compactSizes.size() * sizeof(uint64_t),
        compactSizes.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    vkDestroyQueryPool(device_, queryPool, nullptr);
    if (queryResult != VK_SUCCESS) {
        std::fprintf(stderr, "读取 AS compacted size 失败\n");
        return false;
    }

    blasAccels_.resize(groupCount);
    for (uint32_t gi = 0; gi < groupCount; ++gi) {
        VkDeviceSize compactSize = static_cast<VkDeviceSize>(compactSizes[gi]);
        if (compactSize == 0) {
            compactSize = oldBuffers[gi].size;
        }
        Buffer compactBuffer = createBuffer(compactSize,
                                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                            false);
        if (compactBuffer.buffer == VK_NULL_HANDLE) {
            return false;
        }
        VkAccelerationStructureCreateInfoKHR asCreate{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        asCreate.buffer = compactBuffer.buffer;
        asCreate.offset = 0;
        asCreate.size = compactSize;
        asCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        if (vkCreateAccelerationStructureKHR(device_, &asCreate, nullptr, &handle) != VK_SUCCESS) {
            destroyBuffer(compactBuffer);
            return false;
        }
        blasAccels_[gi].handle = handle;
        blasAccels_[gi].buffer = compactBuffer;
    }

    std::vector<VkAccelerationStructureInstanceKHR> instances(groupCount);
    for (uint32_t gi = 0; gi < groupCount; ++gi) {
        VkAccelerationStructureInstanceKHR& inst = instances[gi];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                inst.transform.matrix[r][c] = (r == c) ? 1.0f : 0.0f;
            }
        }
        inst.instanceCustomIndex = gi;
        inst.mask = 0xFF;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        addressInfo.accelerationStructure = blasAccels_[gi].handle;
        inst.accelerationStructureReference =
            vkGetAccelerationStructureDeviceAddressKHR(device_, &addressInfo);
    }
    const VkDeviceSize instanceBytes =
        static_cast<VkDeviceSize>(instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    tlasInstanceBuffer_ = createBuffer(instanceBytes + 16ull,
                                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       true, 0, false);
    if (tlasInstanceBuffer_.buffer == VK_NULL_HANDLE || tlasInstanceBuffer_.mapped == nullptr) {
        return false;
    }
    // INSTANCES 几何的 deviceAddress 必须 16 字节对齐；VMA 分配地址不保证。
    const VkDeviceAddress tlasInstanceAddress = alignUp(tlasInstanceBuffer_.address, 16ull);
    const VkDeviceSize tlasInstanceOffset = tlasInstanceAddress - tlasInstanceBuffer_.address;
    std::memcpy(static_cast<uint8_t*>(tlasInstanceBuffer_.mapped) +
                    static_cast<size_t>(tlasInstanceOffset),
                instances.data(), static_cast<size_t>(instanceBytes));
    vmaFlushAllocation(allocator_, tlasInstanceBuffer_.allocation, 0, VK_WHOLE_SIZE);

    // 第二条命令：compact copy + TLAS build。
    VkCommandBuffer cmd2 = beginTempCommand();
    for (uint32_t gi = 0; gi < groupCount; ++gi) {
        VkCopyAccelerationStructureInfoKHR copyInfo{
            VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        copyInfo.src = oldHandles[gi];
        copyInfo.dst = blasAccels_[gi].handle;
        copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        vkCmdCopyAccelerationStructureKHR(cmd2, &copyInfo);
    }
    barrier(cmd2, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    VkAccelerationStructureGeometryInstancesDataKHR instancesData{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = alignUp(tlasInstanceBuffer_.address, 16ull);
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
    uint32_t maxInstances = groupCount;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasBuild, &maxInstances, &tlasSizes);
    tlas_.buffer = createBuffer(tlasSizes.accelerationStructureSize,
                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                false);
    const VkDeviceSize tlasScratchAlignment =
        asProps_.minAccelerationStructureScratchOffsetAlignment;
    Buffer tlasScratch = createBuffer(tlasSizes.buildScratchSize + tlasScratchAlignment,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      false);
    if (tlas_.buffer.buffer == VK_NULL_HANDLE || tlasScratch.buffer == VK_NULL_HANDLE) {
        endTempCommand(cmd2, true);
        return false;
    }
    const VkDeviceAddress tlasScratchAddress = alignUp(tlasScratch.address, tlasScratchAlignment);
    VkAccelerationStructureCreateInfoKHR tlasCreate{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    tlasCreate.buffer = tlas_.buffer.buffer;
    tlasCreate.offset = 0;
    tlasCreate.size = tlasSizes.accelerationStructureSize;
    tlasCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(device_, &tlasCreate, nullptr, &tlas_.handle) != VK_SUCCESS) {
        endTempCommand(cmd2, true);
        destroyBuffer(tlasScratch);
        return false;
    }
    tlasBuild.dstAccelerationStructure = tlas_.handle;
    tlasBuild.scratchData.deviceAddress = tlasScratchAddress;
    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = groupCount;
    const VkAccelerationStructureBuildRangeInfoKHR* tlasRangePtr = &tlasRange;
    vkCmdBuildAccelerationStructuresKHR(cmd2, 1, &tlasBuild, &tlasRangePtr);
    endTempCommand(cmd2, true);
    destroyBuffer(tlasScratch);

    // compact copy 与 TLAS build 已完成，旧 BLAS/scratch 可以释放。
    for (uint32_t gi = 0; gi < groupCount; ++gi) {
        if (oldHandles[gi] != VK_NULL_HANDLE) {
            vkDestroyAccelerationStructureKHR(device_, oldHandles[gi], nullptr);
        }
        destroyBuffer(oldBuffers[gi]);
        destroyBuffer(scratchBuffers[gi]);
    }
    return !fatalError_;
}

inline bool Renderer::compileShaders() {
    const char* files[] = {
        "common.glsl",      "globals.glsl",     "scene.glsl",
        "primary.comp",     "primary_info.comp", "shade.comp",
        "shadow_resolve.comp", "build_indirect.comp",
        "restir_init.comp", "restir_temporal.comp", "restir_spatial.comp",
        "restir_shadow.comp", "svgf_temporal.comp",
        "svgf_atrous.comp", "fullscreen.vert", "composite.frag",
        "trace.rgen",       "shadow.rgen",      "primary.rmiss",
        "shadow.rmiss",     "primary.rchit",    "shadow.rahit",
        "shadow.rchit"};
    for (const char* file : files) {
        if (!std::filesystem::exists(shadersDir_ / file)) {
            std::fprintf(stderr, "缺失 shader 文件: %s\n", (shadersDir_ / file).string().c_str());
            return false;
        }
    }
    return true;
}

inline VkDescriptorSetLayout Renderer::createSetLayout(
    const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 descriptor set layout 失败\n");
        return VK_NULL_HANDLE;
    }
    return layout;
}

inline void Renderer::createComputePass(const char* file,
                                        const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                                        Pass& pass) {
    pass.setLayout = createSetLayout(bindings);
    if (pass.setLayout == VK_NULL_HANDLE) {
        return;
    }
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &pass.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pass.layout) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 compute pipeline layout 失败: %s\n", file);
        return;
    }
    std::vector<uint32_t> spv = compileShader(file, shaderc_compute_shader);
    if (spv.empty()) {
        return;
    }
    VkShaderCreateInfoEXT info{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
    info.flags = 0;
    info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.nextStage = VkShaderStageFlagBits{};
    info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
    info.codeSize = spv.size() * sizeof(uint32_t);
    info.pCode = spv.data();
    info.pName = "main";
    info.setLayoutCount = 1;
    info.pSetLayouts = &pass.setLayout;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    if (vkCreateShadersEXT(device_, 1, &info, nullptr, &pass.shader) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 compute shader object 失败: %s\n", file);
        return;
    }
    layoutRefs_.push_back({pass.setLayout, &pass.setOffset});
}

inline void Renderer::createGraphicsPass() {
    const VkShaderStageFlags fs = VK_SHADER_STAGE_FRAGMENT_BIT;
    std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(fs);
    appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fs);
    appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fs);
    appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fs);
    appendBinding(b, bindings::kD, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fs);
    appendBinding(b, bindings::kE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fs);
    appendBinding(b, bindings::kF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fs);
    displaySetLayout_ = createSetLayout(b);
    if (displaySetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.offset = 0;
    range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &displaySetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &displayLayout_) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 display pipeline layout 失败\n");
        return;
    }
    std::vector<uint32_t> vert = compileShader("fullscreen.vert", shaderc_vertex_shader);
    std::vector<uint32_t> frag = compileShader("composite.frag", shaderc_fragment_shader);
    if (vert.empty() || frag.empty()) {
        return;
    }
    // 顶点/片元在同一 vkCreateShadersEXT 调用中显式链接，接口与 push constant 范围一致。
    VkShaderCreateInfoEXT infos[2]{};
    for (VkShaderCreateInfoEXT& info : infos) {
        info.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
        info.flags = VK_SHADER_CREATE_LINK_STAGE_BIT_EXT;
        info.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
        info.pName = "main";
        info.setLayoutCount = 1;
        info.pSetLayouts = &displaySetLayout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;
    }
    infos[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    infos[0].nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
    infos[0].codeSize = vert.size() * sizeof(uint32_t);
    infos[0].pCode = vert.data();
    infos[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    infos[1].nextStage = VkShaderStageFlagBits{};
    infos[1].codeSize = frag.size() * sizeof(uint32_t);
    infos[1].pCode = frag.data();
    VkShaderEXT shaders[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (vkCreateShadersEXT(device_, 2, infos, nullptr, shaders) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 linked display shader objects 失败\n");
        return;
    }
    displayVert_ = shaders[0];
    displayFrag_ = shaders[1];
    layoutRefs_.push_back({displaySetLayout_, &displaySetOffset_});
}

inline void Renderer::createRTPassLayout() {
    std::vector<VkDescriptorSetLayoutBinding> b;
    const VkShaderStageFlags rtStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                         VK_SHADER_STAGE_MISS_BIT_KHR |
                                         VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                         VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    appendBinding(b, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, rtStages);
    appendBinding(b, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, rtStages);
    appendBinding(b, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, rtStages);
    appendBinding(b, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, rtStages);
    appendBinding(b, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, rtStages);
    appendBinding(b, 5, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, rtStages);
    rtSetLayout_ = createSetLayout(b);
    if (rtSetLayout_ == VK_NULL_HANDLE) {
        return;
    }
    VkPushConstantRange range{};
    range.stageFlags = rtStages;
    range.offset = 0;
    range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &rtSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &rtLayout_) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 RT pipeline layout 失败\n");
        return;
    }
    layoutRefs_.push_back({rtSetLayout_, &rtSetOffset_});
}

inline bool Renderer::createPasses() {
    const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;

    {
        std::vector<VkDescriptorSetLayoutBinding> b = globalsBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("primary.comp", b, passPrimary_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("primary_info.comp", b, passPrimaryInfo_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kD, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kG, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kH, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("shade.comp", b, passShade_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = globalsBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kD, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("shadow_resolve.comp", b, passShadowResolve_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = globalsBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("build_indirect.comp", b, passBuildIndirect_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("restir_init.comp", b, passRestirInit_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kD, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("restir_temporal.comp", b, passRestirTemporal_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("restir_spatial.comp", b, passRestirSpatial_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kD, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("restir_shadow.comp", b, passRestirShadow_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kD, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("svgf_temporal.comp", b, passSvgfTemporal_);
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> b = sceneSetBindings(cs);
        appendBinding(b, bindings::kA, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kB, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kD, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        appendBinding(b, bindings::kE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, cs);
        createComputePass("svgf_atrous.comp", b, passSvgfAtrous_);
    }
    createGraphicsPass();
    createRTPassLayout();
    if (passPrimary_.shader == VK_NULL_HANDLE || passPrimaryInfo_.shader == VK_NULL_HANDLE ||
        passShade_.shader == VK_NULL_HANDLE || passShadowResolve_.shader == VK_NULL_HANDLE ||
        passBuildIndirect_.shader == VK_NULL_HANDLE || passRestirInit_.shader == VK_NULL_HANDLE ||
        passRestirTemporal_.shader == VK_NULL_HANDLE || passRestirSpatial_.shader == VK_NULL_HANDLE ||
        passRestirShadow_.shader == VK_NULL_HANDLE || passSvgfTemporal_.shader == VK_NULL_HANDLE ||
        passSvgfAtrous_.shader == VK_NULL_HANDLE || displayVert_ == VK_NULL_HANDLE ||
        displayFrag_ == VK_NULL_HANDLE || rtLayout_ == VK_NULL_HANDLE ||
        displayLayout_ == VK_NULL_HANDLE) {
        return false;
    }
    return true;
}

inline bool Renderer::createRayTracingPipeline() {
    const char* files[] = {"trace.rgen", "shadow.rgen", "primary.rmiss", "shadow.rmiss",
                           "primary.rchit", "shadow.rahit", "shadow.rchit"};
    const shaderc_shader_kind stages[] = {
        shaderc_raygen_shader,      shaderc_raygen_shader, shaderc_miss_shader,
        shaderc_miss_shader,        shaderc_closesthit_shader, shaderc_anyhit_shader,
        shaderc_closesthit_shader};
    constexpr size_t kRtShaderCount = 7;
    std::vector<VkShaderModule> modules(kRtShaderCount);
    std::vector<VkPipelineShaderStageCreateInfo> stageInfos;
    for (size_t i = 0; i < modules.size(); ++i) {
        std::vector<uint32_t> spv = compileShader(files[i], stages[i]);
        if (spv.empty()) {
            for (VkShaderModule m : modules) {
                if (m != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(device_, m, nullptr);
                }
            }
            return false;
        }
        modules[i] = createShaderModule(spv);
        if (modules[i] == VK_NULL_HANDLE) {
            for (VkShaderModule m : modules) {
                if (m != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(device_, m, nullptr);
                }
            }
            return false;
        }
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = (i == 0 || i == 1) ? VK_SHADER_STAGE_RAYGEN_BIT_KHR
                      : (i == 2 || i == 3) ? VK_SHADER_STAGE_MISS_BIT_KHR
                      : (i == 4) ? VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
                      : (i == 5) ? VK_SHADER_STAGE_ANY_HIT_BIT_KHR
                                 : VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        stage.module = modules[i];
        stage.pName = "main";
        stageInfos.push_back(stage);
    }
    VkRayTracingShaderGroupCreateInfoKHR groups[6]{};
    for (int i = 0; i < 6; ++i) {
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
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[3].generalShader = 3;
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[4].closestHitShader = 4;
    groups[5].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[5].closestHitShader = 6;
    groups[5].anyHitShader = 5;

    VkRayTracingPipelineCreateInfoKHR info{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    info.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    info.stageCount = static_cast<uint32_t>(stageInfos.size());
    info.pStages = stageInfos.data();
    info.groupCount = 6;
    info.pGroups = groups;
    info.maxPipelineRayRecursionDepth = 1;
    info.layout = rtLayout_;
    info.basePipelineHandle = VK_NULL_HANDLE;
    info.basePipelineIndex = -1;
    const VkResult result = vkCreateRayTracingPipelinesKHR(
        device_, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &rtPipeline_);
    for (VkShaderModule m : modules) {
        if (m != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, m, nullptr);
        }
    }
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "创建 RT pipeline 失败\n");
        return false;
    }

    const uint32_t handleSize = rtProps_.shaderGroupHandleSize;
    const uint32_t baseAlign = rtProps_.shaderGroupBaseAlignment;
    const uint32_t handleAlign = std::max(1u, rtProps_.shaderGroupHandleAlignment);
    const uint32_t raygenStride = static_cast<uint32_t>(alignUp(handleSize, baseAlign));
    const uint32_t hitStride = static_cast<uint32_t>(alignUp(handleSize, handleAlign));
    std::vector<uint8_t> handles(static_cast<size_t>(handleSize) * 6);
    if (vkGetRayTracingShaderGroupHandlesKHR(device_, rtPipeline_, 0, 6, handles.size(),
                                             handles.data()) != VK_SUCCESS) {
        std::fprintf(stderr, "获取 RT shader group handles 失败\n");
        return false;
    }
    const VkDeviceSize raygenOffset = 0;
    const VkDeviceSize missOffset = alignUp(raygenOffset + 2ull * raygenStride, baseAlign);
    const VkDeviceSize hitOffset = alignUp(missOffset + 2ull * hitStride, baseAlign);
    const VkDeviceSize sbtSize = alignUp(hitOffset + 2ull * hitStride, baseAlign);
    sbtBuffer_ = createBuffer(sbtSize + baseAlign,
                              VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              true, 0, false);
    if (sbtBuffer_.buffer == VK_NULL_HANDLE || sbtBuffer_.mapped == nullptr) {
        std::fprintf(stderr, "创建 SBT buffer 失败\n");
        return false;
    }
    const VkDeviceAddress sbtBaseAddress = alignUp(sbtBuffer_.address, baseAlign);
    const VkDeviceSize sbtBaseOffset = sbtBaseAddress - sbtBuffer_.address;
    std::memset(sbtBuffer_.mapped, 0, static_cast<size_t>(sbtBuffer_.size));
    auto* data = static_cast<uint8_t*>(sbtBuffer_.mapped) + sbtBaseOffset;
    std::memcpy(data + raygenOffset, handles.data() + 0 * handleSize, handleSize);
    std::memcpy(data + raygenOffset + raygenStride, handles.data() + 1 * handleSize, handleSize);
    std::memcpy(data + missOffset, handles.data() + 2 * handleSize, handleSize);
    std::memcpy(data + missOffset + hitStride, handles.data() + 3 * handleSize, handleSize);
    std::memcpy(data + hitOffset, handles.data() + 4 * handleSize, handleSize);
    std::memcpy(data + hitOffset + hitStride, handles.data() + 5 * handleSize, handleSize);
    vmaFlushAllocation(allocator_, sbtBuffer_.allocation, 0, VK_WHOLE_SIZE);
    sbt_.raygen = {sbtBaseAddress + raygenOffset, raygenStride, 2ull * raygenStride};
    sbt_.miss = {sbtBaseAddress + missOffset, hitStride, 2ull * hitStride};
    sbt_.hit = {sbtBaseAddress + hitOffset, hitStride, 2ull * hitStride};
    sbt_.callable = {0, 0, 0};
    return true;
}

inline void Renderer::computeDescriptorOffsets() {
    if (descriptorBuffer_.buffer != VK_NULL_HANDLE) {
        return;
    }
    const VkDeviceSize alignment = std::max<VkDeviceSize>(1, dbProps_.descriptorBufferOffsetAlignment);
    VkDeviceSize total = 0;
    for (auto& entry : layoutRefs_) {
        VkDeviceSize size = 0;
        vkGetDescriptorSetLayoutSizeEXT(device_, entry.first, &size);
        total = alignUp(total, alignment);
        *entry.second = total;
        total += size;
    }
    descriptorBufferSize_ = alignUp(total, alignment);
    descriptorBuffer_ = createBuffer(descriptorBufferSize_ + alignment,
                                     VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     true, 0, false);
    if (descriptorBuffer_.buffer == VK_NULL_HANDLE || descriptorBuffer_.mapped == nullptr) {
        std::fprintf(stderr, "创建 descriptor buffer 失败\n");
        fatalError_ = true;
        return;
    }
    descriptorBufferBindingAddress_ = alignUp(descriptorBuffer_.address, alignment);
    descriptorBufferBaseOffset_ = descriptorBufferBindingAddress_ - descriptorBuffer_.address;
    std::memset(descriptorBuffer_.mapped, 0, static_cast<size_t>(descriptorBuffer_.size));
}

inline void Renderer::writeDescriptors() {
    if (descriptorBuffer_.mapped == nullptr) {
        return;
    }
    auto* base = static_cast<uint8_t*>(descriptorBuffer_.mapped) + descriptorBufferBaseOffset_;
    auto bindingOffset = [&](VkDescriptorSetLayout layout, uint32_t binding) -> VkDeviceSize {
        VkDeviceSize offset = 0;
        vkGetDescriptorSetLayoutBindingOffsetEXT(device_, layout, binding, &offset);
        return offset;
    };
    auto writeBufferDesc = [&](const Pass& pass, uint32_t binding, const Buffer& buffer,
                               VkDeviceSize range) {
        VkDescriptorAddressInfoEXT addr{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        addr.address = buffer.address;
        addr.range = range;
        addr.format = VK_FORMAT_UNDEFINED;
        VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        info.data.pStorageBuffer = &addr;
        vkGetDescriptorEXT(device_, &info, dbProps_.storageBufferDescriptorSize,
                           base + pass.setOffset + bindingOffset(pass.setLayout, binding));
    };
    auto writeCombinedDesc = [&](const Pass& pass, uint32_t binding, VkImageView view,
                                 VkSampler sampler) {
        VkDescriptorImageInfo image{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        info.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        info.data.pCombinedImageSampler = &image;
        vkGetDescriptorEXT(device_, &info, dbProps_.combinedImageSamplerDescriptorSize,
                           base + pass.setOffset + bindingOffset(pass.setLayout, binding));
    };
    auto writeScene = [&](const Pass& pass) {
        writeBufferDesc(pass, bindings::kGlobals, globalsBufferForWrite_, globalsBufferForWrite_.size);
        writeBufferDesc(pass, bindings::kMaterials, materialBuffer_, materialBuffer_.size);
        writeBufferDesc(pass, bindings::kTriangles, triangleBuffer_, triangleBuffer_.size);
        writeBufferDesc(pass, bindings::kInstanceGroups, instanceGroupBuffer_,
                        instanceGroupBuffer_.size);
        writeBufferDesc(pass, bindings::kLights, lightBuffer_, lightBuffer_.size);
        writeCombinedDesc(pass, bindings::kBaseTex, baseTex_.view, baseSampler_);
        writeCombinedDesc(pass, bindings::kRoughTex, roughTex_.view, baseSampler_);
    };
    auto writeGlobals = [&](const Pass& pass) {
        writeBufferDesc(pass, bindings::kGlobals, globalsBufferForWrite_, globalsBufferForWrite_.size);
    };

    writeGlobals(passPrimary_);
    writeBufferDesc(passPrimary_, bindings::kA, pathBuffer_, pathBuffer_.size);
    writeBufferDesc(passPrimary_, bindings::kB, countsBuffer_, countsBuffer_.size);
    writeBufferDesc(passPrimary_, bindings::kC, jitterBuffer_, jitterBuffer_.size);

    writeScene(passPrimaryInfo_);
    writeBufferDesc(passPrimaryInfo_, bindings::kA, hitBuffer_, hitBuffer_.size);
    writeBufferDesc(passPrimaryInfo_, bindings::kB, primaryInfoBuffer_, primaryInfoBuffer_.size);
    writeBufferDesc(passPrimaryInfo_, bindings::kC, pathBuffer_, pathBuffer_.size);

    writeScene(passShade_);
    writeBufferDesc(passShade_, bindings::kA, pathBuffer_, pathBuffer_.size);
    writeBufferDesc(passShade_, bindings::kB, countsBuffer_, countsBuffer_.size);
    writeBufferDesc(passShade_, bindings::kC, hitBuffer_, hitBuffer_.size);
    writeBufferDesc(passShade_, bindings::kD, shadowTaskBuffer_, shadowTaskBuffer_.size);
    writeBufferDesc(passShade_, bindings::kE, slotRadianceBuffer_, slotRadianceBuffer_.size);
    writeBufferDesc(passShade_, bindings::kF, slotGuideBuffer_, slotGuideBuffer_.size);
    writeBufferDesc(passShade_, bindings::kG, slotTagBuffer_, slotTagBuffer_.size);
    writeBufferDesc(passShade_, bindings::kH, emissionBuffer_, emissionBuffer_.size);

    writeGlobals(passShadowResolve_);
    writeBufferDesc(passShadowResolve_, bindings::kA, shadowTaskBuffer_, shadowTaskBuffer_.size);
    writeBufferDesc(passShadowResolve_, bindings::kB, shadowResultBuffer_, shadowResultBuffer_.size);
    writeBufferDesc(passShadowResolve_, bindings::kC, slotRadianceBuffer_, slotRadianceBuffer_.size);
    writeBufferDesc(passShadowResolve_, bindings::kD, countsBuffer_, countsBuffer_.size);

    writeGlobals(passBuildIndirect_);
    writeBufferDesc(passBuildIndirect_, bindings::kA, countsBuffer_, countsBuffer_.size);
    writeBufferDesc(passBuildIndirect_, bindings::kB, indirectBuffer_, indirectBuffer_.size);

    writeScene(passRestirInit_);
    writeBufferDesc(passRestirInit_, bindings::kA, primaryInfoBuffer_, primaryInfoBuffer_.size);
    writeBufferDesc(passRestirInit_, bindings::kB, reservoirInitBuffer_, reservoirInitBuffer_.size);

    writeScene(passRestirTemporal_);
    writeBufferDesc(passRestirTemporal_, bindings::kA, primaryInfoBuffer_, primaryInfoBuffer_.size);
    writeBufferDesc(passRestirTemporal_, bindings::kB, jitterBuffer_, jitterBuffer_.size);
    writeBufferDesc(passRestirTemporal_, bindings::kC, reservoirInitBuffer_, reservoirInitBuffer_.size);
    writeBufferDesc(passRestirTemporal_, bindings::kD, reservoirHistoryBuffer_,
                    reservoirHistoryBuffer_.size);
    writeBufferDesc(passRestirTemporal_, bindings::kE, reservoirTemporalBuffer_,
                    reservoirTemporalBuffer_.size);

    writeScene(passRestirSpatial_);
    writeBufferDesc(passRestirSpatial_, bindings::kA, primaryInfoBuffer_, primaryInfoBuffer_.size);
    writeBufferDesc(passRestirSpatial_, bindings::kB, reservoirTemporalBuffer_,
                    reservoirTemporalBuffer_.size);
    writeBufferDesc(passRestirSpatial_, bindings::kC, reservoirHistoryBuffer_,
                    reservoirHistoryBuffer_.size);

    writeScene(passRestirShadow_);
    writeBufferDesc(passRestirShadow_, bindings::kA, primaryInfoBuffer_, primaryInfoBuffer_.size);
    writeBufferDesc(passRestirShadow_, bindings::kB, reservoirHistoryBuffer_,
                    reservoirHistoryBuffer_.size);
    writeBufferDesc(passRestirShadow_, bindings::kC, shadowTaskBuffer_, shadowTaskBuffer_.size);
    writeBufferDesc(passRestirShadow_, bindings::kD, countsBuffer_, countsBuffer_.size);

    writeScene(passSvgfTemporal_);
    writeBufferDesc(passSvgfTemporal_, bindings::kA, jitterBuffer_, jitterBuffer_.size);
    writeBufferDesc(passSvgfTemporal_, bindings::kB, historyColorBuffer_, historyColorBuffer_.size);
    writeBufferDesc(passSvgfTemporal_, bindings::kC, historyM2Buffer_, historyM2Buffer_.size);
    writeBufferDesc(passSvgfTemporal_, bindings::kD, slotRadianceBuffer_, slotRadianceBuffer_.size);
    writeBufferDesc(passSvgfTemporal_, bindings::kE, slotGuideBuffer_, slotGuideBuffer_.size);
    writeBufferDesc(passSvgfTemporal_, bindings::kF, primaryInfoBuffer_, primaryInfoBuffer_.size);

    writeScene(passSvgfAtrous_);
    writeBufferDesc(passSvgfAtrous_, bindings::kA, historyColorBuffer_, historyColorBuffer_.size);
    writeBufferDesc(passSvgfAtrous_, bindings::kB, historyM2Buffer_, historyM2Buffer_.size);
    writeBufferDesc(passSvgfAtrous_, bindings::kC, filterColorBuffer_, filterColorBuffer_.size);
    writeBufferDesc(passSvgfAtrous_, bindings::kD, filterVarBuffer_, filterVarBuffer_.size);
    writeBufferDesc(passSvgfAtrous_, bindings::kE, slotGuideBuffer_, slotGuideBuffer_.size);

    // RT 集合。
    {
        VkDescriptorAddressInfoEXT addr{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
        auto writeRtBuffer = [&](uint32_t binding, const Buffer& buffer) {
            addr.address = buffer.address;
            addr.range = buffer.size;
            addr.format = VK_FORMAT_UNDEFINED;
            VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
            info.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            info.data.pStorageBuffer = &addr;
            vkGetDescriptorEXT(device_, &info, dbProps_.storageBufferDescriptorSize,
                               base + rtSetOffset_ + bindingOffset(rtSetLayout_, binding));
        };
        writeRtBuffer(0, pathBuffer_);
        writeRtBuffer(1, hitBuffer_);
        writeRtBuffer(2, shadowTaskBuffer_);
        writeRtBuffer(3, shadowResultBuffer_);
        writeRtBuffer(4, countsBuffer_);
        VkAccelerationStructureDeviceAddressInfoKHR asAddressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        asAddressInfo.accelerationStructure = tlas_.handle;
        VkDescriptorGetInfoEXT asInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT};
        asInfo.type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        asInfo.data.accelerationStructure =
            vkGetAccelerationStructureDeviceAddressKHR(device_, &asAddressInfo);
        vkGetDescriptorEXT(device_, &asInfo, dbProps_.accelerationStructureDescriptorSize,
                           base + rtSetOffset_ + bindingOffset(rtSetLayout_, 5));
    }

    // display 集合：全屏三角形合成直接读取分域滤波结果。
    {
        Pass displayPass;
        displayPass.setLayout = displaySetLayout_;
        displayPass.setOffset = displaySetOffset_;
        writeScene(displayPass);
        writeBufferDesc(displayPass, bindings::kA, slotRadianceBuffer_, slotRadianceBuffer_.size);
        writeBufferDesc(displayPass, bindings::kB, slotGuideBuffer_, slotGuideBuffer_.size);
        writeBufferDesc(displayPass, bindings::kC, primaryInfoBuffer_, primaryInfoBuffer_.size);
        writeBufferDesc(displayPass, bindings::kD, filterColorBuffer_, filterColorBuffer_.size);
        writeBufferDesc(displayPass, bindings::kE, slotTagBuffer_, slotTagBuffer_.size);
        writeBufferDesc(displayPass, bindings::kF, emissionBuffer_, emissionBuffer_.size);
    }
    vmaFlushAllocation(allocator_, descriptorBuffer_.allocation, 0, VK_WHOLE_SIZE);
    descriptorsCreated_ = true;
}

inline bool Renderer::createRenderResources() {
    const VkExtent2D ext = extent_;
    if (ext.width == 0 || ext.height == 0) {
        return false;
    }
    pixelCount_ = ext.width * ext.height;
    pathCapacity_ = pixelCount_ * 2;
    shadowCapacity_ = pixelCount_ * 2;
    const VkDeviceSize n = pixelCount_;
    const VkDeviceSize slots = n * kSlotsPerPixel;
    const VkDeviceSize pathBytes = static_cast<VkDeviceSize>(pathCapacity_) * 2ull *
                                   sizeof(GpuPath);
    const VkDeviceSize hitBytes = static_cast<VkDeviceSize>(pathCapacity_) * sizeof(GpuHit);
    const VkDeviceSize shadowTaskBytes =
        static_cast<VkDeviceSize>(shadowCapacity_) * sizeof(GpuShadowTask);
    const VkDeviceSize shadowResultBytes = static_cast<VkDeviceSize>(shadowCapacity_) * sizeof(uint32_t);
    const VkDeviceSize slotRadianceBytes = slots * sizeof(float) * 4;
    const VkDeviceSize slotGuideBytes = slots * 2ull * sizeof(GpuGuide);
    const VkDeviceSize slotTagBytes = slots * sizeof(uint32_t);
    const VkDeviceSize emissionBytes = n * sizeof(float) * 4;
    const VkDeviceSize primaryInfoBytes = n * 2ull * sizeof(GpuPrimaryInfo);
    const VkDeviceSize jitterBytes = n * 2ull * sizeof(float) * 2;
    const VkDeviceSize reservoirInitBytes = n * sizeof(GpuReservoir);
    const VkDeviceSize reservoirTemporalBytes = n * sizeof(GpuReservoir);
    const VkDeviceSize reservoirHistoryBytes = n * 2ull * sizeof(GpuReservoir);
    const VkDeviceSize historyColorBytes = slots * 2ull * sizeof(float) * 4;
    const VkDeviceSize historyM2Bytes = historyColorBytes;
    const VkDeviceSize filterColorBytes = slots * 2ull * sizeof(float) * 4;
    const VkDeviceSize filterVarBytes = slots * 2ull * sizeof(float) * 4;
    const VkShaderStageFlags storageUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    auto make = [&](Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
        b = createBuffer(size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
        return b.buffer != VK_NULL_HANDLE;
    };
    if (!make(pathBuffer_, pathBytes, storageUsage) ||
        !make(hitBuffer_, hitBytes, storageUsage) ||
        !make(shadowTaskBuffer_, shadowTaskBytes, storageUsage) ||
        !make(shadowResultBuffer_, shadowResultBytes, storageUsage) ||
        !make(slotRadianceBuffer_, slotRadianceBytes, storageUsage) ||
        !make(slotGuideBuffer_, slotGuideBytes, storageUsage) ||
        !make(slotTagBuffer_, slotTagBytes, storageUsage) ||
        !make(emissionBuffer_, emissionBytes, storageUsage) ||
        !make(primaryInfoBuffer_, primaryInfoBytes, storageUsage) ||
        !make(jitterBuffer_, jitterBytes, storageUsage) ||
        !make(reservoirInitBuffer_, reservoirInitBytes, storageUsage) ||
        !make(reservoirTemporalBuffer_, reservoirTemporalBytes, storageUsage) ||
        !make(reservoirHistoryBuffer_, reservoirHistoryBytes, storageUsage) ||
        !make(historyColorBuffer_, historyColorBytes, storageUsage) ||
        !make(historyM2Buffer_, historyM2Bytes, storageUsage) ||
        !make(filterColorBuffer_, filterColorBytes, storageUsage) ||
        !make(filterVarBuffer_, filterVarBytes, storageUsage) ||
        !make(indirectBuffer_, 64, storageUsage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) {
        return false;
    }
    // Globals 缓冲：两个 256 字节槽，索引由 push constant 选择。
    globalsBufferForWrite_ = createBuffer(2ull * sizeof(GpuGlobals),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                          true, 0, false);
    if (globalsBufferForWrite_.buffer == VK_NULL_HANDLE) {
        return false;
    }
    std::memset(globalsBufferForWrite_.mapped, 0, static_cast<size_t>(globalsBufferForWrite_.size));

    {
        // 帧标记/guide/历史初始化为 0；帧序号从 1 开始，因此不会误匹配。
        VkCommandBuffer cmd = beginTempCommand();
        vkCmdFillBuffer(cmd, slotTagBuffer_.buffer, 0, slotTagBuffer_.size, 0);
        vkCmdFillBuffer(cmd, slotGuideBuffer_.buffer, 0, slotGuideBuffer_.size, 0);
        vkCmdFillBuffer(cmd, reservoirHistoryBuffer_.buffer, 0, reservoirHistoryBuffer_.size, 0);
        vkCmdFillBuffer(cmd, historyColorBuffer_.buffer, 0, historyColorBuffer_.size, 0);
        vkCmdFillBuffer(cmd, historyM2Buffer_.buffer, 0, historyM2Buffer_.size, 0);
        barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT |
                    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        endTempCommand(cmd, true);
    }
    if (!descriptorsCreated_ || descriptorBuffer_.buffer == VK_NULL_HANDLE) {
        computeDescriptorOffsets();
        if (fatalError_) {
            return false;
        }
    }
    writeDescriptors();
    return !fatalError_;
}

inline void Renderer::destroyRenderResources() {
    destroyBuffer(pathBuffer_);
    destroyBuffer(hitBuffer_);
    destroyBuffer(shadowTaskBuffer_);
    destroyBuffer(shadowResultBuffer_);
    destroyBuffer(slotRadianceBuffer_);
    destroyBuffer(slotGuideBuffer_);
    destroyBuffer(slotTagBuffer_);
    destroyBuffer(emissionBuffer_);
    destroyBuffer(primaryInfoBuffer_);
    destroyBuffer(jitterBuffer_);
    destroyBuffer(reservoirInitBuffer_);
    destroyBuffer(reservoirTemporalBuffer_);
    destroyBuffer(reservoirHistoryBuffer_);
    destroyBuffer(historyColorBuffer_);
    destroyBuffer(historyM2Buffer_);
    destroyBuffer(filterColorBuffer_);
    destroyBuffer(filterVarBuffer_);
    destroyBuffer(indirectBuffer_);
    destroyBuffer(globalsBufferForWrite_);
    pixelCount_ = 0;
    pathCapacity_ = 0;
    shadowCapacity_ = 0;
}

inline bool Renderer::createFrameResources() {
    VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreInfo.pNext = &typeInfo;
    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &timeline_) != VK_SUCCESS) {
        std::fprintf(stderr, "创建 timeline semaphore 失败\n");
        return false;
    }
    for (Frame& frame : frames_) {
        createFrameObjects(frame);
        if (frame.pool == VK_NULL_HANDLE || frame.fence == VK_NULL_HANDLE ||
            frame.imageAvailable == VK_NULL_HANDLE) {
            return false;
        }
    }
    // 帧序号从 1 开始，0 保留给未初始化/失效标签。
    frameCounter_ = 1;
    frameSlot_ = 1;
    timelineValue_ = 0;
    return true;
}

inline void Renderer::createFrameObjects(Frame& frame) {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &frame.pool) != VK_SUCCESS) {
        return;
    }
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = frame.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &alloc, &frame.cmd) != VK_SUCCESS) {
        return;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(device_, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS) {
        return;
    }
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &frame.imageAvailable) != VK_SUCCESS) {
        return;
    }
}

inline void Renderer::destroyFrameObjects(Frame& frame) {
    if (frame.imageAvailable != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, frame.imageAvailable, nullptr);
        frame.imageAvailable = VK_NULL_HANDLE;
    }
    if (frame.fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, frame.fence, nullptr);
        frame.fence = VK_NULL_HANDLE;
    }
    if (frame.pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, frame.pool, nullptr);
        frame.pool = VK_NULL_HANDLE;
        frame.cmd = VK_NULL_HANDLE;
    }
}

} // namespace rae
