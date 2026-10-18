# VoxelPathTracer

Vulkan 1.4 体素展厅路径追踪任务。答卷需在单窗口程序中实现硬件光线追踪、GPU wavefront 多反弹积分、自研 ReSTIR DI、自研 SVGF，以及平面镜与厚玻璃的分域历史处理。

## 题目设计

这个题目显著难于市面上的测评题目，并且工作量足够大更考验模型的长程工作能力。它主要是为了解决超大杯模型，比如 gpt-6-astra，和大杯模型，比如 gpt-6.1-sol，claude-opus-5.5 之间的 Benchmark 跑分差距过于微小而难以比较能力的问题，更大的模型和更高思考几乎不出现致命 bug，并且 bug 的危害程度小不影响大局，不过也会有显著性能问题；而小一号的模型和更低的思考就会出现更多错误和 bug。此题能难倒几乎所有的小杯模型（通常总参数量不超过 1T），无论其跑分多高，比如 deepseek-v4.1-flash-1017，即便是开 max 思考也常常出现编译不通过，致命 bug 和严重不符合任务规范的实现。

## 题面摘要

| 项目 | 内容 |
| --- | --- |
| 图形 API 与语言 | Vulkan 1.4、C++20、GLSL 460 |
| 场景 | 16×16 体素展厅；材质数值来自外部 `materials.json` |
| 必需链路 | Triangle BLAS/TLAS、RT pipeline、descriptor buffer、shader object、wavefront 队列、ReSTIR DI、SVGF、平面镜反射空间重投影、厚玻璃反射/透射分域 |
| 平台 | Windows 与 Linux，适用性以答卷的 `platforms.json` 为准 |
| 验收 | 构建、代码审查与真实交互运行综合判定；不设 FPS 或图像质量分数门槛 |

完整规格见[题面](https://github.com/yhcedpn/RenderArena/blob/main/Tasks/VoxelPathTracer_TASK.md)。

## 必需机制

| 机制 | 核验关注点 |
| --- | --- |
| 内部面消除与 greedy meshing | 暴露面范围、法线、UV、材质与玻璃边界保持正确 |
| 静态 BLAS compaction | 真实 query/copy、地址更新，稳态不重复构建 |
| GPU 活跃路径压缩与间接调度 | 每次反弹只处理活跃路径，计数、padding 与容量无漏算 |
| 发光面功率采样与 ReSTIR DI | proposal、target、复用归一化与当前 visibility 一致 |
| GGX VNDF 与 ray footprint mip | sampler/PDF 匹配，UV 和颜色空间不随合并失真 |
| 分域时间复用与 SVGF | 普通、反射和透射信号各自维护合法历史 |
| 持久资源与显式同步 | 稳态无重复创建，历史与在途资源无提前覆写 |

上述机制必须接入实际渲染路径。构建成功、截图相似或采用算法名称都不能替代对应实现。
