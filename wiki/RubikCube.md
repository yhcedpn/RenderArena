# RubikCube

使用 C++20 和 Vulkan 1.4 渲染部分缺失的三阶魔方与金属地板，运行时读取 `materials.json`，在 CPU 上生成纹理，并使用 PBR 光照。题目同时约束动态渲染、Synchronization 2、Push Descriptors 和 Host Image Copy/额外传输队列路径。

## 答卷与任务定义

| 工具与模型 | 答卷源码 | 任务文档 |
| --- | --- | --- |
| omp + gpt-5.6-luna@max | [答卷目录](https://github.com/yhcedpn/RenderArena/tree/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_omp+gpt-5.6-luna@max) | [任务定义](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_omp+gpt-5.6-luna@max/RubikCube_TASK.md) |
| Copilot + glm-5.2@max | [答卷目录](https://github.com/yhcedpn/RenderArena/tree/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_Copilot+glm5.2@max) | [规格参考](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/Vulkan/RubikCube_Copilot+glm5.2@max/RubikCube_TASK.md) |
