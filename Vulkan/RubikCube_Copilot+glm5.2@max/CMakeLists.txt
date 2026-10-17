cmake_minimum_required(VERSION 3.28)

include(../../cmake/Vcpkg.cmake)
project(RubikCube LANGUAGES CXX)

include(../../cmake/RenderArena.cmake)


# 依赖统一使用 vcpkg 导出的目标，保持 VS 与命令行的链接契约一致。

# Vulkan headers：函数由 volk 从系统驱动动态加载
find_package(VulkanHeaders 1.4 CONFIG REQUIRED)

# volk：函数加载（不链接 libvulkan 与 volk 符号冲突由 config 处理）
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
find_package(volk REQUIRED CONFIG)

# GLFW：窗口与 surface
find_package(glfw3 CONFIG REQUIRED)

# VMA（header-only + 官方 cmake config）
find_package(VulkanMemoryAllocator REQUIRED CONFIG)

# shaderc：运行时 GLSL -> SPIR-V 编译
find_package(unofficial-shaderc CONFIG REQUIRED)
find_package(Threads REQUIRED)

# nlohmann-json：配置读取
find_package(nlohmann_json REQUIRED CONFIG)

# ---- 目标 ---------------------------------------------------------------

add_executable(rubik_cube
    src/main.cpp
    src/config.cpp
    src/texture_gen.cpp
    src/scene.cpp
    src/vulkan_engine.cpp
)

target_include_directories(rubik_cube PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(rubik_cube PRIVATE
    Vulkan::Headers
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    nlohmann_json::nlohmann_json
    glfw
    unofficial::shaderc::shaderc
    Threads::Threads
)

# volk 通过 dlopen 加载 libvulkan，不链接 libvulkan；链接 dl
target_link_libraries(rubik_cube PRIVATE ${CMAKE_DL_LIBS})

if(MSVC)
    target_compile_options(rubik_cube PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(rubik_cube PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 运行时需要 materials.json 与二进制同目录
add_custom_command(TARGET rubik_cube POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/materials.json
        $<TARGET_FILE_DIR:rubik_cube>/materials.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms.json
        $<TARGET_FILE_DIR:rubik_cube>/platforms.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json
        $<TARGET_FILE_DIR:rubik_cube>/vcpkg.json
    # 着色器目录也拷贝一份，便于从 build 目录直接运行（代码内亦回退到 ../shaders）
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:rubik_cube>/shaders
    VERBATIM)
