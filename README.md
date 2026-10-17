# RenderArena

RenderArena 用真实 C++ 图形编程任务，比较模型在渲染算法、资源管理和工程实现上的表现。每份答卷保留题面、源码与构建配置。

## 当前测评

当前 [解决方案](RenderArena.slnx) 仅收录 VoxelPathTracer，工具与模型为 omp + deepseek-v4.1-flash-1017^max。工程入口与实现状态见[任务概览](wiki/VoxelPathTracer.md)。

- [任务题面](Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/VoxelPathTracer_TASK.md)：功能、资源语义和验收标准。
- [通用执行规范](AGENTS.md)与[任务执行规范](Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/AGENTS.md)：测评隔离、工程入口和修改边界。
- [构建指南](wiki/Home.md#构建)：组织者准备依赖，以及 Windows / Linux 构建与运行步骤。
- [任务索引](wiki/Vulkan-BenchmarkTasks.md)：当前工程与历史答卷入口。

## 文档维护

Wiki 源文件位于 [wiki/](wiki/Home.md)，推送到 `main` 后由 [Sync Wiki 工作流](.github/workflows/sync-wiki.yml)自动同步到 [GitHub Wiki](https://github.com/yhcedpn/RenderArena/wiki)，也可手动触发工作流。构建步骤集中在 Wiki 首页，功能要求以任务题面为准，Wiki 任务页提供概览、来源和核验记录。

旧 OpenGL、RubikCube 与 MiniVoxelPathTracer 答卷见 [历史任务索引](wiki/Home.md#历史答卷)。历史页面链接到保留答卷的固定提交。
