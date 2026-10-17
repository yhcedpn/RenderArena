# RenderArena

## 项目简介

`RenderArena` 用一组图形编程任务，测试不同模型在真实 C++ 图形工程场景中的表现。

结果按图形 API 或技术类别组织在根目录下。每个任务目录通常包含任务定义、模型生成的工程结果以及对应的构建配置：

- `OpenGL/`：现有的 OpenGL 编程任务（用 OpenGL 4.6 Core Profile）及不同模型的完成结果
- `Vulkan/`：RubikCube、MiniVoxelPathTracer 等 Vulkan 编程任务及不同模型的完成结果

## 文档（Wiki）

项目文档位于 [GitHub Wiki](https://github.com/yhcedpn/RenderArena/wiki)，源文件保存在仓库 `wiki/` 目录，更新后手动同步到 Wiki。

- [Wiki 首页](https://github.com/yhcedpn/RenderArena/wiki)：项目介绍与构建指南
- [OpenGL 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/OpenGL-BenchmarkTasks)：任务清单、构建说明与相关评审 issue
- [Vulkan 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/Vulkan-BenchmarkTasks)：任务清单、模型答卷与核验结果

## 构建

构建以 CMake 为跨平台契约：Windows 可用 VS2026（`RenderArena.slnx`）或 CMake，Linux 使用 CMake；依赖由各任务目录 `vcpkg.json` 管理。VS 项目委托 CMake，共用 C++20/C17 设置和产物：`bin/<预设>/` 保存可执行文件及运行资源，`obj/<预设>/` 保存中间文件及独立的 vcpkg 安装树。完整步骤与平台适用性见 [Wiki · 如何构建](https://github.com/yhcedpn/RenderArena/wiki#如何构建)。

## 适合用来观察什么

- 不同模型在简单、中等、复杂图形编程任务上的稳定性差异
- 在 PBR、视锥剔除和硬件路径追踪等渲染流程中，代码组织能力如何变化
- 面对资源生命周期、FBO、shader、输入控制时是否容易出错
- 模型是否会编造不存在，不可用的方法，函数，参数等
- 在严格约束下，模型是否还能产出结构清晰、可维护的代码
