# Vulkan 图形编程任务

本页对应仓库 `Vulkan/` 中的题目与模型答卷。同一道题的不同模型结果分别归档，保留功能代码以供比较。

| 题目 | 主题 | 已归档答卷 |
| --- | --- | --- |
| [VulkanWindow](https://github.com/yhcedpn/RenderArena/wiki/VulkanWindow) | Vulkan 窗口与四象限渲染 | omp + gpt-5.6-luna、omp + deepseek-v4-flash-0731、Copilot + glm-5.2 |
| [RubikCube](https://github.com/yhcedpn/RenderArena/wiki/RubikCube) | Vulkan 1.4、配置驱动 PBR、程序化纹理与部分缺失魔方 | omp + gpt-5.6-luna、Copilot + glm-5.2 |
| [MiniVoxelPathTracer](https://github.com/yhcedpn/RenderArena/wiki/MiniVoxelPathTracer) | Vulkan 1.4 硬件路径追踪、GPU wavefront、ReSTIR DI 与 SVGF | Codex + gpt-6.1-sol@high |

构建方法见 [构建指南](https://github.com/yhcedpn/RenderArena/wiki#如何构建)。各答卷的来源、平台适用性与核验结果见对应任务页。CI 构建矩阵验证编译，画面、同步、输入和资源释放通过实际运行验收。

## Release

已发布的任务产物见 [GitHub Releases](https://github.com/yhcedpn/RenderArena/releases)。发布流程按各答卷 `platforms.json` 中的 `build_platforms` 选择构建平台；具体版本包含哪些答卷，以该 Release 的附件为准。
