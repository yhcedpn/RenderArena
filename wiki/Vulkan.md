# Vulkan 测评

Vulkan 图形编程任务与现有答卷。构建命令见[首页“构建”](https://github.com/yhcedpn/RenderArena/wiki/Home#构建)。

## 题目

| 题目 | 主题 | 题面 |
| --- | --- | --- |
| [VoxelPathTracer](https://github.com/yhcedpn/RenderArena/wiki/VoxelPathTracer) | 体素展厅路径追踪、GPU wavefront、ReSTIR DI、SVGF | [VoxelPathTracer_TASK.md](https://github.com/yhcedpn/RenderArena/blob/main/Tasks/VoxelPathTracer_TASK.md) |

## 现有答卷

| 题目 | 工具与模型 | 源码 | 实现说明 | 核验结果 |
| --- | --- | --- | --- | --- |
| VoxelPathTracer | omp + deepseek-v4.1-flash-1017^max | [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max/IMPLEMENTATION.md) | Windows Debug 构建成功并可连续渲染；Windows Release 运行失败；Linux 未核验 |
| VoxelPathTracer | Codex + gpt-5.6-sol^max | [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_codex+gpt-5.6-sol^max) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_codex+gpt-5.6-sol^max/IMPLEMENTATION.md) | 严格隔离重跑；已接入 RT、wavefront、ReSTIR 与分域 SVGF；Windows Debug/Release 无诊断构建，最终持续呈现 20/15 秒、stderr 为空；按 PID 终止，正常关闭、人工画面交互与 Linux 未核验 |
| VoxelPathTracer | Codex + gpt-6-luna^xhigh | [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_codex+gpt-6-luna^xhigh) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_codex+gpt-6-luna^xhigh/IMPLEMENTATION.md) | 未完成：仅 CPU 材质、纹理 mip、场景网格与相机数学；Windows Debug/Release 构建成功，运行退出码 1；无 Vulkan 渲染与 shader；Linux 未核验 |
| VoxelPathTracer | Codex + gpt-6-luna^max | [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_codex+gpt-6-luna^max) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_codex+gpt-6-luna^max/IMPLEMENTATION.md) | 未完成：仅 CPU 材质、纹理 mip、场景网格与发光面 CDF；Windows Debug/Release 构建成功，运行退出码 2；无 Vulkan 渲染与 shader；Linux 未核验 |
| VoxelPathTracer | Codex + gpt-6.1-terra^xhigh | [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_codex+gpt-6.1-terra^xhigh) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_codex+gpt-6.1-terra^xhigh/IMPLEMENTATION.md) | 未完成：仅 CPU 材质、纹理 mip、场景网格与部分 Vulkan 能力查询；Windows Debug/Release 构建成功，运行退出码 1；无 GPU 渲染与 shader；Linux 未核验 |
| VoxelPathTracer | Codex + gpt-6.1-terra^max | [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_codex+gpt-6.1-terra^max) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_codex+gpt-6.1-terra^max/IMPLEMENTATION.md) | 未完全达标：已接入 RT、wavefront、ReSTIR、SVGF；Windows Debug/Release 构建并呈现后正常退出，Debug 无 core/sync warning/error；深层光学历史、大尺寸 resize 与数值输入有缺口；目视交互及 Linux 未核验 |
| VoxelPathTracer | Codex + gpt-6.1-sol^xhigh | [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_codex+gpt-6.1-sol^xhigh) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_codex+gpt-6.1-sol^xhigh/IMPLEMENTATION.md) | 已接入 RT、wavefront、ReSTIR 与分域 SVGF；Windows Debug/Release 构建成功，分别提交 107/110 帧并正常退出；Debug 无 core/sync warning/error；人工画面、交互与 Linux 未核验 |
| VoxelPathTracer | Codex + gpt-6.1-sol^max | [工程目录](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/VoxelPathTracer_codex+gpt-6.1-sol^max) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/VoxelPathTracer_codex+gpt-6.1-sol^max/IMPLEMENTATION.md) | 已接入 RT、wavefront、ReSTIR 与分域 SVGF；Windows Debug/Release 构建成功，分别提交 50/42 帧并正常退出；Debug 无 core/sync warning/error；人工画面、交互与 Linux 未核验 |
