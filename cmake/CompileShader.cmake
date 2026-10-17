# 将编译器诊断提升为构建失败；成功的 glslc 不输出文本。
execute_process(
    COMMAND "${GLSLC}" --target-env=vulkan1.4 --target-spv=spv1.6 -std=460 -Werror
        -MD -MF "${OUTPUT}.d" "-I${INCLUDE_DIR}" "${INPUT}" -o "${OUTPUT}"
    RESULT_VARIABLE arena_result OUTPUT_VARIABLE arena_stdout ERROR_VARIABLE arena_stderr)
if(NOT arena_result EQUAL 0 OR NOT "${arena_stdout}${arena_stderr}" STREQUAL "")
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "GLSL 编译失败：${INPUT}\n${arena_stdout}${arena_stderr}")
endif()
