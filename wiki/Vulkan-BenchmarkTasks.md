# Vulkan 图形编程任务

本页对应仓库 `Vulkan/` 中的题目与模型答卷。同一道题的不同模型结果分别归档，保留功能代码以供比较。

| 题目 | 主题 | 已归档答卷 |
| --- | --- | --- |
| [VulkanWindow](Vulkan-BenchmarkTasks/VulkanWindow) | Vulkan 窗口与四象限渲染 | omp + gpt-5.6-luna、omp + deepseek-v4-flash-0731、Copilot + glm-5.2 |
| [RubikCube](Vulkan-BenchmarkTasks/RubikCube) | Vulkan 1.4、配置驱动 PBR、程序化纹理与部分缺失魔方 | omp + gpt-5.6-luna、Copilot + glm-5.2 |
| [MiniVoxelPathTracer](Vulkan-BenchmarkTasks/MiniVoxelPathTracer) | Vulkan 1.4 硬件路径追踪、GPU wavefront、ReSTIR DI 与 SVGF | Codex + gpt-6.1-sol@high（Windows 已核验，Linux 未核验） |

构建方法见 [Home](Home)。Windows/Linux 构建矩阵仅验证编译；画面、同步、输入和资源释放仍需按题目实际验收。Release 发布流程按各答卷的 `build_platforms` 筛选，失败答卷不能视为已经有可用发布产物。

## MiniVoxelPathTracer

[题面](Vulkan-BenchmarkTasks/MiniVoxelPathTracer)：Vulkan 1.4 硬件路径追踪、GPU wavefront、自研 ReSTIR DI 与 SVGF。Codex + gpt-6.1-sol@high 的答卷目录为 `Vulkan/MiniVoxelPathTracer_Codex+gpt-6.1-sol@high`，实际 pass、容量、归一化、历史与同步规则见答卷中的 `IMPLEMENTATION.md`。

答卷由未继承主会话历史的独立 Codex 会话生成，明确禁止联网搜索及显式读取工作目录外的内容。主会话复查后反馈了 ReSTIR 空间邻居取整可能超出 8 像素半径的问题，由原实现会话改为从 196 个合法整数偏移中选择四个不同邻居并复验；主会话仅整理目录、构建框架、文档与末尾空白，没有另行添加功能实现。

2026-10-03 主会话复核 Windows Debug/Release 配置与构建，均无诊断；最终 Release 在 Radeon 780M 上实际启动、渲染并正常关闭，退出码为 0，core/synchronization validation 无失败诊断。原实现会话另记录了镜面与玻璃分域历史、相机及窗口交互、11 类非法配置拒绝和合法材质变化检查。构建配置、题面与材质输入在实现阶段保持不变，VS 仅追加新文件引用。Linux 构建与运行未在本地执行，不能据此声称两平台均已通过。

用户随后在「检查 MiniVoxelPathTracer 校验层」会话追加要求：仅 Debug 启用校验层，Release 不请求校验专用扩展；标题栏显示 FPS 和 ms/frame。该会话完成代码与 Windows 两种模式的构建、运行核验，主会话据此同步正式题面与答卷副本。上述原始 Release 校验记录属于追加修改前的验证；最终 Release 不启用校验层。
