cmake_minimum_required(VERSION 3.28)

include(../../cmake/Vcpkg.cmake)
project(VoxelPBRFrustumCulling LANGUAGES CXX)

include(../../cmake/RenderArena.cmake)

if(MSVC)
    # 源码含简体中文 UTF-8 字符串字面量，统一按 UTF-8 解释。
    add_compile_options(/utf-8)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(VoxelPBRFrustumCulling main.cpp)
target_link_libraries(VoxelPBRFrustumCulling PRIVATE glfw glad::glad glm::glm nlohmann_json::nlohmann_json)

add_custom_command(TARGET VoxelPBRFrustumCulling POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/materials.json"
        "$<TARGET_FILE_DIR:VoxelPBRFrustumCulling>/materials.json"
    VERBATIM)
