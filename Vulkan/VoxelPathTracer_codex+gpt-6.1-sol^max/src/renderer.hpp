#pragma once
#include "scene.hpp"
#include <volk.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <functional>
class Renderer {
public:
    Renderer()=default;~Renderer();void run();
private:
    struct Buffer {VkBuffer handle{};VkDeviceMemory memory{};VkDeviceSize size{};VkDeviceAddress address{};void* mapped{};};
    struct Image {VkImage handle{};VkDeviceMemory memory{};VkImageView view{};};
    struct AS {VkAccelerationStructureKHR handle{};Buffer buffer;VkDeviceAddress address{};};
    struct Slot {VkCommandPool pool{};VkCommandBuffer command{};VkSemaphore acquire{};uint64_t completion{};Buffer frame;};
    SDL_Window* window{};VkInstance instance{};VkDebugUtilsMessengerEXT messenger{};VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};uint32_t family{};
    VkPhysicalDeviceProperties properties{};VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT db{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT};
    VkPhysicalDeviceExtendedDynamicState3PropertiesEXT dyn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT};
    VkCommandPool uploadPool{};VkSemaphore timeline{};uint64_t serial{};std::array<Slot,2> slots{};
    VkSwapchainKHR swapchain{};VkExtent2D extent{};VkFormat swapFormat{};
    std::vector<VkImage> swapImages;std::vector<VkImageView> swapViews;std::vector<VkSemaphore> presentReady;std::vector<bool> imageInitialized;
    Scene scene;std::filesystem::path base;Buffer triangles,materials,lights,vertices,descriptor,sbt;
    std::array<Buffer,2> paths,guide,reservoir,temporalHistory,filter;
    Buffer hit,counter,signal,initialReservoir,temporalReservoir,shadow,visibility;
    Image color,rough;VkSampler sampler{};std::vector<AS> bottom;AS top;
    VkDescriptorSetLayout layout{};VkPipelineLayout pipelineLayout{};VkDeviceSize setSize{},setStride{};std::array<VkDeviceSize,24> bindingOffset{};
    VkPipeline rayPipeline{};VkShaderEXT compute{},vertex{},fragment{};
    std::array<VkStridedDeviceAddressRegionKHR,2> rgen{};VkStridedDeviceAddressRegionKHR miss{},hitRegion{},callable{};
    uint32_t traceWidth{},dispatchWidth{};uint64_t rendered{};Camera camera,previousCamera;F4 previousJitter{};bool cameraCut=true;
    std::atomic<bool> validationFailure=false;
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT*,void*);
    void initialize();void createDevice();void createScene();void createShaders();void rebuild();void frame(uint32_t,uint32_t);
    void destroySize();void destroySwapchain();void shutdown();
    Buffer makeBuffer(VkDeviceSize,VkBufferUsageFlags,bool=false,const void* =nullptr,VkDeviceSize=1);
    void destroy(Buffer&);Image makeTexture(VkFormat,bool);void destroy(Image&);AS makeAS(VkDeviceSize,VkAccelerationStructureTypeKHR);void destroy(AS&);
    uint32_t memoryType(uint32_t,VkMemoryPropertyFlags);
    void immediate(const std::function<void(VkCommandBuffer)>&);void wait(uint64_t);
    void barrier(VkCommandBuffer,VkPipelineStageFlags2,VkAccessFlags2,VkPipelineStageFlags2,VkAccessFlags2);
    void computePass(VkCommandBuffer,uint32_t,uint32_t,uint32_t,uint32_t,bool=false);
    void descriptors();std::vector<uint32_t> spirv(const char*);void displayState(VkCommandBuffer);
};
