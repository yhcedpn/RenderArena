# RenderArena Wiki

`RenderArena` 通过真实 C++ 图形编程任务，比较不同模型在窗口初始化、渲染流程、资源管理和复杂图形算法上的实现能力。每份答卷保留任务定义、源码和构建配置，任务页提供规格、答卷来源与评审记录。

- [OpenGL 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/OpenGL-BenchmarkTasks)：PBR 与视锥剔除。
- [Vulkan 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/Vulkan-BenchmarkTasks)：配置驱动 PBR 与硬件路径追踪。

## 如何构建

构建配置仅支持 `Debug|x64` 与 `Release|x64`。

### 方法 1：Windows + Visual Studio

在 Windows 11 系统运行，安装 Windows SDK；GPU 和驱动需满足所选题目的 OpenGL 或 Vulkan 版本与特性要求。

1. 安装 Visual Studio 2026，确保安装 `使用 C++ 的桌面开发` 模块。
2. 安装 MSVC v145 和 VS 自带的 vcpkg，或设置 `VCPKG_ROOT` 指向独立的 vcpkg。
3. 克隆本仓库，打开 `RenderArena.slnx`，构建并运行你想测试的项目，只可选择 `Debug|x64` 或 `Release|x64` 构建配置。

全部 VS 项目调用任务目录的 CMake 预设，与命令行共用依赖、编译、链接及资源复制规则。使用 `VCPKG_ROOT` 指定的 vcpkg；未设置时使用 VS 自带的 vcpkg。VS 调试器从对应 `bin/<预设>/` 启动程序。无需另外执行 vcpkg 的 MSBuild 全局集成。CMake 是唯一实际构建系统，`.vcxproj` 提供 VS 项目视图和启动入口。

### 方法 2：CLI + CMake（Windows / Linux）

任务通过 CMake + vcpkg 构建，适用平台以各答卷的 `platforms.json` 为准。

1. Windows 安装 Visual Studio 2026 的 C++ 开发工具及支持 `Visual Studio 18 2026` 生成器的 CMake；Linux 安装 CMake（≥ 3.28）、Ninja 与 GCC。
2. 设置环境变量 `VCPKG_ROOT` 指向本机 vcpkg 目录。
3. 进入任务目录（如 `OpenGL/VoxelPBRFrustumCulling_Codex+gpt-5.6-terra@max`）：
   - Windows：`cmake --preset windows-msvc-release`，然后 `cmake --build --preset windows-msvc-release`
   - Linux：`cmake --preset linux-gcc-release`，然后 `cmake --build --preset linux-gcc-release`
   - Debug 对应 `windows-msvc-debug` / `linux-gcc-debug`；预设定义集中在仓库根 `CMakePresets.json`，各任务目录的 `CMakePresets.json` 引用同一份。
4. 依赖由各任务目录 `vcpkg.json`（manifest 模式）声明，vcpkg 按 `x64-windows` / `x64-linux` triplet 自动安装；每个项目及预设各自安装依赖到 `obj/<预设>/vcpkg_installed/`，避免混用不同题目的依赖。语言标准统一为 C++20/C17，编译器警告视为错误。
5. 运行 Release 产物：Windows 下为 `bin\windows-msvc-release\<可执行名>.exe`；Linux 下为 `./bin/linux-gcc-release/<可执行名>`。需要外部资源的任务应保留输出目录中的 `shaders/` 与 `materials.json`。

> 说明：
> - 渲染程序需要图形环境运行。Linux 上请在带 X11 会话与 GPU 驱动（或 mesa 软件渲染）的机器上运行；运行检查在本地执行。
> - 平台适用性由各任务目录 `platforms.json` 声明（`{"build_platforms": ["windows","linux"]}`，缺失时构建矩阵包含两平台）；两份 VoxelPBR 答卷使用 Windows 专有 API，且仅声明 `windows`，不参与 Linux 构建。平台声明不代表构建与运行验收已经通过。

可执行文件、DLL 和运行资源统一放在 `bin/<预设>/`；CMake 生成文件、目标文件、VS 包装项目中间文件和 vcpkg 依赖统一放在 `obj/<预设>/`。Debug/Release 以及 Windows/Linux 分开，VS 与 CLI 共用同一预设的产物。`bin/obj` 已由现有 Git 忽略规则覆盖。

## 任务与文档维护

- [OpenGL 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/OpenGL-BenchmarkTasks)
- [Vulkan 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/Vulkan-BenchmarkTasks)

新增答卷放在 `OpenGL/` 或 `Vulkan/` 的直接子目录，平台由 `platforms.json` 声明。仓库不配置 CI、自动 Wiki 同步或 Release 发布；源码、题面及配置通过 Git 维护，程序在本地构建。

2026-10-17 已清理历史 Release 和 CI 构建附件。Wiki 源文件位于仓库 `wiki/`，更新时手动同步。

## 本机构建核验（2026-10-17）

Windows 11、Visual Studio 2026 / MSVC v145 下，以下六份答卷的 CMake Debug/Release 均通过；整个 `RenderArena.slnx` 的 `Debug|x64` 和 `Release|x64` 也均通过，最终构建日志无 warning/error。

| 题目 | 答卷 | Debug | Release |
| --- | --- | --- | --- |
| VoxelPBRFrustumCulling | Codex + gpt-5.6-terra@max | 通过 | 通过 |
| VoxelPBRFrustumCulling | omp + gpt-5.6-luna@max | 通过 | 通过 |
| RubikCube | Copilot + glm5.2@max | 通过 | 通过 |
| RubikCube | omp + gpt-5.6-luna@max | 通过 | 通过 |
| MiniVoxelPathTracer | Codex + gpt-6.1-sol@high | 通过 | 通过 |
| MiniVoxelPathTracer | omp + deepseek-v4.1-flash-1017@max | 通过 | 通过 |

本次验证包含配置、编译、链接与运行资源部署；Linux 构建和图形交互运行未在本次执行。Debug/Release 的链接库分别来自当前项目的 vcpkg `debug/lib` 和 `lib`，不链接另一份答卷的依赖或 Vulkan SDK loader 库。Vulkan 函数由 volk 加载系统驱动。
