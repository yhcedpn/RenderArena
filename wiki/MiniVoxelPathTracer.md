# MiniVoxelPathTracer（历史）

本页记录旧答卷，已移出当前解决方案。源码、题面和实现说明固定到保留答卷的历史提交。

## 答卷与相关文档

| 工具与模型 | 答卷源码 | 任务定义 | 实现说明 |
| --- | --- | --- | --- |
| Codex + gpt-6.1-sol@high | [答卷目录](https://github.com/yhcedpn/RenderArena/tree/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/MiniVoxelPathTracer_Codex+gpt-6.1-sol@high) | [MiniVoxelPathTracer_TASK.md](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/MiniVoxelPathTracer_Codex+gpt-6.1-sol@high/MiniVoxelPathTracer_TASK.md) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/MiniVoxelPathTracer_Codex+gpt-6.1-sol@high/IMPLEMENTATION.md) |
| omp + deepseek-v4.1-flash-1017@max | [答卷目录](https://github.com/yhcedpn/RenderArena/tree/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/MiniVoxelPathTracer_omp+deepseek-v4.1-flash-1017@max) | [MiniVoxelPathTracer_TASK.md](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/MiniVoxelPathTracer_omp+deepseek-v4.1-flash-1017@max/MiniVoxelPathTracer_TASK.md) | [IMPLEMENTATION.md](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/MiniVoxelPathTracer_omp+deepseek-v4.1-flash-1017@max/IMPLEMENTATION.md) |

Codex 答卷使用独立 Codex 会话生成，生成阶段禁止联网搜索及读取任务目录外的内容；实现包含代码复查反馈后的修正。完整功能与验收要求见表中任务定义；实际 pass 顺序、队列容量、ReSTIR 归一化、光学历史及同步规则见对应实现说明。

## 构建与核验状态

2026-10-17，两份答卷均通过 Windows CMake Debug/Release 和 VS 解决方案的构建，最终日志无 warning/error。

以下 Codex 答卷结果记录于 2026-10-03，详细核验范围见 [实现说明的实际核验章节](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/MiniVoxelPathTracer_Codex+gpt-6.1-sol@high/IMPLEMENTATION.md#实际核验)。复现时使用对应历史提交中的构建配置。

| 平台 | 已记录的核验结果 |
| --- | --- |
| Windows | Debug/Release 配置与构建无编译诊断；在 Radeon 780M 上实际渲染、正常关闭并退出 0，9 个 GLSL stage 运行时编译通过。Debug 启用 core/synchronization validation，无失败诊断；Release 不启用校验层或请求校验专用扩展。两种模式的标题栏 FPS 与 ms/frame 均持续更新，缩放及最小化恢复后重新统计。 |
| Linux | 声明为目标平台，尚未执行构建与运行核验。 |

运行核验还覆盖相机与窗口交互、镜面/玻璃分域历史、11 类非法材质配置拒绝及合法材质变化。记录中的早期 Release 校验层运行发生在构建模式策略调整之前；现有 Release 按题目要求关闭校验层。
