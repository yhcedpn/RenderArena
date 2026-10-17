# OpenGL 图形编程任务

本页介绍仓库 `OpenGL/` 目录下的 OpenGL 4.6 Core Profile 图形编程任务，以及不同模型在这些任务上的完成结果与评审。Vulkan 任务见 [Vulkan 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/Vulkan-BenchmarkTasks)。

## 任务清单

| 题目 | 一句话主题 |
| --- | --- |
| [VoxelPBRFrustumCulling](https://github.com/yhcedpn/RenderArena/wiki/VoxelPBRFrustumCulling) | PBR + 实例化 + 可验证的 CPU 视锥剔除 |



构建步骤见 [构建指南](https://github.com/yhcedpn/RenderArena/wiki#如何构建)，支持 `Debug|x64` 与 `Release|x64`。两份 VoxelPBR 答卷使用 Windows 专有 API，`platforms.json` 仅声明 `windows`，不适用于 Linux 构建；相关评审见 [#17](https://github.com/yhcedpn/RenderArena/issues/17) 和 [#21](https://github.com/yhcedpn/RenderArena/issues/21)。
