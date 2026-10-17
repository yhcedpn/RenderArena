// Vulkan 渲染器声明：硬件 RT、wavefront、ReSTIR DI、SVGF 与显示。
#pragma once

#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>
#include <volk.h>

#include <shaderc/shaderc.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "rae_common.hpp"
#include "rae_scene.hpp"

namespace rae {

inline std::atomic<bool> g_validationFailure{false};

// shader 绑定号约定。scene.glsl 使用 0..6；各 pass 的私有资源从 10 开始。
namespace bindings {
constexpr uint32_t kGlobals = 0;
constexpr uint32_t kMaterials = 1;
constexpr uint32_t kTriangles = 2;
constexpr uint32_t kInstanceGroups = 3;
constexpr uint32_t kLights = 4;
constexpr uint32_t kBaseTex = 5;
constexpr uint32_t kRoughTex = 6;
constexpr uint32_t kA = 10;
constexpr uint32_t kB = 11;
constexpr uint32_t kC = 12;
constexpr uint32_t kD = 13;
constexpr uint32_t kE = 14;
constexpr uint32_t kF = 15;
constexpr uint32_t kG = 16;
constexpr uint32_t kH = 17;
constexpr uint32_t kI = 18;
constexpr uint32_t kJ = 19;
} // namespace bindings

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;
    VkDeviceAddress address = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{0, 0};
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t mipLevels = 1;
    uint32_t layers = 1;
};

struct Accel {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer buffer;
};

struct Pass {
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderEXT shader = VK_NULL_HANDLE;
    VkDeviceSize setOffset = 0;
};

struct SbtRegions {
    VkStridedDeviceAddressRegionKHR raygen{};
    VkStridedDeviceAddressRegionKHR miss{};
    VkStridedDeviceAddressRegionKHR hit{};
    VkStridedDeviceAddressRegionKHR callable{};
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
};

class Renderer {
public:
    int run();

private:
    // 初始化/销毁。
    bool init();
    void shutdown();
    void destroySwapchain();
    void destroyRenderResources();
    void destroyStaticResources();
    bool initWindow();
    bool initVulkan();
    bool createInstance();
    bool pickPhysicalDevice();
    bool createDevice();
    bool createAllocator();
    bool createSwapchain();
    bool createSwapchainViews();
    bool createStaticResources();
    bool buildAccelerationStructures();
    bool compileShaders();
    bool createPasses();
    bool createRayTracingPipeline();
    bool createRenderResources();
    bool createDescriptorBuffer();
    void writeDescriptors();
    bool createFrameResources();
    void createFrameObjects(Frame& frame);
    void destroyFrameObjects(Frame& frame);

    // 每帧。
    void runLoop();
    void updateCamera(double dt);
    void drawFrame();
    void recordCommandBuffer(VkCommandBuffer cmd, VkImage swapImage, VkImageView swapView);
    void updateGlobals();
    void submitAndPresent(uint32_t imageIndex);
    void handleResize(int width, int height);
    bool acquireImage(uint32_t& imageIndex, VkResult& result);
    void updateTitle(bool placeholder);
    void resetStats();

