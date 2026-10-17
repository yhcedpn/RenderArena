# 将 Windows 环境变量带入的工具链路径规范为 CMake 路径。
file(TO_CMAKE_PATH "${CMAKE_TOOLCHAIN_FILE}" CMAKE_TOOLCHAIN_FILE)
# vcpkg 在 Windows 下构建依赖时需要较短的临时路径。
# 每个项目和预设独立分区，避免并行构建争用 buildtrees/packages。
string(MD5 arena_task_hash "${CMAKE_CURRENT_SOURCE_DIR}")
string(SUBSTRING "${arena_task_hash}" 0 8 arena_task_id)
get_filename_component(arena_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(arena_vcpkg_work "${arena_repo_root}/obj/vcpkg/${arena_task_id}/${ARENA_PRESET_NAME}")
list(FILTER VCPKG_INSTALL_OPTIONS EXCLUDE REGEX "^--x-(buildtrees|packages)-root=")
list(APPEND VCPKG_INSTALL_OPTIONS
    "--x-buildtrees-root=${arena_vcpkg_work}/b"
    "--x-packages-root=${arena_vcpkg_work}/p")
