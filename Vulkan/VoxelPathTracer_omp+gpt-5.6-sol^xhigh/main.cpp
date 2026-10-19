#include "src/renderer.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL3 初始化失败：" << SDL_GetError() << '\n';
        return 1;
    }
    try {
        const char* basePath = SDL_GetBasePath();
        if (basePath == nullptr) throw std::runtime_error(std::string("无法定位程序目录：") + SDL_GetError());
        arena::Application application{std::filesystem::path(basePath)};
        const int result = application.run();
        SDL_Quit();
        return result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VoxelPathTracer", error.what(), nullptr);
        SDL_Quit();
        return 1;
    }
}
