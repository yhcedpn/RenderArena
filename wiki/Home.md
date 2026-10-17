# RenderArena Wiki

RenderArena 的项目介绍与当前测评状态见 [README](https://github.com/yhcedpn/RenderArena/blob/main/README.md)。本页提供构建步骤和任务导航。

## 当前任务

[VoxelPathTracer](https://github.com/yhcedpn/RenderArena/wiki/VoxelPathTracer)：Vulkan 1.4 体素展厅，要求硬件路径追踪、GPU wavefront、ReSTIR DI、SVGF 和镜面/玻璃分域历史。实现状态与题面入口见任务页。

## 构建

### 组织者准备依赖

1. Windows 安装 Visual Studio 2026 的“使用 C++ 的桌面开发”工作负载、MSVC v145 和 Windows SDK；CMake 需支持 `Visual Studio 18 2026` 生成器。Linux 安装 CMake ≥ 3.28、Ninja 和 GCC。
2. 设置 `VCPKG_ROOT` 指向已安装的 vcpkg。提供 Vulkan SDK 的 `glslc`，可通过 `PATH` 或 `VULKAN_SDK` 定位。运行渲染器所需设备能力与校验层要求见[题面](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/VoxelPathTracer_TASK.md)。
3. 从仓库根目录执行依赖准备脚本：

```powershell
cmake "-DARENA_TASK_DIR=Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max" -P cmake/PrepareDependencies.cmake
```

各答卷的 `vcpkg.json` 锁定依赖和 baseline。脚本把依赖安装到该答卷的 `obj/dependencies/<主机平台>/`，同树包含 Debug/Release；临时构建目录为仓库根 `obj/vcpkg/<项目路径哈希>/<主机平台>/`。二进制缓存使用 vcpkg 用户级共享缓存或显式设置的 `VCPKG_BINARY_SOURCES`。

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

当前目标可执行文件为 Windows 的 `VoxelPathTracer.exe` 或 Linux 的 `VoxelPathTracer`。`materials.json` 部署到可执行文件同目录；独立 stage shader 在构建时编译到 `shaders/<相对源文件名>.spv`。部署映射见[任务执行规范](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/AGENTS.md#构建与运行入口)。

构建、算法和交互的完成条件分别见题面“交付与验收”。实际核验记录放在对应答卷的 `IMPLEMENTATION.md`，并注明平台、配置、诊断与未核验行为。

## 历史答卷

以下页面保留旧答卷的来源和核验记录；其源码与题面固定链接到提交 [`e6249c9`](https://github.com/yhcedpn/RenderArena/tree/e6249c90946b758e1aab3885e6c50e70ec6f7ef1)。旧记录不代表当前 VoxelPathTracer 的核验结果。

- [OpenGL 任务](https://github.com/yhcedpn/RenderArena/wiki/OpenGL-BenchmarkTasks)：VoxelPBRFrustumCulling。
- [Vulkan 任务](https://github.com/yhcedpn/RenderArena/wiki/Vulkan-BenchmarkTasks)：RubikCube、MiniVoxelPathTracer。

## 文档维护

- 当前工程入口以 `RenderArena.slnx` 为准；新增答卷的平台和构建配置由答卷目录声明。
- 文档职责与同题题面维护规则见[通用执行规范](https://github.com/yhcedpn/RenderArena/blob/main/AGENTS.md#文档职责)。Wiki 任务页链接题面，核验记录注明日期与适用答卷。
- Wiki 源文件位于仓库 `wiki/`。[Sync Wiki 工作流](https://github.com/yhcedpn/RenderArena/blob/main/.github/workflows/sync-wiki.yml)在 `main` 上的 `wiki/**` 或同步工作流文件变更时运行，也支持 `workflow_dispatch` 手动触发。同步以 `wiki/` 为准，包含删除页面；没有内容变化时跳过提交。
- 构建与运行核验在本地执行；构建 CI 和 Release 发布工作流保持停用。
