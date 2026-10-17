# Voxel Path Tracer 实现说明

## 1. 构建与运行

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

- 可执行文件：`bin/windows-msvc-debug/VoxelPathTracer.exe`、`bin/windows-msvc-release/VoxelPathTracer.exe`。
- 运行资源：`shaders/*.spv` 与 `materials.json` 由 CMake 复制到可执行文件同目录。
- Debug 启用 core + synchronization validation；Release 不启用校验层。
- 依赖：SDL3、volk、Vulkan-Headers、nlohmann-json、VMA（VMA 通过 volk 函数表，关闭静态/动态加载）。

## 2. 渲染流程

一帧的 pass 顺序（全部经 push constant + descriptor buffer 绑定）：

1. `setup.comp`：按像素生成主路径（pinch-hole 相机、亚像素 jitter、玻璃体内初始化 medium），写入路径队列 A。
2. `prepare.comp`：由 `pathCount[queue]`、`shadowCount` 生成 trace/dispatch 的 indirect 参数；同时清零**另一个**路径队列计数，避免波前计数跨反弹累积，并清零 shadow 任务计数。
3. RT `trace.rgen/rchit/rmiss`：主射线直接 dispatch；记录命中 t、instance、primitive。
4. `shade.comp`：主表面着色；非 delta 主表面写 `Primary`，delta 表面继续反射/折射路径；玻璃首次命中做确定性 Fresnel 拆分。
5. `restir_initial/temporal/spatial/shadow`：自研 ReSTIR DI，4 个初始候选、4 个空间邻居 + 当前 reservoir（最多 5 个），历史按 32 帧上限；`restir_shadow` 生成带权重的 shadow 任务。
6. `shadow_resolve` + RT shadow：对 ReSTIR 与路径 NEE 的 shadow 任务做可见性积分，结果写回 `restirOut` 或 `branchContrib`。
7. 对 8 次反弹循环：RT trace → `shade` → `prepare`（下一队列）→ shadow trace/resolve。队列 A/B 双缓冲，每次反弹的 continuation 写入另一队列。
8. `resolve.comp`：按 6 个信号域汇总 `branchContrib` 与 `restirOut`，写入 `domainRadiance` / `domainGuide`；镜子与玻璃使用终端虚拟位置/法线。
9. 对 6 个域依次：`svgf_temporal` → `svgf_variance` → 5 次 à-trous（步长 1/2/4/8/16，核 `[1,4,6,4,1]/16`）→ `svgf_finalize`。
10. `composite.comp`：analytic 主发光 + 6 个域的 filtered radiance，写 composite SSBO；`display.vert/frag` 做 Reinhard `c/(1+c)` 并输出到 sRGB swapchain。

### 队列/容量

- 路径队列 A/B 各 `2 * N` 条（`N = width * height`）。
- shadow 队列表 `3 * N` 条任务。
- 单条路径最多 8 次散射；反射链平面最多 8 个（`uint8` 打包）；ReSTIR 候选/邻居/历史上限见上。
- 路径容量溢出计入 `Counters.overflow`。

### ReSTIR 归一化

`restir_*` 的 `stats.x` 是 **luminance target** 单位下的 `W = (1/M) Σ target/p̂`，`stats.z` 是所选样本的 target。最终贡献为：

```glsl
vec3 weight = contrib * (r.stats.x / max(r.stats.z, 1e-6));
```

等价于 `f_rgb / target_lum * W_lum`，避免直接使用 `f * W` 导致能量被 target 亮度缩放。temporal/spatial 合并时同样按 target 重缩放 W，并保持 `stats.z` 为所选样本 target。

### SVGF 域与历史

