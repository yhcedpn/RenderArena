# RenderArena

## 项目简介

`RenderArena` 用一组图形编程任务，测试不同模型在真实 C++ 图形工程场景中的表现。

结果按图形 API 或技术类别组织在根目录下。每个任务目录通常包含任务定义、模型生成的工程结果以及对应的构建配置：

- `Vulkan/`：Vulkan 测评项目；VoxelPathTracer 使用 Vulkan 1.4。

## 文档（Wiki）

项目文档位于 [GitHub Wiki](https://github.com/yhcedpn/RenderArena/wiki)，源文件保存在仓库 `wiki/` 目录，更新后手动同步到 Wiki。

- [Wiki 首页](https://github.com/yhcedpn/RenderArena/wiki)：项目介绍与构建指南
- [OpenGL 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/OpenGL-BenchmarkTasks)：任务清单、构建说明与相关评审 issue
- [Vulkan 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/Vulkan-BenchmarkTasks)：任务清单、模型答卷与核验结果

OpenGL 与旧 Vulkan 答卷的 Wiki 页面保留为历史资料，当前项目入口以 `RenderArena.slnx` 为准。

## 构建

Windows 使用 VS2026（`RenderArena.slnx`）或 CMake；Linux 使用 CMake、Ninja 和 GCC。VS 项目委托 CMake。C++20/C17 的语言标准及编译诊断设置集中在 `cmake/RenderArena.cmake`；具体平台以项目 `platforms.json` 为准。

组织者先设置 `VCPKG_ROOT` 指向已安装的 vcpkg，提供 Vulkan SDK 的 `glslc`，再从仓库根准备依赖：

```powershell
cmake "-DARENA_TASK_DIR=Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max" -P cmake/PrepareDependencies.cmake
```

每份答卷用自己的 `vcpkg.json` 锁定依赖和 baseline，并安装到该答卷的 `obj/dependencies/<主机平台>/`；同树包含 Debug/Release，两种应用配置无需重复安装。依赖临时构建目录在仓库根 `obj/vcpkg/<项目路径哈希>/<主机平台>/`。二进制缓存默认使用 vcpkg 用户级共享缓存；可用 `VCPKG_BINARY_SOURCES` 指定缓存源，脚本保留显式设置。

依赖准备完成后，进入任务目录，执行：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

Linux 使用 `linux-gcc-debug` 和 `linux-gcc-release`。测评构建关闭依赖自动安装；缺项交由组织者处理。可执行文件、DLL 和运行资源位于 `bin/<预设>/`；应用中间文件位于 `obj/<预设>/`。GLSL 460 在构建时编译为 Vulkan 1.4 / SPIR-V 1.6，输出至可执行文件同目录的 `shaders/`。

根目录 `AGENTS.md` 定义通用测评规则，项目 `AGENTS.md` 定义工程入口与修改边界，`*_TASK.md` 定义题目功能和验收。Wiki 中的旧答卷与核验记录属于历史资料。

## 适合用来观察什么

- 不同模型在复杂图形编程任务上的稳定性差异
- 在 PBR、视锥剔除和硬件路径追踪等复杂高级渲染流程中，代码组织能力如何变化
- 面对资源生命周期、FBO、shader、输入控制时是否容易出错
- 模型是否会编造不存在，不可用的方法，函数，参数等
- 在严格约束下，模型是否还能产出结构清晰、可维护的代码
