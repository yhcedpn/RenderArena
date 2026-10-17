# VoxelPathTracer 实现说明

## 工程入口

构建步骤见 [Wiki 构建指南](../../wiki/Home.md#构建)，程序路径与资源部署映射见 [AGENTS.md“构建与运行入口”](AGENTS.md#构建与运行入口)。功能要求与验收标准见 [VoxelPathTracer_TASK.md](VoxelPathTracer_TASK.md)。

## 渲染流程

一帧的 pass 顺序（全部经 push constant + descriptor buffer 绑定）：

1. `setup.comp`：按像素生成主路径（针孔相机、亚像素 jitter、玻璃体内初始化介质），写入路径队列 A。
2. `prepare.comp`：由 `pathCount[queue]`、`shadowCount` 生成 trace/dispatch 的 indirect 参数；同时清零**另一个**路径队列计数，避免波前计数跨反弹累积，并清零 shadow 任务计数。
3. RT `trace.rgen/rchit/rmiss`：主射线直接 dispatch；记录命中 t、instance、primitive。
4. `shade.comp`：主表面着色；非 delta 主表面写 `Primary`，delta 表面继续反射/折射路径；玻璃首次命中做确定性 Fresnel 拆分。
5. `restir_initial/temporal/spatial/shadow`：生成 4 个初始候选，合并时间历史，再从当前 reservoir 与 4 个空间邻居进行复用；`restir_shadow` 生成带权重的 shadow 任务。
6. `prepare` → RT shadow → `shadow_resolve`：生成间接参数，查询 ReSTIR 与路径 NEE 任务的可见性，结果写回 `restirOut` 或 `branchContrib`。
7. 对 8 次反弹循环：RT trace → `shade` → `prepare`（下一队列）→ shadow trace/resolve。队列 A/B 双缓冲，每次反弹的 continuation 写入另一队列。
8. `resolve.comp`：按 6 个信号域汇总 `branchContrib` 与 `restirOut`，写入 `domainRadiance` / `domainGuide`。光学 guide 在 `shade.comp` 中生成：纯反射链使用终端的虚拟位置/法线，含透射的链保留终端位置/法线。
9. 对 6 个域依次：`svgf_temporal` → `svgf_variance` → 5 次 à-trous（步长 1/2/4/8/16，核 `[1,4,6,4,1]/16`）→ `svgf_finalize`。
10. `composite.comp`：analytic 主发光 + 6 个域的 filtered radiance，写 composite SSBO；`display.vert/frag` 做 Reinhard `c/(1+c)` 并输出到 sRGB swapchain。

### 队列/容量

- 路径队列 A/B 各 `2 * N` 条（`N = width * height`）；首次玻璃命中拆成反射与透射两条分支，后续每条路径只产生一个 continuation。
- shadow 队列 `3 * N` 条任务，容纳最多 `2 * N` 条路径 NEE 和 `N` 条主表面 ReSTIR 可见性任务。
- 单条路径最多 8 次散射；反射链平面最多 8 个，每个平面索引占 8 bit。
- 路径容量溢出计入 `Counters.overflow`。

### ReSTIR 归一化

初始 reservoir 的 `stats.x = (1/M) Σ target/q`，其中 target 是未计可见性的直接光贡献亮度，`q` 是光源选择概率与面上采样概率密度相乘得到的面积 proposal PDF；`stats.z` 是所选样本的 target。最终贡献为：

```glsl
vec3 weight = contrib * (r.stats.x / max(r.stats.z, 1e-6));
```

temporal/spatial 合并时按当前表面的 target 重缩放来源权重，`stats.z` 保存所选样本在当前表面的 target。时间复用把历史 reservoir 的有效样本数 `M` 截断到 32 后再与当前候选合并。

### SVGF 域与历史

- 6 个域：diffuse、specular、mirror-diffuse、mirror-specular、glass-reflect、glass-transmit。
- 每个域、每个 parity（帧号 & 1）独立保存 `domainGuide`、`domainM1/M2`、`domainHist`。
- 普通域用“上一帧无 jitter 投影 + 两帧 jitter 补偿”重投影；光学域额外要求 `chainHash`、`terminalId` 一致；glass-transmit 在相机移动时拒绝历史。
- 时间历史长度从 1 开始，上限 32；历史索引使用**重投影后的像素**而不是当前像素。
- 方差 = `max(m2 - m1², 0)`；`history < 4` 时使用 3×3 局部矩。
- finalize 对 diffuse/specular 乘回主表面 albedo；光学域直接输出滤波后辐射度。

## 资源与同步

- 单一 descriptor set layout（36 个 binding），描述符写入 descriptor buffer；compute / graphics / ray tracing 三种 binding point 都设置 offset 0，不分配普通 descriptor set。
- 单一共享 descriptor buffer（`slots_[0]`）：相机 UBO 每帧在等待上一帧 timeline 后由 CPU 更新；所有 pass 绑定同一 descriptor buffer。
- 路径/命中/域缓冲按 `N`、`2N`、`6N`、`12N` 分配；每帧开始清空 counters、branchContrib、domainRadiance、analytic、restirOut。
- swapchain 重建时销毁并重建尺寸相关资源；`createDescriptors(false)` 只重写对应缓冲区的描述符，保留纹理、相机与 TLAS 描述符。
- 纹理为程序化生成的 2D array + 完整 mip 链（`R8G8B8A8_SRGB` / `R8_UNORM`），上传与 AS build 的 transient submit 均包含 command buffer 并以 fence 等待。

## 核验记录

以下结果来自[提交 `9ffc672` 的交付记录](https://github.com/yhcedpn/RenderArena/blob/9ffc672fb416066ef385a17e4cc5e56cb610561d/Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017%5Emax/IMPLEMENTATION.md)，适用于该版本答卷。

记录环境为 Windows、AMD Radeon 780M / AMDVLK 1.4.349；工具版本记录为 Visual Studio 2026 / MSVC 14.51.36231（v145）、CMake 4.3.1-msvc1、Vulkan SDK 1.4.357.0（glslc shaderc v2026.3）。

| 检查 | 原交付记录 |
| --- | --- |
| Windows Debug/Release 构建 | 两种配置均构建成功，0 warning / 0 error。 |
| Windows Debug 运行 | 连续渲染多帧，core / synchronization validation 无诊断；截图记录了墙面、地板、阶梯、柱体、发光块、镜面和玻璃体。 |
| Windows Release 运行 | 未启用校验层时，在 `createDescriptors` 调用 `vkGetDescriptorEXT` 写 binding 0 的 combined image sampler 描述符期间发生访问冲突 `0xC0000005`，运行验收未通过。崩溃原因未确定。 |
| 交互 | 原记录未提供 R 重置、鼠标捕获、缩放与最小化恢复、横向移动观察遮挡的逐项核验结果。 |
| Linux 构建与运行 | 原交付记录未提供核验结果。 |

## 源码导航

- `main.cpp`、`src/app.*`：SDL3 窗口、输入、标题 FPS。
- `src/vk_context.*`：Vulkan 1.4、volk、VMA、能力检查。
- `src/renderer.*`、`src/renderer_frame.*`：swapchain、descriptor buffer、shader object、RT pipeline/SBT、BLAS/TLAS、帧记录。
- `src/scene.*`：严格 JSON 校验、程序化纹理/mip、体素、内面消除、greedy meshing、按材质 BLAS 分组、光源表。
- `src/shared.hpp`：CPU/GLSL 共享的结构体与绑定索引。
- `shaders/*`：各 pass 的 GLSL 460。
