# RubikCube（历史）

本页记录旧答卷，已移出当前解决方案。源码与题面固定到保留答卷的历史提交。

使用 C++20 和 Vulkan 1.4 渲染部分缺失的三阶魔方与金属地板，运行时读取 `materials.json`，在 CPU 上生成纹理，并使用 PBR 光照。题目同时约束动态渲染、Synchronization 2、Push Descriptors 和 Host Image Copy/额外传输队列路径。

| 工具与模型 | 任务定义 | 收录 PR |
| --- | --- | --- |
| omp + gpt-5.6-luna@max | [任务文件](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_omp+gpt-5.6-luna@max/RubikCube_TASK.md) | [#43](https://github.com/yhcedpn/RenderArena/pull/43) |
| Copilot + glm-5.2@max | [题目副本](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_Copilot+glm5.2@max/RubikCube_TASK.md) | [#47](https://github.com/yhcedpn/RenderArena/pull/47) |

## Copilot + glm-5.2 答卷来源

答卷位于 [`Vulkan/RubikCube_Copilot+glm5.2@max`](https://github.com/yhcedpn/RenderArena/tree/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_Copilot+glm5.2@max)。贡献者说明生成过程中包含人为反馈，提交前未在 Visual Studio 环境测试。[PR #47](https://github.com/yhcedpn/RenderArena/pull/47) 未附题目文件；仓库中的题目副本与 [PR #43](https://github.com/yhcedpn/RenderArena/pull/43) 的已有题目逐字节一致，用于提供规格参考，不能据此确认贡献者使用的原始提示词。
