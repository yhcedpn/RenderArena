#include "renderer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>

namespace arena {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(result));
}
VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) { return (value + alignment - 1) & ~(alignment - 1); }
std::vector<std::uint32_t> shaderCode(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open shader: " + path.string());
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) % 4 != 0) throw std::runtime_error("invalid SPIR-V size: " + path.string());
    std::vector<std::uint32_t> code(static_cast<std::size_t>(end) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(end));
    if (!file) throw std::runtime_error("cannot read shader: " + path.string());
    return code;
}
void allMemory(VkCommandBuffer command) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.memoryBarrierCount = 1; info.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(command, &info);
}
Vec3 add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 scale(Vec3 a, float b) { return {a.x*b,a.y*b,a.z*b}; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 normalize(Vec3 v) { const float m = std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); return scale(v,1.0f/m); }
}
Renderer::Renderer(VulkanContext& context, GpuScene& gpuScene, std::filesystem::path base)
    : vk(context), scene(gpuScene), basePath(std::move(base)) {}

void Renderer::initialize() {
    static_assert(sizeof(Push)==96 && sizeof(CameraState)==64 && sizeof(CameraPair)==144);
    createPipeline(); createFrames(); recreate();
}
void Renderer::createPipeline() {
    const auto& limits=vk.properties.limits;
    if (limits.maxPerStageDescriptorStorageBuffers<25 || limits.maxDescriptorSetStorageBuffers<25 || limits.maxPerStageResources<28 ||
        vk.asProperties.maxPerStageDescriptorAccelerationStructures<1 || vk.descriptorProperties.maxDescriptorBufferBindings<1)
        throw std::runtime_error("device descriptor limits cannot hold the renderer layout");
    std::array<VkDescriptorSetLayoutBinding, 28> bindings{};
    for (std::uint32_t i=0; i<bindings.size(); ++i) {
        bindings[i].binding=i; bindings[i].descriptorCount=1; bindings[i].stageFlags=VK_SHADER_STAGE_ALL;
        bindings[i].descriptorType = i==0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR :
            (i==4 || i==5 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    VkDescriptorSetLayoutCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorInfo.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    descriptorInfo.bindingCount=static_cast<std::uint32_t>(bindings.size()); descriptorInfo.pBindings=bindings.data();
    check(vkCreateDescriptorSetLayout(vk.device,&descriptorInfo,nullptr,&descriptorLayout),"vkCreateDescriptorSetLayout");
    VkPushConstantRange range{}; range.stageFlags=VK_SHADER_STAGE_ALL; range.size=sizeof(Push);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount=1; layoutInfo.pSetLayouts=&descriptorLayout; layoutInfo.pushConstantRangeCount=1; layoutInfo.pPushConstantRanges=&range;
    check(vkCreatePipelineLayout(vk.device,&layoutInfo,nullptr,&pipelineLayout),"vkCreatePipelineLayout");
    const auto object = [&](const char* file, VkShaderStageFlagBits stage, VkShaderStageFlags next, VkShaderEXT& result) {
        const auto code=shaderCode(basePath / "shaders" / file);
        VkShaderCreateInfoEXT info{VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT};
        info.stage=stage; info.nextStage=next; info.codeType=VK_SHADER_CODE_TYPE_SPIRV_EXT;
        info.codeSize=code.size()*sizeof(std::uint32_t); info.pCode=code.data(); info.pName="main";
        info.setLayoutCount=1; info.pSetLayouts=&descriptorLayout; info.pushConstantRangeCount=1; info.pPushConstantRanges=&range;
        check(vkCreateShadersEXT(vk.device,1,&info,nullptr,&result),"vkCreateShadersEXT");
    };
    object("primary.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,primary);
    object("scatter.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,scatter);
    object("prepare.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,prepare);
    object("shadow_resolve.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,shadowResolve);
    object("restir_init.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,restirInit);
    object("restir_temporal.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,restirTemporal);
    object("restir_spatial.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,restirSpatial);
    object("svgf_temporal.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,svgfTemporal);
    object("svgf_variance.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,svgfVariance);
    object("svgf_atrous.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,svgfAtrous);
    object("optical_temporal.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,opticalTemporal);
    object("optical_variance.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,opticalVariance);
    object("optical_atrous.comp.spv",VK_SHADER_STAGE_COMPUTE_BIT,0,opticalAtrous);
    object("display.vert.spv",VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_FRAGMENT_BIT,vertex);
    object("display.frag.spv",VK_SHADER_STAGE_FRAGMENT_BIT,0,fragment);
    std::array<VkShaderModule,3> modules{};
    try {
        const std::array<const char*,3> files{"trace.rgen.spv","trace.rmiss.spv","trace.rchit.spv"};
        const std::array<VkShaderStageFlagBits,3> stages{VK_SHADER_STAGE_RAYGEN_BIT_KHR,VK_SHADER_STAGE_MISS_BIT_KHR,VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR};
        std::array<VkPipelineShaderStageCreateInfo,3> shaderStages{};
        for (std::size_t i=0;i<files.size();++i) {
            const auto code=shaderCode(basePath / "shaders" / files[i]);
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=code.size()*sizeof(std::uint32_t); info.pCode=code.data();
            check(vkCreateShaderModule(vk.device,&info,nullptr,&modules[i]),"vkCreateShaderModule");
            shaderStages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; shaderStages[i].stage=stages[i]; shaderStages[i].module=modules[i]; shaderStages[i].pName="main";
        }
        std::array<VkRayTracingShaderGroupCreateInfoKHR,3> groups{};
        for (auto& group : groups) {
            group.sType=VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            group.generalShader=group.closestHitShader=group.anyHitShader=group.intersectionShader=VK_SHADER_UNUSED_KHR;
        }
        groups[0].type=VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; groups[0].generalShader=0;
        groups[1].type=VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; groups[1].generalShader=1;
        groups[2].type=VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR; groups[2].closestHitShader=2;
        VkRayTracingPipelineCreateInfoKHR info{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        info.flags=VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT; info.stageCount=3; info.pStages=shaderStages.data();
        info.groupCount=3; info.pGroups=groups.data(); info.maxPipelineRayRecursionDepth=1; info.layout=pipelineLayout;
        check(vkCreateRayTracingPipelinesKHR(vk.device,VK_NULL_HANDLE,VK_NULL_HANDLE,1,&info,nullptr,&rayPipeline),"vkCreateRayTracingPipelinesKHR");
    } catch (...) { for (auto m:modules) if (m) vkDestroyShaderModule(vk.device,m,nullptr); throw; }
    for (auto m:modules) vkDestroyShaderModule(vk.device,m,nullptr);
    const auto& r=vk.rayProperties;
    const VkDeviceSize stride=alignUp(r.shaderGroupHandleSize,r.shaderGroupHandleAlignment);
    if (stride>r.maxShaderGroupStride || !r.maxRayRecursionDepth) throw std::runtime_error("ray tracing shader group limits are insufficient");
    sbt=scene.makeBuffer(3*(stride+r.shaderGroupBaseAlignment), VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    const auto rayAddress=alignUp(sbt.address,r.shaderGroupBaseAlignment);
    const auto missAddress=alignUp(rayAddress+stride,r.shaderGroupBaseAlignment);
    const auto hitAddress=alignUp(missAddress+stride,r.shaderGroupBaseAlignment);
    raygenRegion={rayAddress,stride,stride}; missRegion={missAddress,stride,stride}; hitRegion={hitAddress,stride,stride};
    std::vector<std::uint8_t> handles(static_cast<std::size_t>(r.shaderGroupHandleSize)*3);
    check(vkGetRayTracingShaderGroupHandlesKHR(vk.device,rayPipeline,0,3,handles.size(),handles.data()),"vkGetRayTracingShaderGroupHandlesKHR");
    std::vector<std::uint8_t> table(static_cast<std::size_t>(sbt.size),0);
    const std::array<VkDeviceAddress,3> addresses{rayAddress,missAddress,hitAddress};
    for (std::size_t i=0;i<3;++i)
        std::memcpy(table.data()+static_cast<std::size_t>(addresses[i]-sbt.address),handles.data()+i*r.shaderGroupHandleSize,r.shaderGroupHandleSize);
    scene.upload(sbt,table.data(),sbt.size);
    VkDeviceSize layoutSize=0; vkGetDescriptorSetLayoutSizeEXT(vk.device,descriptorLayout,&layoutSize);
    const auto alignment=vk.descriptorProperties.descriptorBufferOffsetAlignment;
    if (layoutSize>vk.descriptorProperties.maxResourceDescriptorBufferRange || layoutSize>vk.descriptorProperties.maxSamplerDescriptorBufferRange)
        throw std::runtime_error("descriptor buffer layout exceeds device range");
    descriptor=scene.makeBuffer(layoutSize+alignment, VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
        VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    descriptorOffset=alignUp(descriptor.address,alignment)-descriptor.address;
}
void Renderer::createFrames() {
    for (auto& frame:slots) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; poolInfo.queueFamilyIndex=vk.graphicsFamily;
        check(vkCreateCommandPool(vk.device,&poolInfo,nullptr,&frame.pool),"frame command pool");
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=frame.pool; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
        check(vkAllocateCommandBuffers(vk.device,&allocation,&frame.command),"frame command buffer");
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; check(vkCreateSemaphore(vk.device,&sem,nullptr,&frame.acquired),"acquire semaphore");
        VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; f.flags=VK_FENCE_CREATE_SIGNALED_BIT; check(vkCreateFence(vk.device,&f,nullptr,&frame.fence),"frame fence");
    }
}
void Renderer::destroySwapchain() {
    for (auto sem:presented) if (sem) vkDestroySemaphore(vk.device,sem,nullptr);
    for (auto view:swapViews) if (view) vkDestroyImageView(vk.device,view,nullptr);
    presented.clear(); swapViews.clear(); swapImages.clear(); imageUsed.clear();
    if (swapchain) vkDestroySwapchainKHR(vk.device,swapchain,nullptr);
    swapchain=VK_NULL_HANDLE;
    scene.destroyBuffer(paths0); scene.destroyBuffer(paths1); scene.destroyBuffer(hits); scene.destroyBuffer(counters); scene.destroyBuffer(radiance); scene.destroyBuffer(shadows);
    scene.destroyBuffer(reservoirs); scene.destroyBuffer(guides); scene.destroyBuffer(cameras);
    scene.destroyBuffer(signalFlags); scene.destroyBuffer(histories); scene.destroyBuffer(filters);
    scene.destroyBuffer(opticalPaths); scene.destroyBuffer(opticalGuides0); scene.destroyBuffer(opticalGuides1);
    scene.destroyBuffer(opticalMetadata); scene.destroyBuffer(opticalHistories); scene.destroyBuffer(opticalFilters); scene.destroyBuffer(opticalChains);
    scene.destroyBuffer(prefixRadiance); scene.destroyBuffer(prefixOptical);
}
void Renderer::recreate() {
    check(vkDeviceWaitIdle(vk.device),"device idle for swapchain recreation");
    destroySwapchain();
    int pixelWidth=0,pixelHeight=0;
    if (!SDL_GetWindowSizeInPixels(vk.window,&pixelWidth,&pixelHeight)) throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels: ")+SDL_GetError());
    if (pixelWidth<=0 || pixelHeight<=0) return;
    VkSurfaceCapabilitiesKHR capabilities{}; check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.physical,vk.surface,&capabilities),"surface capabilities");
    std::uint32_t formatCount=0; check(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical,vk.surface,&formatCount,nullptr),"surface format count");
    std::vector<VkSurfaceFormatKHR> formats(formatCount); check(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.physical,vk.surface,&formatCount,formats.data()),"surface formats");
    const auto format=std::find_if(formats.begin(),formats.end(),[](const auto& f) {
        return (f.format==VK_FORMAT_B8G8R8A8_SRGB || f.format==VK_FORMAT_R8G8B8A8_SRGB) && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    VkSurfaceFormatKHR selected{};
    if (format!=formats.end()) selected=*format;
    else if (formats.size()==1 && formats[0].format==VK_FORMAT_UNDEFINED && formats[0].colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        selected={VK_FORMAT_B8G8R8A8_SRGB,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    else throw std::runtime_error("no sRGB swapchain format");
    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) throw std::runtime_error("swapchain images cannot be color attachments");
    swapFormat=selected.format;
    if (capabilities.currentExtent.width!=std::numeric_limits<std::uint32_t>::max()) extent=capabilities.currentExtent;
    else {
        extent.width=std::clamp(static_cast<std::uint32_t>(pixelWidth),capabilities.minImageExtent.width,capabilities.maxImageExtent.width);
        extent.height=std::clamp(static_cast<std::uint32_t>(pixelHeight),capabilities.minImageExtent.height,capabilities.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return;
    const std::uint64_t pixels=static_cast<std::uint64_t>(extent.width)*extent.height;
    constexpr VkDeviceSize pathStride=68, hitStride=16, radianceStride=16, shadowStride=52, reservoirStride=32, guideStride=64;
    const VkDeviceSize pathBytes=2*pixels*pathStride, hitBytes=2*pixels*hitStride, radianceBytes=2*pixels*radianceStride, shadowBytes=2*pixels*shadowStride;
    const VkDeviceSize reservoirBytes=4*pixels*reservoirStride, guideBytes=2*pixels*guideStride;
    const VkDeviceSize flagBytes=pixels*4, historyBytes=2*pixels*32, filterBytes=2*pixels*16;
    const VkDeviceSize opticalPathBytes=2*pixels*52, opticalGuideBytes=2*pixels*64, opticalMetaBytes=4*pixels*20;
    const VkDeviceSize opticalHistoryBytes=4*pixels*32, opticalFilterBytes=4*pixels*16, opticalChainBytes=4*pixels*8*4;
    const auto& limits=vk.properties.limits;
    if (pixels>std::numeric_limits<std::uint32_t>::max()/2 || pathBytes>limits.maxStorageBufferRange ||
        hitBytes>limits.maxStorageBufferRange || radianceBytes>limits.maxStorageBufferRange || shadowBytes>limits.maxStorageBufferRange ||
        reservoirBytes>limits.maxStorageBufferRange || guideBytes>limits.maxStorageBufferRange ||
        flagBytes>limits.maxStorageBufferRange || historyBytes>limits.maxStorageBufferRange || filterBytes>limits.maxStorageBufferRange ||
        opticalPathBytes>limits.maxStorageBufferRange || opticalGuideBytes>limits.maxStorageBufferRange || opticalMetaBytes>limits.maxStorageBufferRange ||
        opticalHistoryBytes>limits.maxStorageBufferRange || opticalFilterBytes>limits.maxStorageBufferRange || opticalChainBytes>limits.maxStorageBufferRange)
        throw std::runtime_error("framebuffer exceeds wavefront storage buffer capacity");
    const auto traceWidth=std::min<std::uint64_t>(1024,static_cast<std::uint64_t>(limits.maxComputeWorkGroupCount[0])*limits.maxComputeWorkGroupSize[0]);
    const auto padded=((2*pixels+traceWidth-1)/traceWidth)*traceWidth;
    if (padded>vk.rayProperties.maxRayDispatchInvocationCount || extent.width>static_cast<std::uint64_t>(limits.maxComputeWorkGroupCount[0])*limits.maxComputeWorkGroupSize[0] ||
        extent.height>static_cast<std::uint64_t>(limits.maxComputeWorkGroupCount[1])*limits.maxComputeWorkGroupSize[1] ||
        ((2*pixels+63)/64+limits.maxComputeWorkGroupCount[0]-1)/limits.maxComputeWorkGroupCount[0]>limits.maxComputeWorkGroupCount[1])
        throw std::runtime_error("framebuffer exceeds ray tracing or dispatch limits");
    std::uint32_t count=capabilities.minImageCount+1;
    if (capabilities.maxImageCount>0) count=std::min(count,capabilities.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(capabilities.supportedCompositeAlpha & alpha)) {
        for (auto choice : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (capabilities.supportedCompositeAlpha & choice) { alpha=choice; break; }
    }
    const std::uint32_t families[2]{vk.graphicsFamily,vk.presentFamily};
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface=vk.surface; info.minImageCount=count; info.imageFormat=selected.format; info.imageColorSpace=selected.colorSpace;
    info.imageExtent=extent; info.imageArrayLayers=1; info.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode=vk.graphicsFamily==vk.presentFamily ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
    if (vk.graphicsFamily!=vk.presentFamily) { info.queueFamilyIndexCount=2; info.pQueueFamilyIndices=families; }
    info.preTransform=capabilities.currentTransform; info.compositeAlpha=alpha; info.presentMode=VK_PRESENT_MODE_FIFO_KHR; info.clipped=VK_TRUE;
    check(vkCreateSwapchainKHR(vk.device,&info,nullptr,&swapchain),"vkCreateSwapchainKHR");
    check(vkGetSwapchainImagesKHR(vk.device,swapchain,&count,nullptr),"swapchain image count"); swapImages.resize(count);
    check(vkGetSwapchainImagesKHR(vk.device,swapchain,&count,swapImages.data()),"swapchain images"); swapImages.resize(count);
    swapViews.resize(count); presented.resize(count); imageUsed.assign(count,false);
    for (std::size_t i=0;i<count;++i) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=swapImages[i]; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=swapFormat;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; check(vkCreateImageView(vk.device,&view,nullptr,&swapViews[i]),"swapchain image view");
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; check(vkCreateSemaphore(vk.device,&sem,nullptr,&presented[i]),"present semaphore");
    }
    const auto usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    paths0=scene.makeBuffer(pathBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    paths1=scene.makeBuffer(pathBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    hits=scene.makeBuffer(hitBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    counters=scene.makeBuffer(32,usage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    radiance=scene.makeBuffer(radianceBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    shadows=scene.makeBuffer(shadowBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    reservoirs=scene.makeBuffer(reservoirBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    guides=scene.makeBuffer(guideBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    cameras=scene.makeBuffer(sizeof(CameraPair)*slots.size(),usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    signalFlags=scene.makeBuffer(flagBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    histories=scene.makeBuffer(historyBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    filters=scene.makeBuffer(filterBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    opticalPaths=scene.makeBuffer(opticalPathBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    opticalGuides0=scene.makeBuffer(opticalGuideBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    opticalGuides1=scene.makeBuffer(opticalGuideBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    opticalMetadata=scene.makeBuffer(opticalMetaBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    opticalHistories=scene.makeBuffer(opticalHistoryBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    opticalFilters=scene.makeBuffer(opticalFilterBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    opticalChains=scene.makeBuffer(opticalChainBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    prefixRadiance=scene.makeBuffer(pixels*16,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    prefixOptical=scene.makeBuffer(radianceBytes,usage,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    writeDescriptors(); frameNumber=0; frameSlot=0; historyValid=false; cameraCut=true;
    SDL_SetWindowTitle(vk.window,"VoxelPathTracer — FPS: -- | ms/frame: --");
}
void Renderer::writeDescriptors() {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(descriptor.size),0);
    const auto offset = [&](std::uint32_t binding) {
        VkDeviceSize value=0; vkGetDescriptorSetLayoutBindingOffsetEXT(vk.device,descriptorLayout,binding,&value);
        return static_cast<std::size_t>(descriptorOffset+value);
    };
    VkAccelerationStructureDeviceAddressInfoKHR as{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR}; as.accelerationStructure=scene.top;
    VkDescriptorGetInfoEXT info{VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT}; info.type=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    info.data.accelerationStructure=vkGetAccelerationStructureDeviceAddressKHR(vk.device,&as);
    vkGetDescriptorEXT(vk.device,&info,vk.descriptorProperties.accelerationStructureDescriptorSize,bytes.data()+offset(0));
    const std::array<std::pair<std::uint32_t,const Buffer*>,25> buffers{{
        {1,&scene.vertices},{2,&scene.triangleInfo},{3,&scene.materials},{6,&paths0},{7,&hits},{8,&counters},{9,&radiance},{10,&paths1},{11,&scene.lights},{12,&shadows},{13,&reservoirs},{14,&guides},{15,&cameras},{16,&signalFlags},{17,&histories},{18,&filters},{19,&opticalPaths},{20,&opticalGuides0},{21,&opticalGuides1},
        {22,&opticalMetadata},{23,&opticalHistories},{24,&opticalFilters},{25,&opticalChains},{26,&prefixRadiance},{27,&prefixOptical}}};
    for (const auto& [binding,buffer]:buffers) {
        VkDescriptorAddressInfoEXT address{VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT}; address.address=buffer->address; address.range=buffer->size; address.format=VK_FORMAT_UNDEFINED;
        info={VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT}; info.type=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; info.data.pStorageBuffer=&address;
        vkGetDescriptorEXT(vk.device,&info,vk.descriptorProperties.storageBufferDescriptorSize,bytes.data()+offset(binding));
    }
    const std::array<VkImageView,2> views{scene.baseColorTexture.view,scene.roughnessTexture.view};
    for (std::uint32_t i=0;i<2;++i) {
        VkDescriptorImageInfo image{}; image.sampler=scene.sampler; image.imageView=views[i]; image.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info={VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT}; info.type=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; info.data.pCombinedImageSampler=&image;
        vkGetDescriptorEXT(vk.device,&info,vk.descriptorProperties.combinedImageSamplerDescriptorSize,bytes.data()+offset(4+i));
    }
    scene.upload(descriptor,bytes.data(),descriptor.size);
}
Renderer::Push Renderer::camera() const {
    Push p{}; p.position=position; p.tanHalfFov=std::tan(std::numbers::pi_v<float>/6.0f);
    p.forward=normalize(Vec3{std::cos(pitch)*std::cos(yaw),std::sin(pitch),std::cos(pitch)*std::sin(yaw)});
    p.right=normalize(cross(p.forward,Vec3{0,1,0})); p.up=normalize(cross(p.right,p.forward));
    p.width=extent.width; p.height=extent.height; p.frame=frameNumber;
    p.maxTraceWidth=static_cast<std::uint32_t>(std::min<std::uint64_t>(1024,
        static_cast<std::uint64_t>(vk.properties.limits.maxComputeWorkGroupCount[0])*vk.properties.limits.maxComputeWorkGroupSize[0]));
    p.maxGroupWidth=vk.properties.limits.maxComputeWorkGroupCount[0]; p.lightCount=scene.lightCount; p.lightPower=scene.lightPower; p.cameraSlot=frameSlot; return p;
}
void Renderer::record(VkCommandBuffer cmd, std::uint32_t imageIndex, const Push& cameraPush) {
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd,&begin),"vkBeginCommandBuffer");
    allMemory(cmd);
    VkDescriptorBufferBindingInfoEXT binding{VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT};
    binding.address=descriptor.address+descriptorOffset;
    binding.usage=VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
    vkCmdBindDescriptorBuffersEXT(cmd,1,&binding);
    const std::uint32_t bufferIndex=0; const VkDeviceSize offset=0;
    for (auto point : {VK_PIPELINE_BIND_POINT_COMPUTE,VK_PIPELINE_BIND_POINT_GRAPHICS,VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR})
        vkCmdSetDescriptorBufferOffsetsEXT(cmd,point,pipelineLayout,0,1,&bufferIndex,&offset);
    const VkShaderStageFlagBits computeStage=VK_SHADER_STAGE_COMPUTE_BIT;
    vkCmdBindShadersEXT(cmd,1,&computeStage,&primary);
    Push push=cameraPush; push.inputQueue=0;
    vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push);
    const std::uint32_t groups=(extent.width*extent.height+63)/64;
    const auto groupX=std::min(groups,push.maxGroupWidth), groupY=(groups+groupX-1)/groupX;
    vkCmdDispatch(cmd,groupX,groupY,1); allMemory(cmd);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,rayPipeline);
    for (std::uint32_t bounce=0;bounce<=8;++bounce) {
        push.inputQueue=bounce%2; push.mode=0;
        vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push);
        if (bounce==0) vkCmdTraceRaysKHR(cmd,&raygenRegion,&missRegion,&hitRegion,&callableRegion,extent.width,extent.height,1);
        else vkCmdTraceRaysIndirectKHR(cmd,&raygenRegion,&missRegion,&hitRegion,&callableRegion,counters.address+8);
        allMemory(cmd);
        if (bounce==0) {
            vkCmdBindShadersEXT(cmd,1,&computeStage,&restirInit); vkCmdDispatch(cmd,groupX,groupY,1); allMemory(cmd);
            vkCmdBindShadersEXT(cmd,1,&computeStage,&restirTemporal); vkCmdDispatch(cmd,groupX,groupY,1); allMemory(cmd);
            vkCmdBindShadersEXT(cmd,1,&computeStage,&restirSpatial); vkCmdDispatch(cmd,groupX,groupY,1); allMemory(cmd);
            push.mode=1; vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push);
            vkCmdTraceRaysKHR(cmd,&raygenRegion,&missRegion,&hitRegion,&callableRegion,extent.width,extent.height,1); allMemory(cmd);
            vkCmdBindShadersEXT(cmd,1,&computeStage,&shadowResolve); vkCmdDispatch(cmd,groupX,groupY,1); allMemory(cmd);
            push.mode=0; vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push);
        }
        vkCmdFillBuffer(cmd,counters.buffer,(1u-push.inputQueue)*sizeof(std::uint32_t),sizeof(std::uint32_t),0);
        allMemory(cmd);
        vkCmdBindShadersEXT(cmd,1,&computeStage,&scatter);
        if (bounce==0) vkCmdDispatch(cmd,groupX,groupY,1); else vkCmdDispatchIndirect(cmd,counters.buffer,20);
        allMemory(cmd);
        if (bounce>0 && bounce<8) {
            push.mode=1; vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push);
            vkCmdTraceRaysIndirectKHR(cmd,&raygenRegion,&missRegion,&hitRegion,&callableRegion,counters.address+8);
            allMemory(cmd);
            vkCmdBindShadersEXT(cmd,1,&computeStage,&shadowResolve); vkCmdDispatchIndirect(cmd,counters.buffer,20);
            allMemory(cmd);
        }
        if (bounce<8) {
            vkCmdBindShadersEXT(cmd,1,&computeStage,&prepare); push.inputQueue=1u-push.inputQueue;
            vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push); vkCmdDispatch(cmd,1,1,1); allMemory(cmd);
        }
    }
    vkCmdBindShadersEXT(cmd,1,&computeStage,&svgfTemporal); vkCmdDispatch(cmd,groupX,groupY,1); allMemory(cmd);
    vkCmdBindShadersEXT(cmd,1,&computeStage,&svgfVariance); vkCmdDispatch(cmd,groupX,groupY,1); allMemory(cmd);
    vkCmdBindShadersEXT(cmd,1,&computeStage,&svgfAtrous);
    for (std::uint32_t iteration=0;iteration<5;++iteration) {
        push.mode=iteration; push.inputQueue=iteration%2;
        vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push);
        vkCmdDispatch(cmd,groupX,groupY,1); allMemory(cmd);
    }
    const std::uint32_t opticalGroups=(2*extent.width*extent.height+63)/64;
    const auto opticalX=std::min(opticalGroups,push.maxGroupWidth), opticalY=(opticalGroups+opticalX-1)/opticalX;
    vkCmdBindShadersEXT(cmd,1,&computeStage,&opticalTemporal); vkCmdDispatch(cmd,opticalX,opticalY,1); allMemory(cmd);
    vkCmdBindShadersEXT(cmd,1,&computeStage,&opticalVariance); vkCmdDispatch(cmd,opticalX,opticalY,1); allMemory(cmd);
    vkCmdBindShadersEXT(cmd,1,&computeStage,&opticalAtrous);
    for (std::uint32_t iteration=0;iteration<5;++iteration) {
        push.mode=iteration; push.inputQueue=iteration%2;
        vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push);
        vkCmdDispatch(cmd,opticalX,opticalY,1); allMemory(cmd);
    }
    VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image.srcStageMask=VK_PIPELINE_STAGE_2_NONE; image.dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    image.dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    image.oldLayout=imageUsed[imageIndex] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    image.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    image.image=swapImages[imageIndex]; image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo transition{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; transition.imageMemoryBarrierCount=1; transition.pImageMemoryBarriers=&image;
    vkCmdPipelineBarrier2(cmd,&transition);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; color.imageView=swapViews[imageIndex];
    color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; color.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent=extent; rendering.layerCount=1;
    rendering.colorAttachmentCount=1; rendering.pColorAttachments=&color; vkCmdBeginRendering(cmd,&rendering);
    const VkShaderStageFlagBits stages[2]{VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_FRAGMENT_BIT}; const VkShaderEXT shaders[2]{vertex,fragment};
    vkCmdBindShadersEXT(cmd,2,stages,shaders); push=cameraPush;
    vkCmdPushConstants(cmd,pipelineLayout,VK_SHADER_STAGE_ALL,0,sizeof(push),&push);
    const VkViewport viewport{0,0,static_cast<float>(extent.width),static_cast<float>(extent.height),0,1};
    const VkRect2D scissor{{0,0},extent};
    vkCmdSetViewportWithCount(cmd,1,&viewport); vkCmdSetScissorWithCount(cmd,1,&scissor);
    vkCmdSetVertexInputEXT(cmd,0,nullptr,0,nullptr); vkCmdSetPrimitiveTopology(cmd,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cmd,VK_FALSE); vkCmdSetRasterizerDiscardEnable(cmd,VK_FALSE);
    vkCmdSetPolygonModeEXT(cmd,VK_POLYGON_MODE_FILL); vkCmdSetCullMode(cmd,VK_CULL_MODE_NONE); vkCmdSetFrontFace(cmd,VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthClampEnableEXT(cmd,VK_FALSE); vkCmdSetDepthBiasEnable(cmd,VK_FALSE); vkCmdSetLineWidth(cmd,1.0f);
    vkCmdSetDepthTestEnable(cmd,VK_FALSE); vkCmdSetDepthWriteEnable(cmd,VK_FALSE); vkCmdSetDepthCompareOp(cmd,VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBoundsTestEnable(cmd,VK_FALSE); vkCmdSetStencilTestEnable(cmd,VK_FALSE);
    vkCmdSetRasterizationSamplesEXT(cmd,VK_SAMPLE_COUNT_1_BIT); const VkSampleMask mask=~0u; vkCmdSetSampleMaskEXT(cmd,VK_SAMPLE_COUNT_1_BIT,&mask);
    vkCmdSetAlphaToCoverageEnableEXT(cmd,VK_FALSE); vkCmdSetLogicOpEnableEXT(cmd,VK_FALSE);
    const VkBool32 blend=VK_FALSE; vkCmdSetColorBlendEnableEXT(cmd,0,1,&blend);
    const VkColorBlendEquationEXT equation{VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ZERO,VK_BLEND_OP_ADD,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ZERO,VK_BLEND_OP_ADD};
    vkCmdSetColorBlendEquationEXT(cmd,0,1,&equation);
    const VkColorComponentFlags components=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    vkCmdSetColorWriteMaskEXT(cmd,0,1,&components); vkCmdDraw(cmd,3,1,0,0); vkCmdEndRendering(cmd);
    image.srcStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; image.srcAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    image.dstStageMask=VK_PIPELINE_STAGE_2_NONE; image.dstAccessMask=0;
    image.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; image.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd,&transition);
    check(vkEndCommandBuffer(cmd),"vkEndCommandBuffer");
}
bool Renderer::render(const Push& push) {
    auto& frame=slots[frameSlot];
    check(vkWaitForFences(vk.device,1,&frame.fence,VK_TRUE,std::numeric_limits<std::uint64_t>::max()),"frame fence wait");
    if (vk.validationFailed.load()) throw std::runtime_error("Vulkan validation reported warning or error");
    std::uint32_t imageIndex=0;
    const auto acquire=vkAcquireNextImageKHR(vk.device,swapchain,std::numeric_limits<std::uint64_t>::max(),frame.acquired,VK_NULL_HANDLE,&imageIndex);
    if (acquire==VK_ERROR_OUT_OF_DATE_KHR) return false;
    if (acquire!=VK_SUCCESS && acquire!=VK_SUBOPTIMAL_KHR) check(acquire,"vkAcquireNextImageKHR");
    CameraPair cameraPair{};
    cameraPair.current={push.position,push.tanHalfFov,push.forward,push.width,push.right,push.height,push.up,push.frame};
    cameraPair.previous=historyValid ? previousCamera : cameraPair.current;
    cameraPair.valid=static_cast<std::uint32_t>(historyValid && !cameraCut);
    void* mapped=nullptr;
    check(vkMapMemory(vk.device,cameras.memory,static_cast<VkDeviceSize>(frameSlot)*sizeof(CameraPair),sizeof(CameraPair),0,&mapped),"map camera frame");
    std::memcpy(mapped,&cameraPair,sizeof(cameraPair)); vkUnmapMemory(vk.device,cameras.memory);
    check(vkResetFences(vk.device,1,&frame.fence),"vkResetFences");
    check(vkResetCommandPool(vk.device,frame.pool,0),"vkResetCommandPool");
    record(frame.command,imageIndex,push);
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; wait.semaphore=frame.acquired; wait.stageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; signal.semaphore=presented[imageIndex]; signal.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer=frame.command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.waitSemaphoreInfoCount=1; submit.pWaitSemaphoreInfos=&wait;
    submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command; submit.signalSemaphoreInfoCount=1; submit.pSignalSemaphoreInfos=&signal;
    check(vkQueueSubmit2(vk.graphics,1,&submit,frame.fence),"vkQueueSubmit2");
    previousCamera=cameraPair.current; historyValid=true; cameraCut=false;
    imageUsed[imageIndex]=true; frameNumber++; frameSlot=(frameSlot+1)%static_cast<std::uint32_t>(slots.size());
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.waitSemaphoreCount=1; present.pWaitSemaphores=&presented[imageIndex];
    present.swapchainCount=1; present.pSwapchains=&swapchain; present.pImageIndices=&imageIndex;
    const auto result=vkQueuePresentKHR(vk.present,&present);
    if (result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_SUBOPTIMAL_KHR || acquire==VK_SUBOPTIMAL_KHR) return false;
    check(result,"vkQueuePresentKHR"); return true;
}
void Renderer::run() {
    using Clock=std::chrono::steady_clock;
    auto last=Clock::now(), sampleStart=last;
    std::uint64_t presentedFrames=0, totalPresentedFrames=0;
    bool running=true, focused=true, minimized=false, dirty=!swapchain, paused=false;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type==SDL_EVENT_QUIT || event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED) running=false;
            else if (event.type==SDL_EVENT_WINDOW_MINIMIZED) {
                minimized=true; dirty=true; presentedFrames=0; SDL_SetWindowTitle(vk.window,"VoxelPathTracer — FPS: -- | ms/frame: --");
            } else if (event.type==SDL_EVENT_WINDOW_RESTORED) { minimized=false; dirty=true; last=Clock::now(); }
            else if (event.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) dirty=true;
            else if (event.type==SDL_EVENT_WINDOW_FOCUS_LOST) {
                focused=false; if (captured) { SDL_SetWindowRelativeMouseMode(vk.window,false); captured=false; }
            } else if (event.type==SDL_EVENT_WINDOW_FOCUS_GAINED) { focused=true; last=Clock::now(); }
            else if (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_RIGHT && focused) {
                if (!SDL_SetWindowRelativeMouseMode(vk.window,true)) throw std::runtime_error(std::string("capture mouse: ")+SDL_GetError());
                captured=true;
            } else if (event.type==SDL_EVENT_MOUSE_BUTTON_UP && event.button.button==SDL_BUTTON_RIGHT && captured) {
                SDL_SetWindowRelativeMouseMode(vk.window,false); captured=false;
            } else if (event.type==SDL_EVENT_MOUSE_MOTION && captured) {
                yaw+=event.motion.xrel*0.0025f; pitch=std::clamp(pitch-event.motion.yrel*0.0025f,
                    -89.0f*std::numbers::pi_v<float>/180.0f,89.0f*std::numbers::pi_v<float>/180.0f);
            } else if (event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                if (event.key.scancode==SDL_SCANCODE_ESCAPE) {
                    if (captured) { SDL_SetWindowRelativeMouseMode(vk.window,false); captured=false; } else running=false;
                } else if (event.key.scancode==SDL_SCANCODE_R) {
                    position={8.0f,2.5f,20.0f}; yaw=-std::numbers::pi_v<float>/2.0f; pitch=-0.0356991127f; cameraCut=true;
                }
            }
        }
        if (!running) break;
        int width=0,height=0;
        if (!SDL_GetWindowSizeInPixels(vk.window,&width,&height)) throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels: ")+SDL_GetError());
        if (minimized || width<=0 || height<=0) {
            if (!paused) { SDL_SetWindowTitle(vk.window,"VoxelPathTracer — FPS: -- | ms/frame: --"); paused=true; }
            SDL_Delay(16); last=Clock::now(); sampleStart=last; presentedFrames=0; continue;
        }
        if (paused) { paused=false; dirty=true; last=Clock::now(); }
        if (dirty) {
            recreate(); dirty=false; last=Clock::now(); sampleStart=last; presentedFrames=0;
            if (!swapchain) { SDL_Delay(16); dirty=true; continue; }
        }
        const auto now=Clock::now(); const float dt=std::chrono::duration<float>(now-last).count(); last=now;
        if (focused) {
            const bool* keys=SDL_GetKeyboardState(nullptr);
            const float x=static_cast<float>(keys[SDL_SCANCODE_D])-static_cast<float>(keys[SDL_SCANCODE_A]);
            const float z=static_cast<float>(keys[SDL_SCANCODE_W])-static_cast<float>(keys[SDL_SCANCODE_S]);
            const float y=static_cast<float>(keys[SDL_SCANCODE_E])-static_cast<float>(keys[SDL_SCANCODE_Q]);
            Vec3 forward{std::cos(yaw),0,std::sin(yaw)}, right{-std::sin(yaw),0,std::cos(yaw)};
            Vec3 movement=add(add(scale(forward,z),scale(right,x)),Vec3{0,y,0});
            const float magnitude=std::sqrt(movement.x*movement.x+movement.y*movement.y+movement.z*movement.z);
            if (magnitude>0) {
                const float speed=(keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 9.0f : 3.0f;
                position=add(position,scale(movement,speed*dt/std::max(1.0f,magnitude)));
            }
        }
        if (!render(camera())) { dirty=true; continue; }
        ++presentedFrames; ++totalPresentedFrames;
        const auto after=Clock::now(); const double elapsed=std::chrono::duration<double>(after-sampleStart).count();
        if (elapsed>=0.5 && presentedFrames) {
            std::ostringstream title; title << "VoxelPathTracer — FPS: " << std::fixed << std::setprecision(1)
                << static_cast<double>(presentedFrames)/elapsed << " | ms/frame: " << elapsed*1000.0/static_cast<double>(presentedFrames);
            SDL_SetWindowTitle(vk.window,title.str().c_str()); sampleStart=after; presentedFrames=0;
        }
    }
    if (captured) { SDL_SetWindowRelativeMouseMode(vk.window,false); captured=false; }
    std::cout << "Presented frames: " << totalPresentedFrames << "\n";
    check(vkDeviceWaitIdle(vk.device),"device idle on shutdown");
    if (vk.validationFailed.load()) throw std::runtime_error("Vulkan validation reported warning or error");
}
Renderer::~Renderer() {
    if (!vk.device) return;
    vkDeviceWaitIdle(vk.device);
    destroySwapchain();
    for (auto& frame:slots) {
        if (frame.fence) vkDestroyFence(vk.device,frame.fence,nullptr);
        if (frame.acquired) vkDestroySemaphore(vk.device,frame.acquired,nullptr);
        if (frame.pool) vkDestroyCommandPool(vk.device,frame.pool,nullptr);
    }
    scene.destroyBuffer(descriptor); scene.destroyBuffer(sbt);
    if (rayPipeline) vkDestroyPipeline(vk.device,rayPipeline,nullptr);
    for (auto shader : {primary,scatter,prepare,shadowResolve,restirInit,restirTemporal,restirSpatial,svgfTemporal,svgfVariance,svgfAtrous,opticalTemporal,opticalVariance,opticalAtrous,vertex,fragment}) if (shader) vkDestroyShaderEXT(vk.device,shader,nullptr);
    if (pipelineLayout) vkDestroyPipelineLayout(vk.device,pipelineLayout,nullptr);
    if (descriptorLayout) vkDestroyDescriptorSetLayout(vk.device,descriptorLayout,nullptr);
}
}
