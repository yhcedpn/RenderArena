cmake_minimum_required(VERSION 3.28)
include("../../cmake/Vcpkg.cmake")
project(VoxelPathTracer LANGUAGES C CXX)
include("../../cmake/RenderArena.cmake")
include("../../cmake/VulkanShaders.cmake")

find_package(volk CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

# 新增编译单元与 shader 随下次配置自动纳入，测评期间无需修改构建逻辑。
file(GLOB_RECURSE arena_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
add_executable(VoxelPathTracer main.cpp ${arena_sources})
target_link_libraries(VoxelPathTracer PRIVATE volk::volk Vulkan::Headers
    nlohmann_json::nlohmann_json SDL3::SDL3 GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(VoxelPathTracer PRIVATE VK_NO_PROTOTYPES
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=0)
arena_add_shaders(VoxelPathTracer)

add_custom_target(VoxelPathTracer_resources
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${ARENA_RUNTIME_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/materials.json" "${ARENA_RUNTIME_DIR}/materials.json"
    VERBATIM)
add_dependencies(VoxelPathTracer VoxelPathTracer_resources)
