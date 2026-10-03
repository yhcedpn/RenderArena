# OpenGL 图形编程任务

本页介绍仓库 `OpenGL/` 目录下的四道 OpenGL 4.6 Core Profile 图形编程任务（难度由简单到复杂），以及不同模型在这些任务上的完成结果与评审。Vulkan 任务见 [Vulkan 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/Vulkan-BenchmarkTasks)。

## 任务清单

| 题目 | 一句话主题 |
| --- | --- |
| [HelloWindow](https://github.com/yhcedpn/RenderArena/wiki/HelloWindow) | 窗口初始化：GLFW + GLAD + OpenGL 4.6 |
| [CachedCubePipelines](https://github.com/yhcedpn/RenderArena/wiki/CachedCubePipelines) | CPU 侧命令/状态缓存绘制 + 双 pipeline 重放 |
| [ProceduralDeferredRenderer](https://github.com/yhcedpn/RenderArena/wiki/ProceduralDeferredRenderer) | 多 pass 程序化渲染器：shadow mapping、HDR、后处理 |
| [VoxelPBRFrustumCulling](https://github.com/yhcedpn/RenderArena/wiki/VoxelPBRFrustumCulling) | PBR + 实例化 + 可验证的 CPU 视锥剔除 |

## Release

已发布的任务产物见 [GitHub Releases](https://github.com/yhcedpn/RenderArena/releases)。具体版本包含哪些答卷，以该 Release 的附件为准。

构建步骤见 [构建指南](https://github.com/yhcedpn/RenderArena/wiki#如何构建)，支持 `Debug|x64` 与 `Release|x64`。两份 VoxelPBR 答卷使用 Windows 专有 API，`platforms.json` 仅声明 `windows`，不参与 Linux 构建；相关评审见 [#17](https://github.com/yhcedpn/RenderArena/issues/17) 和 [#21](https://github.com/yhcedpn/RenderArena/issues/21)。其余 OpenGL 答卷参与 Windows/Linux 构建矩阵。
