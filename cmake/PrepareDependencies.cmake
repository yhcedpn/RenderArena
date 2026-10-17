cmake_minimum_required(VERSION 3.28)

# 由组织者在测评前调用，不属于测评智能体的构建入口。
if(NOT DEFINED ARENA_TASK_DIR)
    message(FATAL_ERROR "使用 -DARENA_TASK_DIR=<任务目录> 指定要准备的答卷")
endif()
get_filename_component(ARENA_TASK_DIR "${ARENA_TASK_DIR}" ABSOLUTE)
if(NOT DEFINED VCPKG_ROOT)
    set(VCPKG_ROOT "$ENV{VCPKG_ROOT}")
endif()
if(CMAKE_HOST_WIN32)
    set(arena_vcpkg "${VCPKG_ROOT}/vcpkg.exe")
    set(arena_triplet x64-windows)
else()
    set(arena_vcpkg "${VCPKG_ROOT}/vcpkg")
    set(arena_triplet x64-linux)
endif()
get_filename_component(arena_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
string(MD5 arena_task_hash "${ARENA_TASK_DIR}")
string(SUBSTRING "${arena_task_hash}" 0 8 arena_task_id)
set(arena_work "${arena_repo_root}/obj/vcpkg/${arena_task_id}/${CMAKE_HOST_SYSTEM_NAME}")

# 使用 vcpkg 默认的用户级共享缓存或用户设置的 VCPKG_BINARY_SOURCES。
execute_process(
    COMMAND "${arena_vcpkg}" install
        "--triplet=${arena_triplet}" "--host-triplet=${arena_triplet}"
        "--x-manifest-root=${ARENA_TASK_DIR}"
        "--x-install-root=${ARENA_TASK_DIR}/obj/dependencies/${CMAKE_HOST_SYSTEM_NAME}"
        "--x-buildtrees-root=${arena_work}/b" "--x-packages-root=${arena_work}/p"
    WORKING_DIRECTORY "${ARENA_TASK_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
