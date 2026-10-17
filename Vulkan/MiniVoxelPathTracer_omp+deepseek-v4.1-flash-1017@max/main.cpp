// MiniVoxelPathTracer：Vulkan 1.4 硬件 RT voxel 路径追踪器入口。
#define VK_NO_PROTOTYPES
#include <volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include "rae_common.hpp"
#include "rae_scene.hpp"
#include "rae_renderer.hpp"
#include "rae_renderer_setup.hpp"
#include "rae_renderer_resources.hpp"
#include "rae_renderer_frame.hpp"

#include <cstdlib>
#include <exception>
#include <cstdio>

int main() {
    try {
        rae::Renderer app;
        return app.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "未捕获异常: %s\n", e.what());
        return EXIT_FAILURE;
    }
}
