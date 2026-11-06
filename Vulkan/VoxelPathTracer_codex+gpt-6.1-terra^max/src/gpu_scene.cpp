#include "gpu_scene.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace arena {
namespace {
void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
VkDeviceAddress alignUp(VkDeviceAddress address, VkDeviceSize alignment)
{
    return (address + alignment - 1) & ~(alignment - 1);
}
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = srcStage; memory.srcAccessMask = srcAccess;
    memory.dstStageMask = dstStage; memory.dstAccessMask = dstAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(cmd, &dependency);
}
}
Buffer GpuScene::makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags)
{
    Buffer b{}; b.size = size;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size; create.usage = usage; create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(vk.device, &create, nullptr, &b.buffer), "vkCreateBuffer");
    try {
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(vk.device, b.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(vk.physical, &memory);
        std::uint32_t index = memory.memoryTypeCount;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) { index = i; break; }
        }
        if (index == memory.memoryTypeCount) throw std::runtime_error("no compatible Vulkan memory type for buffer");
        VkMemoryAllocateFlagsInfo addressFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        addressFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = index;
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) allocation.pNext = &addressFlags;
        check(vkAllocateMemory(vk.device, &allocation, nullptr, &b.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(vk.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; info.buffer = b.buffer;
            b.address = vkGetBufferDeviceAddress(vk.device, &info);
        }
        return b;
    } catch (...) { destroyBuffer(b); throw; }
}
void GpuScene::destroyBuffer(Buffer& b)
{
    if (b.buffer) vkDestroyBuffer(vk.device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(vk.device, b.memory, nullptr);
    b = {};
}
Image GpuScene::makeImage(std::uint32_t size, std::uint32_t levels, std::uint32_t layers, VkFormat format)
{
    Image image{};
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D; create.format = format; create.extent = {size, size, 1};
    create.mipLevels = levels; create.arrayLayers = layers; create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL; create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE; create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(vk.device, &create, nullptr, &image.image), "vkCreateImage");
    try {
        VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(vk.device, image.image, &requirements);
        VkPhysicalDeviceMemoryProperties properties{}; vkGetPhysicalDeviceMemoryProperties(vk.physical, &properties);
        std::uint32_t index = properties.memoryTypeCount;
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { index = i; break; }
        if (index == properties.memoryTypeCount) throw std::runtime_error("no device-local memory type for texture");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = index;
        check(vkAllocateMemory(vk.device, &allocation, nullptr, &image.memory), "texture memory allocation");
        check(vkBindImageMemory(vk.device, image.image, image.memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        check(vkCreateImageView(vk.device, &view, nullptr, &image.view), "vkCreateImageView");
        return image;
    } catch (...) { destroyImage(image); throw; }
}
void GpuScene::destroyImage(Image& image)
{
    if (image.view) vkDestroyImageView(vk.device, image.view, nullptr);
    if (image.image) vkDestroyImage(vk.device, image.image, nullptr);
    if (image.memory) vkFreeMemory(vk.device, image.memory, nullptr);
    image = {};
}
void GpuScene::upload(const Buffer& b, const void* data, VkDeviceSize size)
{
    void* mapped = nullptr; check(vkMapMemory(vk.device, b.memory, 0, size, 0, &mapped), "vkMapMemory");
    std::memcpy(mapped, data, static_cast<std::size_t>(size)); vkUnmapMemory(vk.device, b.memory);
}
VkAccelerationStructureKHR GpuScene::makeAS(const Buffer& buffer, VkAccelerationStructureTypeKHR type)
{
    VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = buffer.buffer; create.size = buffer.size; create.type = type;
    VkAccelerationStructureKHR result = VK_NULL_HANDLE;
    check(vkCreateAccelerationStructureKHR(vk.device, &create, nullptr, &result), "vkCreateAccelerationStructureKHR");
    return result;
}
VkCommandBuffer GpuScene::begin()
{
    check(vkResetCommandPool(vk.device, pool, 0), "vkResetCommandPool");
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool; alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE; check(vkAllocateCommandBuffers(vk.device, &alloc, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &info), "vkBeginCommandBuffer"); return cmd;
}
void GpuScene::execute(VkCommandBuffer cmd)
{
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    if (fence) { vkDestroyFence(vk.device, fence, nullptr); fence = VK_NULL_HANDLE; }
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(vk.device, &f, nullptr, &fence), "vkCreateFence");
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &command;
    check(vkQueueSubmit2(vk.graphics, 1, &submit, fence), "vkQueueSubmit2");
    fencePending = true;
    const VkResult waited = vkWaitForFences(vk.device, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    fencePending = false;
    check(waited, "vkWaitForFences");
    vkFreeCommandBuffers(vk.device, pool, 1, &cmd);
}
void GpuScene::initialize(const Scene& scene)
{
    struct Info { std::uint32_t material, surface; };
    struct Batch { std::uint32_t first, count; };
    std::vector<Vertex> ordered; std::vector<Info> metadata; std::vector<Batch> batches;
    std::vector<std::uint32_t> remap(scene.triangles.size());
    for (std::size_t material = 0; material < scene.materials.size(); ++material) {
        const auto first = static_cast<std::uint32_t>(metadata.size());
        for (std::size_t i = 0; i < scene.triangles.size(); ++i) {
            const auto& t = scene.triangles[i];
            if (t.material != material) continue;
            remap[i] = static_cast<std::uint32_t>(metadata.size());
            ordered.insert(ordered.end(), t.vertices.begin(), t.vertices.end()); metadata.push_back({t.material, t.surface});
        }
        const auto count = static_cast<std::uint32_t>(metadata.size()) - first;
        if (count) batches.push_back({first, count});
    }
    if (batches.empty()) throw std::runtime_error("scene has no geometry");
    if (batches.size() > vk.asProperties.maxInstanceCount || metadata.size() > vk.asProperties.maxPrimitiveCount || metadata.size() >= (1u << 24))
        throw std::runtime_error("scene geometry exceeds acceleration structure or instance index capacity");
    const VkDeviceSize vertexBytes = ordered.size() * sizeof(Vertex), infoBytes = metadata.size() * sizeof(Info);
    const auto address = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    vertices = makeBuffer(vertexBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    triangleInfo = makeBuffer(infoBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    temporary.push_back(makeBuffer(vertexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host)); upload(temporary.back(), ordered.data(), vertexBytes);
    temporary.push_back(makeBuffer(infoBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host)); upload(temporary.back(), metadata.data(), infoBytes);

    if (scene.materials.size() > vk.properties.limits.maxImageArrayLayers)
        throw std::runtime_error("material texture layers exceed maxImageArrayLayers");
    std::vector<GpuMaterial> materialData;
    for (const auto& m : scene.materials) {
        GpuMaterial data{};
        for (std::size_t i = 0; i < 3; ++i) { data.absorption[i] = m.absorption[i]; data.emission[i] = m.emission[i]; }
        data.ior = m.ior; data.type = static_cast<std::uint32_t>(m.type); materialData.push_back(data);
    }
    std::vector<GpuLight> lightData;
    for (const auto& light : scene.lights) lightData.push_back({remap[light.triangle], light.area, light.cumulativePower});
    lightCount = static_cast<std::uint32_t>(lightData.size()); lightPower = scene.lightPower;
    if (lightData.empty()) lightData.push_back({});
    const VkDeviceSize materialBytes = materialData.size() * sizeof(GpuMaterial), lightBytes = lightData.size() * sizeof(GpuLight);
    materials = makeBuffer(materialBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    lights = makeBuffer(lightBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    temporary.push_back(makeBuffer(materialBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host)); upload(temporary.back(), materialData.data(), materialBytes);
    temporary.push_back(makeBuffer(lightBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host)); upload(temporary.back(), lightData.data(), lightBytes);
    const auto levels = static_cast<std::uint32_t>(scene.materials.front().mip.size());
    const auto layers = static_cast<std::uint32_t>(scene.materials.size());
    baseColorTexture = makeImage(scene.textureSize, levels, layers, VK_FORMAT_R8G8B8A8_SRGB);
    roughnessTexture = makeImage(scene.textureSize, levels, layers, VK_FORMAT_R8_UNORM);
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR; samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = static_cast<float>(levels - 1);
    check(vkCreateSampler(vk.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
    std::vector<std::uint8_t> colorData, roughnessData;
    std::vector<VkBufferImageCopy> colorRegions, roughnessRegions;
    const auto appendMip = [](std::vector<std::uint8_t>& dst, std::vector<VkBufferImageCopy>& regions,
        const std::vector<std::uint8_t>& src, std::uint32_t size, std::uint32_t layer, std::uint32_t mip) {
        dst.resize((dst.size() + 3) & ~std::size_t(3));
        VkBufferImageCopy region{}; region.bufferOffset = dst.size();
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1}; region.imageExtent = {size, size, 1};
        regions.push_back(region); dst.insert(dst.end(), src.begin(), src.end());
    };
    for (std::uint32_t layer = 0; layer < layers; ++layer) for (std::uint32_t mip = 0; mip < levels; ++mip) {
        const auto& level = scene.materials[layer].mip[mip];
        appendMip(colorData, colorRegions, level.color, level.size, layer, mip);
        appendMip(roughnessData, roughnessRegions, level.roughness, level.size, layer, mip);
    }
    temporary.push_back(makeBuffer(colorData.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host)); upload(temporary.back(), colorData.data(), colorData.size());
    temporary.push_back(makeBuffer(roughnessData.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host)); upload(temporary.back(), roughnessData.data(), roughnessData.size());
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.queueFamilyIndex = vk.graphicsFamily;
    check(vkCreateCommandPool(vk.device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR; queryInfo.queryCount = static_cast<std::uint32_t>(batches.size());
    check(vkCreateQueryPool(vk.device, &queryInfo, nullptr, &query), "vkCreateQueryPool");
    const auto scratchAlignment = vk.asProperties.minAccelerationStructureScratchOffsetAlignment;
    std::vector<VkAccelerationStructureGeometryKHR> geometries(batches.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds(batches.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(batches.size());
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePointers(batches.size());
    for (std::size_t i = 0; i < batches.size(); ++i) {
        auto& g = geometries[i]; g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR; g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR; g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress = vertices.address + static_cast<VkDeviceSize>(batches[i].first) * 3 * sizeof(Vertex);
        g.geometry.triangles.vertexStride = sizeof(Vertex); g.geometry.triangles.maxVertex = batches[i].count * 3 - 1;
        g.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        auto& b = builds[i]; b.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        b.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; b.geometryCount = 1; b.pGeometries = &g;
        VkAccelerationStructureBuildSizesInfoKHR size{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &batches[i].count, &size);
        BottomLevel original{}; original.firstTriangle = batches[i].first;
        original.storage = makeBuffer(size.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        uncompacted.push_back(original);
        uncompacted.back().handle = makeAS(uncompacted.back().storage, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        b.dstAccelerationStructure = uncompacted.back().handle;
        temporary.push_back(makeBuffer(size.buildScratchSize + scratchAlignment - 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        b.scratchData.deviceAddress = alignUp(temporary.back().address, scratchAlignment);
        ranges[i].primitiveCount = batches[i].count; rangePointers[i] = &ranges[i];
    }
    auto cmd = begin();
    barrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy copy{0, 0, vertexBytes}; vkCmdCopyBuffer(cmd, temporary[0].buffer, vertices.buffer, 1, &copy);
    copy.size = infoBytes; vkCmdCopyBuffer(cmd, temporary[1].buffer, triangleInfo.buffer, 1, &copy);
    copy.size = materialBytes; vkCmdCopyBuffer(cmd, temporary[2].buffer, materials.buffer, 1, &copy);
    copy.size = lightBytes; vkCmdCopyBuffer(cmd, temporary[3].buffer, lights.buffer, 1, &copy);
    const auto imageTransitions = [&](VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags2 sourceStage, VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage, VkAccessFlags2 destinationAccess) {
        VkImageMemoryBarrier2 imageBarriers[2]{};
        const VkImage images[2]{baseColorTexture.image, roughnessTexture.image};
        for (int i = 0; i < 2; ++i) {
            auto& b = imageBarriers[i]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = sourceStage; b.srcAccessMask = sourceAccess; b.dstStageMask = destinationStage; b.dstAccessMask = destinationAccess;
            b.oldLayout = oldLayout; b.newLayout = newLayout; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = images[i]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        }
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = 2; dependency.pImageMemoryBarriers = imageBarriers;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    imageTransitions(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, temporary[4].buffer, baseColorTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<std::uint32_t>(colorRegions.size()), colorRegions.data());
    vkCmdCopyBufferToImage(cmd, temporary[5].buffer, roughnessTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<std::uint32_t>(roughnessRegions.size()), roughnessRegions.data());
    imageTransitions(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);
    vkCmdResetQueryPool(cmd, query, 0, static_cast<std::uint32_t>(batches.size()));
    vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<std::uint32_t>(builds.size()), builds.data(), rangePointers.data());
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    std::vector<VkAccelerationStructureKHR> handles; for (const auto& b : uncompacted) handles.push_back(b.handle);
    vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, static_cast<std::uint32_t>(handles.size()), handles.data(),
        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, query, 0);
    execute(cmd);
    std::vector<VkDeviceSize> compactSizes(batches.size());
    check(vkGetQueryPoolResults(vk.device, query, 0, static_cast<std::uint32_t>(batches.size()),
        compactSizes.size() * sizeof(VkDeviceSize), compactSizes.data(), sizeof(VkDeviceSize), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "BLAS compact size query");
    for (std::size_t i = 0; i < batches.size(); ++i) {
        BottomLevel compact{}; compact.firstTriangle = batches[i].first;
        compact.storage = makeBuffer(compactSizes[i], VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        bottom.push_back(compact); bottom.back().handle = makeAS(bottom.back().storage, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
    }
    std::vector<VkAccelerationStructureInstanceKHR> instances(bottom.size());
    for (std::size_t i = 0; i < bottom.size(); ++i) {
        auto& instance = instances[i]; instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
        instance.instanceCustomIndex = bottom[i].firstTriangle; instance.mask = 0xff;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; info.accelerationStructure = bottom[i].handle;
        instance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(vk.device, &info);
    }
    temporary.push_back(makeBuffer(instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | address, host));
    upload(temporary.back(), instances.data(), instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
    VkAccelerationStructureGeometryKHR topGeometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    topGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    topGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    topGeometry.geometry.instances.data.deviceAddress = temporary.back().address;
    VkAccelerationStructureBuildGeometryInfoKHR topBuild{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    topBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR; topBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    topBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; topBuild.geometryCount = 1; topBuild.pGeometries = &topGeometry;
    const auto instanceCount = static_cast<std::uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR topSize{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &topBuild, &instanceCount, &topSize);
    topStorage = makeBuffer(topSize.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    top = makeAS(topStorage, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR); topBuild.dstAccelerationStructure = top;
    temporary.push_back(makeBuffer(topSize.buildScratchSize + scratchAlignment - 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | address, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    topBuild.scratchData.deviceAddress = alignUp(temporary.back().address, scratchAlignment);
    VkAccelerationStructureBuildRangeInfoKHR topRange{}; topRange.primitiveCount = instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* topRangePointer = &topRange;
    cmd = begin();
    for (std::size_t i = 0; i < bottom.size(); ++i) {
        VkCopyAccelerationStructureInfoKHR copyAS{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
        copyAS.src = uncompacted[i].handle; copyAS.dst = bottom[i].handle; copyAS.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        vkCmdCopyAccelerationStructureKHR(cmd, &copyAS);
    }
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_HOST_BIT,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_HOST_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &topBuild, &topRangePointer);
    barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    execute(cmd);
    for (auto& b : uncompacted) { vkDestroyAccelerationStructureKHR(vk.device, b.handle, nullptr); destroyBuffer(b.storage); }
    uncompacted.clear();
    for (auto& b : temporary) destroyBuffer(b);
    temporary.clear();
}
GpuScene::~GpuScene()
{
    if (fencePending) vkWaitForFences(vk.device, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
    if (top) vkDestroyAccelerationStructureKHR(vk.device, top, nullptr);
    destroyBuffer(topStorage);
    for (auto& b : bottom) { if (b.handle) vkDestroyAccelerationStructureKHR(vk.device, b.handle, nullptr); destroyBuffer(b.storage); }
    for (auto& b : uncompacted) { if (b.handle) vkDestroyAccelerationStructureKHR(vk.device, b.handle, nullptr); destroyBuffer(b.storage); }
    for (auto& b : temporary) destroyBuffer(b);
    if (sampler) vkDestroySampler(vk.device, sampler, nullptr);
    destroyImage(roughnessTexture); destroyImage(baseColorTexture);
    destroyBuffer(lights); destroyBuffer(materials); destroyBuffer(triangleInfo); destroyBuffer(vertices);
    if (fence) vkDestroyFence(vk.device, fence, nullptr);
    if (query) vkDestroyQueryPool(vk.device, query, nullptr);
    if (pool) vkDestroyCommandPool(vk.device, pool, nullptr);
}
}
