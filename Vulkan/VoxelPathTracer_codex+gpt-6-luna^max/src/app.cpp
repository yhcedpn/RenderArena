#include "app.hpp"
#include "scene.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace voxel {

int runApplication()
{
    const char* basePath = SDL_GetBasePath();
    if (!basePath) {
        std::cerr << "无法取得程序目录: " << SDL_GetError() << '\n';
        return 1;
    }
    const std::filesystem::path executableDirectory(basePath);
    SDL_free(const_cast<char*>(basePath));

    Scene scene;
    try {
        scene = loadScene(executableDirectory / "materials.json");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "场景资源已解析：" << scene.materials.size() << " 种材质，"
              << scene.vertices.size() << " 个顶点，" << scene.triangles.size()
              << " 个三角形，" << scene.lights.size() << " 个发光三角形。\n";
    std::cerr << "当前交付仅包含材质、纹理与静态网格生成；Vulkan 光线追踪、窗口渲染及交互链尚未实现。\n";
    return 2;
}

} // namespace voxel
