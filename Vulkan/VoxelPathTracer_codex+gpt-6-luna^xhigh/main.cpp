#include "src/scene.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        const char* executableDirectory = SDL_GetBasePath();
        if (executableDirectory == nullptr)
            throw std::runtime_error("SDL_GetBasePath 失败，无法定位运行时资源目录");
        const std::filesystem::path materialPath = std::filesystem::path(executableDirectory) / "materials.json";

        const auto materials = voxelpt::loadMaterials(materialPath);
        const auto scene = voxelpt::buildScene(materials);
        std::size_t opaqueTriangles = 0;
        for (const auto& mesh : scene.opaqueMeshes)
            opaqueTriangles += mesh.indices.size() / 3U;
        const std::size_t glassTriangles = scene.glassMesh.indices.size() / 3U;

        std::cout << "材质配置有效：" << materials.materials.size() << " 种材质，"
                  << materials.textures.size() << " 组程序化纹理 mip。\n"
                  << "场景几何已提取：" << scene.opaqueMeshes.size() << " 个材质分组，"
                  << opaqueTriangles << " 个不透明三角形，玻璃 " << glassTriangles << " 个三角形，"
                  << scene.exposedLightTriangles << " 个发光三角形。\n";
        std::cerr << "GPU 光追、wavefront、ReSTIR DI、SVGF 与窗口呈现尚未实现；当前答卷不能启动完整展厅。\n";
        return EXIT_FAILURE;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
