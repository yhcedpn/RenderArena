find_program(ARENA_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)

function(arena_add_shaders target)
    # 每轮先配置再构建；避免 VS 的 VerifyGlobs 命令未引用含 ^ 的路径。
    file(GLOB_RECURSE arena_shader_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.rgen"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.rmiss" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.rchit"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.rahit" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.rint"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.rcall")
    set(arena_shader_outputs)
    foreach(arena_shader IN LISTS arena_shader_sources)
        file(RELATIVE_PATH arena_relative "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${arena_shader}")
        set(arena_output "${ARENA_RUNTIME_DIR}/shaders/${arena_relative}.spv")
        get_filename_component(arena_output_dir "${arena_output}" DIRECTORY)
        add_custom_command(OUTPUT "${arena_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${arena_output_dir}"
            COMMAND "${CMAKE_COMMAND}" "-DGLSLC=${ARENA_GLSLC}" "-DINPUT=${arena_shader}"
                "-DOUTPUT=${arena_output}" "-DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CompileShader.cmake"
            DEPENDS "${arena_shader}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CompileShader.cmake"
            DEPFILE "${arena_output}.d" VERBATIM)
        list(APPEND arena_shader_outputs "${arena_output}")
    endforeach()
    add_custom_target(${target}_shaders DEPENDS ${arena_shader_outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()
