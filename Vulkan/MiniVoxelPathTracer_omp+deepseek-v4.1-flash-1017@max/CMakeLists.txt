cmake_minimum_required(VERSION 3.28)
project(MiniVoxelPathTracer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan 1.4 REQUIRED)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
find_package(volk CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(unofficial-shaderc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SPIRV-Tools CONFIG REQUIRED)
find_package(SPIRV-Tools-opt CONFIG REQUIRED)
find_package(unofficial-spirv-reflect CONFIG REQUIRED)
find_package(VulkanUtilityLibraries CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(MiniVoxelPathTracer main.cpp)
target_link_libraries(MiniVoxelPathTracer PRIVATE
    Vulkan::Headers volk::volk glfw GPUOpen::VulkanMemoryAllocator
    unofficial::shaderc::shaderc nlohmann_json::nlohmann_json
    SPIRV-Tools-static SPIRV-Tools-opt unofficial::spirv-reflect
    Vulkan::SafeStruct Vulkan::LayerSettings Vulkan::UtilityHeaders
    Threads::Threads ${CMAKE_DL_LIBS})
if(MSVC)
    target_compile_options(MiniVoxelPathTracer PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(MiniVoxelPathTracer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_custom_command(TARGET MiniVoxelPathTracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/materials.json"
        "$<TARGET_FILE_DIR:MiniVoxelPathTracer>/materials.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        "$<TARGET_FILE_DIR:MiniVoxelPathTracer>/shaders"
    VERBATIM)
include(GNUInstallDirs)
install(TARGETS MiniVoxelPathTracer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES materials.json DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY shaders DESTINATION ${CMAKE_INSTALL_BINDIR})
