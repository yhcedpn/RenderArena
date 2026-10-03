cmake_minimum_required(VERSION 3.28)

project(RubikCube LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出目录统一，便于运行时找到 materials.json
set(RUBIK_RUNTIME_OUTPUT ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${RUBIK_RUNTIME_OUTPUT})

# 依赖统一使用 vcpkg 导出的目标，保持本地与 CI 的链接契约一致。

# Vulkan headers + loader
find_package(Vulkan REQUIRED)
# 功能代码使用 Vulkan 1.4 核心接口，不能用更旧的 headers 编译。
if(Vulkan_VERSION VERSION_LESS 1.4)
    message(FATAL_ERROR "需要 Vulkan headers >= 1.4，当前 ${Vulkan_VERSION}")
endif()

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
