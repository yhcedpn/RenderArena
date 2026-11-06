#include "src/scene.hpp"
#include "src/gpu_scene.hpp"
#include "src/renderer.hpp"
#include "src/vulkan.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main()
{
    try {
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
        struct Quit { ~Quit() { SDL_Quit(); } } quit;
        const char* base = SDL_GetBasePath();
        if (!base) throw std::runtime_error(std::string("SDL_GetBasePath: ") + SDL_GetError());
        const auto basePath = std::filesystem::u8path(base);
        const auto scene = arena::loadScene(basePath / "materials.json");
        std::cout << "Scene: " << scene.materials.size() << " materials, " << scene.triangles.size()
                  << " triangles, " << scene.lights.size() << " emissive triangles\n";
        arena::VulkanContext vulkan;
        vulkan.initialize();
        {
            arena::GpuScene gpuScene(vulkan);
            gpuScene.initialize(scene);
            std::cout << "Built and compacted " << gpuScene.bottom.size() << " BLAS and one TLAS\n";
            arena::Renderer renderer(vulkan, gpuScene, basePath);
            renderer.initialize();
            renderer.run();
        }
        vulkan.shutdown();
        if (vulkan.validationFailed.load()) throw std::runtime_error("Vulkan validation reported warning or error");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "VoxelPathTracer: " << e.what() << '\n';
        return 1;
    }
}
