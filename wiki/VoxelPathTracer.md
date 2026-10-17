# VoxelPathTracer

使用 C++20、GLSL 460 和 Vulkan 1.4 实现静态体素展厅，要求硬件路径追踪、GPU wavefront、ReSTIR DI、SVGF，以及平面镜反射与厚玻璃透射的分域历史。

## 工程与规格

| 文档 | 用途 |
| --- | --- |
| [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max) | omp + deepseek-v4.1-flash-1017^max 答卷 |
| [VoxelPathTracer_TASK.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/VoxelPathTracer_TASK.md) | 功能、资源语义与验收标准 |
| [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/IMPLEMENTATION.md) | 实现机制与构建、运行核验记录 |
| [任务 AGENTS.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/AGENTS.md) | 工程入口、修改边界与部署映射 |
| [通用 AGENTS.md](https://github.com/yhcedpn/RenderArena/blob/main/AGENTS.md) | 测评隔离、工程约定与交付规则 |

[构建指南](https://github.com/yhcedpn/RenderArena/wiki/Home#构建)说明依赖准备与 Debug/Release 构建步骤。目标平台声明为 Windows、Linux；依赖及版本以答卷的 `vcpkg.json` 为准。
