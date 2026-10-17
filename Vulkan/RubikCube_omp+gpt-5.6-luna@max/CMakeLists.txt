cmake_minimum_required(VERSION 3.28)

include(../../cmake/Vcpkg.cmake)
project(RubikCube LANGUAGES CXX)

include(../../cmake/RenderArena.cmake)

find_package(VulkanHeaders 1.4 CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
find_package(volk CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(RubikCube main.cpp shaders.inc)
set_source_files_properties(shaders.inc PROPERTIES HEADER_FILE_ONLY ON)

target_link_libraries(RubikCube PRIVATE
    glfw
    volk::volk
    Vulkan::Headers
    nlohmann_json::nlohmann_json
    ${CMAKE_DL_LIBS}
)

if(MSVC)
    target_compile_options(RubikCube PRIVATE /utf-8)
endif()

add_custom_command(TARGET RubikCube POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/materials.json"
        "$<TARGET_FILE_DIR:RubikCube>/materials.json"
    VERBATIM
)

include(GNUInstallDirs)
install(TARGETS RubikCube RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES materials.json DESTINATION ${CMAKE_INSTALL_BINDIR})