- 6 个域：diffuse、specular、mirror-diffuse、mirror-specular、glass-reflect、glass-transmit。
- 每个域、每个 parity（帧号 & 1）独立保存 `domainGuide`、`domainM1/M2`、`domainHist`。
- 普通域用“上一帧无 jitter 投影 + 两帧 jitter 补偿”重投影；光学域额外要求 `chainHash`、`terminalId` 一致；glass-transmit 在相机移动时拒绝历史。
- 时间历史长度从 1 开始，上限 32；历史索引使用**重投影后的像素**而不是当前像素。
- 方差 = `max(m2 - m1², 0)`；`history < 4` 时使用 3×3 局部矩。
- finalize 对 diffuse/specular 乘回主表面 albedo；光学域直接输出滤波后辐射度。

## 3. 资源与同步约定

- 单一 descriptor set（36 个 binding），经 descriptor buffer 绑定；compute / graphics / ray tracing 三种 binding point 都设置 offset 0。
- 单一共享 descriptor buffer（`slots_[0]`）：相机 UBO 每帧在等待上一帧 timeline 后由 CPU 更新；所有 pass 绑定同一 descriptor buffer。
- 路径/命中/域缓冲按 `N`、`2N`、`6N`、`12N` 分配；每帧开始清空 counters、branchContrib、domainRadiance、analytic、restirOut。
- swapchain 重建时销毁并重建 sized resources；descriptor 只重写 sized bindings（图像/相机/TLAS 描述符保持），避免 AMDVLK 在重复写 combined image sampler 描述符时崩溃。
- 纹理为程序化生成的 2D array + 完整 mip 链（`R8G8B8A8_SRGB` / `R8_UNORM`），上传与 AS build 的 transient submit 均包含 command buffer 并以 fence 等待。

## 4. 已知驱动限制（必须记录）

本机为 **AMD Radeon 780M + AMDVLK 1.4.349**。在**不加载校验层**的 Release 进程里，调用 `vkGetDescriptorEXT` 写 **combined image sampler** 描述符（binding 0）会在 AMDVLK 内部触发访问冲突（0xC0000005）；该调用发生在 `createDescriptors`，与 shader/资源内容无关。加载 Khronos validation 层时同一份代码可稳定运行（已连续运行并截图）。

因此：

- **Debug（validation 开启）**：初始化、渲染与校验通过；validation 无 WARNING/ERROR。
- **Release**：编译通过，但独立运行时在该驱动上于 `createDescriptors` 崩溃；这是驱动/描述符缓冲扩展的运行时问题，未在本环境中绕过。若换用其他 Vulkan 驱动或加载校验层，Release 路径应可运行。
- 曾尝试把纹理改为 storage buffer + 手写 mip 采样以绕开该驱动崩溃；Release 仍在该驱动上出现 device lost，故未保留该方案。

## 5. 已核验 / 未核验

已核验：

- Debug/Release 均以 `cmake --preset` + `cmake --build --preset` 构建成功，0 warning/0 error。
- Debug 运行多帧，validation core + sync 无消息；截图显示：红/蓝墙、石地板、木阶梯、金属柱、4 个发光块、镜面与玻璃体、SVGF 去噪后的直接/间接光。
- 初始视角、镜面/玻璃的暗部现象与场景几何一致：中央镜面朝向开放的前方（暗），玻璃后方处于镜墙阴影区。
- `R`/右键鼠标捕获、缩放/最小化恢复、进入展厅后的横向遮挡等交互**未逐项截图核验**；Release 运行时受上述驱动问题阻塞。

## 6. 文件

- `main.cpp`、`src/app.*`：SDL3 窗口、输入、标题 FPS。
- `src/vk_context.*`：Vulkan 1.4、volk、VMA、能力检查。
- `src/renderer.*`、`src/renderer_frame.*`：swapchain、descriptor buffer、shader object、RT pipeline/SBT、BLAS/TLAS、帧记录。
- `src/scene.*`：严格 JSON 校验、程序化纹理/mip、体素、内面消除、greedy meshing、按材质 BLAS 分组、光源表。
- `src/shared.hpp`：CPU/GLSL 共享的结构体与绑定索引。
- `shaders/*`：各 pass 的 GLSL 460。
