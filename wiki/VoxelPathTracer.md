# VoxelPathTracer

体素展厅路径追踪任务，涉及 GPU wavefront（波前调度）、ReSTIR DI、SVGF，以及镜面与厚玻璃的分域历史。具体要求见题面。

## 工程与规格

| 文档 | 用途 |
| --- | --- |
| [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max) | omp + deepseek-v4.1-flash-1017^max 答卷 |
| [VoxelPathTracer_TASK.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/VoxelPathTracer_TASK.md) | 功能、资源语义与验收标准 |
| [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/IMPLEMENTATION.md) | 实现机制与构建、运行核验记录 |
| [任务 AGENTS.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/AGENTS.md) | 工程入口、修改边界与部署映射 |
| [通用 AGENTS.md](https://github.com/yhcedpn/RenderArena/blob/main/AGENTS.md) | 测评隔离、工程约定与交付规则 |

[构建指南](https://github.com/yhcedpn/RenderArena/wiki/Home#构建)提供依赖准备和构建步骤；目标平台与依赖分别见工程目录的 `platforms.json`、`vcpkg.json`。