    // 输入回调。
    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos);
    static void focusCallback(GLFWwindow* window, int focused);
    void releaseMouse();
    void captureMouse();
    void resetCamera();
    void onKey(int key, int action);

    // 资源。
    bool createTempResources();
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                        VkDeviceSize alignment = 0, bool preferDevice = true);
    void destroyBuffer(Buffer& buffer);
    Image createImage2D(VkFormat format, VkExtent2D extent, uint32_t mipLevels, uint32_t layers,
                        VkImageUsageFlags usage, VkImageAspectFlags aspect,
                        VkImageViewType viewType);
    void destroyImage(Image& image);
    void uploadBuffer(Buffer& dst, const void* data, VkDeviceSize size);
    void destroyAccel(Accel& accel);
    VkCommandBuffer beginTempCommand();
    void endTempCommand(VkCommandBuffer cmd, bool wait);
    void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                         VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                         VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                         VkAccessFlags2 dstAccess, uint32_t mipLevels, uint32_t layers);
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                 VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
    VkShaderModule createShaderModule(const std::vector<uint32_t>& spv);
    std::vector<uint32_t> compileShader(const std::string& file, shaderc_shader_kind stage);

    // pass 辅助。
    VkDescriptorSetLayout createSetLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings);
    void createComputePass(const char* file, const std::vector<VkDescriptorSetLayoutBinding>& bindings,
                           Pass& pass);
    void createGraphicsPass();
    void createRTPassLayout();
    void computeDescriptorOffsets();
    void bindPass(VkCommandBuffer cmd, const Pass& pass, VkPipelineBindPoint bindPoint,
                  const PushConstants& pc, bool push);
    void dispatchCompute(VkCommandBuffer cmd, const Pass& pass, const PushConstants& pc,
                         uint32_t count);
    void dispatchComputeIndirect(VkCommandBuffer cmd, const Pass& pass, const PushConstants& pc,
                                 VkDeviceSize offset);
    void traceRaysDirect(VkCommandBuffer cmd, const PushConstants& pc, uint32_t width, uint32_t height,
                         uint32_t depth, uint32_t raygenIndex);
    void traceRaysIndirect(VkCommandBuffer cmd, const PushConstants& pc,
                           VkDeviceAddress indirectAddress, uint32_t raygenIndex);
    void drawFullscreen(VkCommandBuffer cmd, VkImage swapImage, VkImageView swapView,
                        const PushConstants& pc);
    void setDisplayDynamicState(VkCommandBuffer cmd);

    // 纹理/采样器。
    bool uploadTextures();
    void destroyTextureResources();

    // 统计/标题。
    void beginStatsFrame();

    GLFWwindow* window_ = nullptr;
    bool initialized_ = false;
    bool fatalError_ = false;

    // 实例/设备。
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties deviceProps_{};
    VkPhysicalDeviceLimits limits_{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps_{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps_{};
    VkPhysicalDeviceDescriptorBufferPropertiesEXT dbProps_{};
    uint32_t maxGroupsX_ = 1;
    uint32_t maxGroupsY_ = 1;
    uint32_t maxGroupsZ_ = 1;

    // swapchain。
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR swapchainColorSpace_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{0, 0};
    std::vector<VkImage> swapImages_;
    std::vector<VkImageView> swapViews_;
    std::vector<VkSemaphore> renderFinished_;
    bool swapchainSuboptimal_ = false;

    // 帧。
    static constexpr uint32_t kFramesInFlight = 2;
    std::array<Frame, kFramesInFlight> frames_{};
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t timelineValue_ = 0;
    uint32_t frameCounter_ = 0; // 渲染帧序号，初始化后从 1 开始；slot = frameCounter & 1
    uint32_t frameSlot_ = 0;
    VkCommandPool tempPool_ = VK_NULL_HANDLE;
    VkFence tempFence_ = VK_NULL_HANDLE;

    // 场景。
    SceneData scene_;
    std::filesystem::path exeDir_;
    std::filesystem::path shadersDir_;
    Image baseTex_{};
    Image roughTex_{};
    VkSampler baseSampler_ = VK_NULL_HANDLE;
    std::vector<Buffer> groupVertexBuffers_;
    std::vector<Buffer> groupIndexBuffers_;
    std::vector<uint32_t> groupVertexCounts_;
    std::vector<uint32_t> groupIndexCounts_;
    Buffer triangleBuffer_{};
    Buffer instanceGroupBuffer_{};
    Buffer materialBuffer_{};
    Buffer lightBuffer_{};
    Buffer countsBuffer_{};
    Buffer tlasInstanceBuffer_{};
    std::vector<Accel> blasAccels_;
    Accel tlas_{};
    Buffer sbtBuffer_{};
    SbtRegions sbt_{};

    // shader/pipeline。
    VkPipeline rtPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout rtLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout rtSetLayout_ = VK_NULL_HANDLE;
    VkDeviceSize rtSetOffset_ = 0;
    Pass passPrimary_{};
    Pass passPrimaryInfo_{};
    Pass passShade_{};
    Pass passShadowResolve_{};
    Pass passBuildIndirect_{};
    Pass passRestirInit_{};
    Pass passRestirTemporal_{};
    Pass passRestirSpatial_{};
    Pass passRestirShadow_{};
    Pass passSvgfTemporal_{};
    Pass passSvgfAtrous_{};
    VkShaderEXT displayVert_ = VK_NULL_HANDLE;
    VkShaderEXT displayFrag_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout displaySetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout displayLayout_ = VK_NULL_HANDLE;
    VkDeviceSize displaySetOffset_ = 0;
    std::vector<std::pair<VkDescriptorSetLayout, VkDeviceSize*>> layoutRefs_;
    bool descriptorsCreated_ = false;

    // 帧尺寸相关资源。
    Buffer pathBuffer_{};
    Buffer hitBuffer_{};
    Buffer shadowTaskBuffer_{};
    Buffer shadowResultBuffer_{};
    Buffer slotRadianceBuffer_{};
    Buffer slotGuideBuffer_{};
    Buffer slotTagBuffer_{};
    Buffer emissionBuffer_{};
    Buffer primaryInfoBuffer_{};
    Buffer jitterBuffer_{};
    Buffer reservoirInitBuffer_{};
    Buffer reservoirTemporalBuffer_{};
    Buffer reservoirHistoryBuffer_{};
    Buffer historyColorBuffer_{};
    Buffer historyM2Buffer_{};
    Buffer filterColorBuffer_{};
    Buffer filterVarBuffer_{};
    Buffer indirectBuffer_{};
    Buffer globalsBufferForWrite_{};
    Buffer descriptorBuffer_{};
    VkDeviceSize descriptorBufferSize_ = 0;
    VkDeviceSize descriptorBufferBaseOffset_ = 0;
    VkDeviceAddress descriptorBufferBindingAddress_ = 0;
    uint32_t pathCapacity_ = 0;
    uint32_t shadowCapacity_ = 0;
    uint32_t pixelCount_ = 0;

    // 相机/输入。
    Camera camera_{};
    Camera prevCamera_{};
    bool prevCameraValid_ = false;
    Vec3 cameraPos_{8.0f, 2.5f, 20.0f};
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    bool mouseCaptured_ = false;
    bool firstMouse_ = true;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
    bool windowFocused_ = true;
    bool resetInputTime_ = true;
    double lastInputTime_ = 0.0;
    bool historyReset_ = true;
    bool paused_ = false;
    bool forceTitlePlaceholder_ = true;

    // 统计。
    uint64_t statsFrames_ = 0;
    double statsStartTime_ = 0.0;
    double lastPresentTime_ = 0.0;
};

} // namespace rae
