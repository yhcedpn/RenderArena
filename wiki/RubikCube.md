# RubikCube

使用 C++20 和 Vulkan 1.4 渲染部分缺失的三阶魔方与金属地板，运行时读取 `materials.json`，在 CPU 上生成纹理，并使用 PBR 光照。题目同时约束动态渲染、Synchronization 2、Push Descriptors 和 Host Image Copy/额外传输队列路径。

| 工具与模型 | 任务定义 | 收录 PR |
| --- | --- | --- |
| omp + gpt-5.6-luna@max | [任务文件](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/RubikCube_omp+gpt-5.6-luna@max/RubikCube_TASK.md) | [#43](https://github.com/yhcedpn/RenderArena/pull/43) |
| Copilot + glm-5.2@max | [题目副本](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/RubikCube_Copilot+glm5.2@max/RubikCube_TASK.md) | [#47](https://github.com/yhcedpn/RenderArena/pull/47) |

## Copilot + glm-5.2 答卷来源

答卷位于 [`Vulkan/RubikCube_Copilot+glm5.2@max`](https://github.com/yhcedpn/RenderArena/tree/main/Vulkan/RubikCube_Copilot+glm5.2@max)。贡献者说明生成过程中包含人为反馈，提交前未在 Visual Studio 环境测试。[PR #47](https://github.com/yhcedpn/RenderArena/pull/47) 未附题目文件；仓库中的题目副本与 [PR #43](https://github.com/yhcedpn/RenderArena/pull/43) 的已有题目逐字节一致，用于提供规格参考，不能据此确认贡献者使用的原始提示词。

仓库保留原始 `src/` 多编译单元结构、着色器和材质输入；工程接入提供共享 CMake 预设、VS 解决方案入口及 CI 平台声明。原始源文件、着色器和材质输入共 13 个文件的 SHA256 在接入前后保持一致。Visual Studio 与命令行使用同一份 CMake 构建，操作方法见 [构建指南](https://github.com/yhcedpn/RenderArena/wiki#如何构建)。

## 构建与核验状态

2026-10-03 的 Windows 核验中，Debug/Release 均完成配置并生成可执行文件，但存在以下 3 条 MSVC 警告。仓库将编译诊断按失败处理，因此两种配置的构建核验均未通过。Linux 构建及程序运行验收尚未执行。

| 源码位置 | 诊断 | 内容 |
| --- | --- | --- |
| [vulkan_engine.cpp:59](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/RubikCube_Copilot+glm5.2@max/src/vulkan_engine.cpp#L59) | C4996 | `getenv` 的 MSVC 弃用/安全诊断 |
| [vulkan_engine.cpp:208](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/RubikCube_Copilot+glm5.2@max/src/vulkan_engine.cpp#L208) | C4189 | 局部变量 `cp` 已初始化但未使用 |
| [vulkan_engine.cpp:1536](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/RubikCube_Copilot+glm5.2@max/src/vulkan_engine.cpp#L1536) | C4996 | `fopen` 的 MSVC 弃用/安全诊断 |
