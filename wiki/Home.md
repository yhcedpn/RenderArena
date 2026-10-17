# RenderArena Wiki

RenderArena 用 C++ 图形编程任务比较模型的渲染与工程实现。本页提供构建指南和任务导航。

## 任务索引

- [Vulkan 任务](https://github.com/yhcedpn/RenderArena/wiki/Vulkan-BenchmarkTasks)
- [OpenGL 任务（历史）](https://github.com/yhcedpn/RenderArena/wiki/OpenGL-BenchmarkTasks)

## 构建

### 组织者准备依赖

1. Windows 安装 Visual Studio 2026 的“使用 C++ 的桌面开发”工作负载、MSVC v145 和 Windows SDK；CMake 需支持 `Visual Studio 18 2026` 生成器。Linux 安装 CMake ≥ 3.28、Ninja 和 GCC。
2. 设置 `VCPKG_ROOT` 指向已安装的 vcpkg。提供 Vulkan SDK 的 `glslc`，可通过 `PATH` 或 `VULKAN_SDK` 定位。运行渲染器所需设备能力与校验层要求见[题面](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/VoxelPathTracer_TASK.md)。
3. 从仓库根目录执行依赖准备脚本：

```powershell
cmake "-DARENA_TASK_DIR=Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max" -P cmake/PrepareDependencies.cmake
```

依赖与版本由答卷的 `vcpkg.json` 声明。安装目录由[依赖准备脚本](https://github.com/yhcedpn/RenderArena/blob/main/cmake/PrepareDependencies.cmake)与构建预设约定；二进制缓存使用 vcpkg 用户级共享缓存或显式设置的 `VCPKG_BINARY_SOURCES`。

依赖准备完成后，测评构建只消费已安装依赖；缺项由组织者补齐。实现智能体的操作范围见[通用执行规范](https://github.com/yhcedpn/RenderArena/blob/main/AGENTS.md#测评隔离)。

### 配置与构建

进入任务目录：

```powershell
cd "Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max"
```

Windows 分别配置并构建 Debug、Release：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

Linux 使用对应预设：

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
```

平台适用性以任务目录 `platforms.json` 为准。预设定义见[根目录 CMakePresets.json](https://github.com/yhcedpn/RenderArena/blob/main/CMakePresets.json)，任务目录引用共享预设；语言标准和编译诊断设置见 [RenderArena.cmake](https://github.com/yhcedpn/RenderArena/blob/main/cmake/RenderArena.cmake)。

Windows 也可打开 `RenderArena.slnx`，选择 `Debug|x64` 或 `Release|x64`。VS 项目委托相同 CMake 预设；未设置 `VCPKG_ROOT` 时，VS 入口使用随 VS 安装的 vcpkg。依赖仍需先按上述步骤准备。

### 运行与产物

可执行文件、DLL 和运行资源位于任务目录 `bin/<预设>/`；应用中间文件位于 `obj/<预设>/`，依赖安装树与应用配置目录分开。VS 调试器从对应产物目录启动程序。

VoxelPathTracer 的程序名与资源部署映射见[任务执行规范“构建与运行入口”](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/AGENTS.md#构建与运行入口)。从产物目录启动对应程序。

构建、算法和交互的完成条件分别见题面“交付与验收”，核验记录见答卷的 [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/IMPLEMENTATION.md)。

## 历史答卷

以下页面提供历史答卷的来源与文档入口；源码与题面固定链接到提交 [`e6249c9`](https://github.com/yhcedpn/RenderArena/tree/e6249c90946b758e1aab3885e6c50e70ec6f7ef1)。

- [VoxelPBRFrustumCulling](https://github.com/yhcedpn/RenderArena/wiki/VoxelPBRFrustumCulling)
- [RubikCube](https://github.com/yhcedpn/RenderArena/wiki/RubikCube)
- [MiniVoxelPathTracer](https://github.com/yhcedpn/RenderArena/wiki/MiniVoxelPathTracer)

## 文档维护

- 文档职责与同题题面维护规则见[通用执行规范](https://github.com/yhcedpn/RenderArena/blob/main/AGENTS.md#文档职责)。
- Wiki 源文件位于仓库 `wiki/`，修改后由 [Sync Wiki 工作流](https://github.com/yhcedpn/RenderArena/blob/main/.github/workflows/sync-wiki.yml)同步到 GitHub Wiki。触发条件见工作流配置。
