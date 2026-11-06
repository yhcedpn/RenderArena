#include "src/vulkan_app.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <iostream>

int main() {
    try {
        return arena::runVoxelPathTracer();
    } catch (const std::exception& error) {
        std::cerr << "VoxelPathTracer 启动失败: " << error.what() << '\n';
        if (SDL_WasInit(SDL_INIT_VIDEO) != 0U) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VoxelPathTracer", error.what(), nullptr);
        }
        return 1;
    }
}
