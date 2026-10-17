# RubikCube（历史）

本页记录旧答卷，已移出当前解决方案。源码与题面固定到保留答卷的历史提交。

使用 C++20 和 Vulkan 1.4 渲染部分缺失的三阶魔方与金属地板，运行时读取 `materials.json`，在 CPU 上生成纹理，并使用 PBR 光照。题目同时约束动态渲染、Synchronization 2、Push Descriptors 和 Host Image Copy/额外传输队列路径。

| 工具与模型 | 任务定义 | 收录 PR |
| --- | --- | --- |
| omp + gpt-5.6-luna@max | [任务文件](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_omp+gpt-5.6-luna@max/RubikCube_TASK.md) | [#43](https://github.com/yhcedpn/RenderArena/pull/43) |
| Copilot + glm-5.2@max | [题目副本](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_Copilot+glm5.2@max/RubikCube_TASK.md) | [#47](https://github.com/yhcedpn/RenderArena/pull/47) |

## Copilot + glm-5.2 答卷来源

答卷位于 [`Vulkan/RubikCube_Copilot+glm5.2@max`](https://github.com/yhcedpn/RenderArena/tree/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_Copilot+glm5.2@max)。贡献者说明生成过程中包含人为反馈，提交前未在 Visual Studio 环境测试。[PR #47](https://github.com/yhcedpn/RenderArena/pull/47) 未附题目文件；仓库中的题目副本与 [PR #43](https://github.com/yhcedpn/RenderArena/pull/43) 的已有题目逐字节一致，用于提供规格参考，不能据此确认贡献者使用的原始提示词。

仓库保留原始 `src/` 多编译单元结构、着色器和材质输入；工程接入提供共享 CMake 预设、VS 解决方案入口及平台声明。最初接入时，原始源文件、着色器和材质输入共 13 个文件的 SHA256 保持一致。2026-10-17 为通过警告视为错误的构建，修正了 `vulkan_engine.cpp` 中两处 MSVC CRT 弃用诊断，并删除未使用变量；C++ 头文件现统一为 `.hpp`，渲染算法和资源保持原有实现。Visual Studio 与命令行使用同一份 CMake 构建，复现时使用对应历史提交中的构建配置。

## 构建与核验状态

2026-10-17 在 Windows 11 / MSVC v145 上，两份答卷的 Debug/Release 均通过 CMake 及 VS 解决方案构建，编译器警告视为错误。Copilot 答卷启用 `/W4 /WX`，早期的三处诊断已修复；omp 答卷移除了源码中的手写链接指令，统一使用 CMake 导入目标。

### 早期核验（2026-10-03）

Copilot 答卷的 Windows 核验中，Debug/Release 均完成配置并生成可执行文件，但存在 3 条 MSVC 警告。仓库将编译诊断按失败处理，因此两种配置的构建核验均未通过。Linux 构建及程序运行验收尚未执行。

当时的诊断为 `getenv`、`fopen` 的 C4996 和未使用局部变量 `cp` 的 C4189；上述 2026-10-17 记录已更新其构建结论。
