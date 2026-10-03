# RubikCube

使用 C++20 和 Vulkan 1.4 渲染部分缺失的三阶魔方与金属地板，运行时读取 `materials.json`，在 CPU 上生成纹理，并使用 PBR 光照。题目同时约束动态渲染、Synchronization 2、Push Descriptors 和 Host Image Copy/额外传输队列路径。

| 工具与模型 | 任务定义 | 收录 PR |
| --- | --- | --- |
| omp + gpt-5.6-luna@max | [任务文件](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/RubikCube_omp+gpt-5.6-luna@max/RubikCube_TASK.md) | [#43](https://github.com/yhcedpn/RenderArena/pull/43) |
| Copilot + glm-5.2@max | [题目副本](https://github.com/yhcedpn/RenderArena/blob/main/Vulkan/RubikCube_Copilot+glm5.2@max/RubikCube_TASK.md) | [#47](https://github.com/yhcedpn/RenderArena/pull/47) |

## Copilot + glm-5.2 答卷接入

保留贡献者的目录名 `RubikCube_Copilot+glm5.2@max`、`src/` 多编译单元结构、着色器及材质输入，避免工程整理改变评测对象。题目副本来自 #43 已有题目，PR #47 未附题目文件；它用于补齐仓库内的题目引用，不作为贡献者原始提示词的证明。

贡献者说明生成时有人为反馈，原提交未在 VS 环境测试。后续工程接入补齐共享 CMake 预设、VS 解决方案入口和 CI 平台字段，修正依赖基线与 vcpkg 导出目标，不修复功能代码。VS 入口与命令行都使用同一份 CMake 构建。

2026-10-03 本地 Windows Debug/Release 均完成配置并生成可执行文件，但原始代码存在 3 条 MSVC 警告，按仓库规则构建核验未通过。未修改功能代码或屏蔽警告；Linux 与运行验收未执行。

| 位置（`src/vulkan_engine.cpp`） | 诊断 | 内容 |
| --- | --- | --- |
| 59 | C4996 | `getenv` 的 MSVC 弃用/安全诊断 |
| 208 | C4189 | 局部变量 `cp` 已初始化但未使用 |
| 1536 | C4996 | `fopen` 的 MSVC 弃用/安全诊断 |

原始源文件、着色器和材质输入共 13 个文件的 SHA256 在接入前后保持一致；题目副本与现有题目逐字节一致。项目、filters 和解决方案引用检查通过，两种配置的输出资源与原始输入一致。初次依赖构建遇到 SPIRV-Tools 的 MSVC C1083（失败对象文件路径长 278 字符）；使用 vcpkg 的短 buildtrees 路径完成依赖安装后，已移除本地缓存中的临时选项。该验证参数未写入仓库预设。
