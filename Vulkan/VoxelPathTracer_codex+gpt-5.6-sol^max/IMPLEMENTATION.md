# VoxelPathTracer 实现说明

## 实现范围

程序使用 Vulkan 1.4、C++20 与 GLSL 460。窗口、输入、Surface 和程序目录由 SDL3 提供；Vulkan 调用由 volk 加载；VMA 使用从 volk 显式导入的完整函数表。运行时只从可执行文件目录读取 `materials.json` 与离线编译的 SPIR-V。

启动时分别检查 loader 与设备 API 版本，并逐项查询 Dynamic Rendering、Synchronization 2、Timeline Semaphore、Buffer Device Address、Scalar Block Layout、Acceleration Structure、RT Pipeline（含 indirect trace）、Descriptor Buffer、Shader Object 及显示所需动态状态。缺项报告具体 feature/extension 后退出，不建立替代渲染路径。

## 场景、材质与 AS

- 严格校验 `materials.json` 的必填字段、类型、数值范围、类型约束、唯一 ID 与场景引用。所有材质数值只来自该文件。
- `stone_noise` 使用四尺度确定性 Value 噪声；`wood_grain` 使用沿 U 的周期条纹和低频 Value 噪声扰动。基础色先在 sRGB 编码域调制，mip 在解码后的线性域平均再编码；粗糙度 mip 在线性标量域平均。
- 不透明格按题面顺序覆盖写入。表面提取先删除任意相邻不透明格之间的内部面，再按轴、方向、切片、行列的确定顺序执行 width-first greedy meshing。矩形 UV 直接取世界平面坐标，因此每世界单位重复一次，不随合并拉伸。
- 玻璃不进入整数格邻接：单独生成边界为 `[7,9] × [0.125,2.125] × [7,9]` 的六面闭合体，没有内部界面。所有三角形保留稳定的 surface ID 和 triangle ID。
- 几何按材质分组为 Triangle BLAS。每个静态 BLAS 以 `ALLOW_COMPACTION` 构建，查询真实 compacted size，复制到压缩资源；随后以压缩后地址生成 TLAS instance 并构建 TLAS。旧 BLAS 仅在 compact copy/TLAS 构建完成后释放，稳态不重建。
- light table 只收录最终网格中暴露的 emissive 三角形，CDF 权重为三角形面积乘 emission 亮度。

## Descriptor、pipeline 与 shader

所有 shader 资源位于一个带 `DESCRIPTOR_BUFFER` 标志的 set layout。运行时查询 set 大小、每个 binding offset、各类 descriptor size 与 buffer offset alignment，并由 `vkGetDescriptorEXT` 写入同时具有 resource/sampler descriptor usage 的持久映射 buffer。RT pipeline 使用 `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`；compute、全屏 vertex/fragment 使用 `VkShaderEXT`，没有 descriptor pool、descriptor set、compute/graphics pipeline、render pass 或 framebuffer。

RT pipeline 的递归深度为 1，SBT 分为 3 个分别满足 base alignment 的 raygen 记录、2 个 miss 记录和 radiance/shadow 两个 hit 记录。主可见性、续射线和 shadow 都走 Triangle TLAS；shadow 使用独立任务和独立 trace，不在 closest-hit 内递归。

## 每帧实际 pass 顺序

1. 清空本帧瞬态 buffer，写入 primary 数量，直接执行 `primary.rgen`。
2. `shade.comp` 处理第 1 次表面事件；GPU append 到下一紧凑队列和 shadow 队列；`make_indirect.comp` 生成真实 `VkTraceRaysIndirectCommandKHR` 与 compute indirect 参数。
3. 独立 shadow indirect trace，`shadow_resolve.comp` 在可见时写入对应 branch slot。
4. 对后续 segment 重复 8 次：continuation indirect trace → 紧凑 shading/append → GPU 参数生成 → shadow trace/resolve。其中前 7 次允许形成第 2～8 次散射，最后一次只接收预算后的 emissive/miss，不产生第 9 次散射。
5. `resolve_branches.comp` 将每像素至多两个独立分支归并为六个逻辑信号域和解析 emission。
6. ReSTIR DI：4 个新候选 initial → temporal → 4 邻居 spatial（半径不超过 8）→ 当前 shading point 的真实 shadow trace → resolve。
7. SVGF：分域时间重投影、矩和短历史局部方差 → 5 次 à-trous（步长 1/2/4/8/16，kernel `[1,4,6,4,1]/16`）→ demodulation 重建与域合成。
8. 写入本帧 guide/reservoir history；全屏三角形读取 HDR 合成结果，执行固定 Reinhard `c/(1+c)`，向 sRGB swapchain attachment 写线性 LDR；Dynamic Rendering 结束后转换到 present。

## 路径、玻璃与采样

- 每像素、帧、采样维度和 branch 构造独立 PCG 哈希随机数；主射线带子像素 jitter。主射线从 0.05 世界单位的近裁平面位置出发，而不是统一使用 `tMin=0.05`。
- diffuse 使用 Lambert 与余弦半球采样；rough metal 使用 GGX、height-correlated Smith、Fresnel-Schlick 与 VNDF 采样；mirror 为离散理想反射；dielectric 使用精确非偏振 Fresnel、Snell、TIR 和 radiance transport 的 eta² 因子。
- 每条路径最多 8 次散射；第 4 次起以 throughput 最大分量在 `[0.05,0.95]` 内执行带补偿 Russian Roulette。环境为黑色。
- 首次玻璃界面在非 TIR 时确定性产生 reflection branch 0 和 transmission branch 1；之前的 branch 0 radiance 不复制到 branch 1。之后的玻璃界面按 Fresnel 概率选择并补偿，不再扩展分支。介质状态随几何朝向和当前 inside 状态切换，玻璃段按真实 hit distance 乘 `exp(-absorption*d)`；主相机位于玻璃边界内时直接初始化 inside 状态。
- 路径队列 A/B、hit 和 shadow 容量均覆盖 `2 × framebufferPixelCount`；每像素两个 branch output slot 无并发写冲突。每次 shading 只 append 活跃路径，间接维度由 GPU counter 生成，不做 CPU readback。
- ray cone 从像素角 footprint 初始化，传播距离扩大 cone，粗糙反射扩大 spread；基础色和粗糙度用 `textureLod` 显式选择 trilinear mip。

## 直接光与 ReSTIR 归一化

直接可见的普通 primary 非 delta 表面使用 ReSTIR DI。更深非 delta 顶点及 delta 链终端使用发光三角形 NEE，并把离散 light selection、面积 PDF、距离平方/发光端余弦的方向 measure 转换与 BSDF PDF 进行 power-heuristic MIS。ReSTIR primary continuation 的下一段直接 emitter hit 被排除；若先经过 delta 事件则恢复 emitter contribution。

Initial reservoir 对 4 个候选使用 `w = target / proposalAreaPdf`。复用阶段不是累加亮度或简单累加 M：对每个 source representative `y_i`，在所有参与 source surface 上重新计算未遮挡 target，并使用

`denominator(y_i) = Σ_j M_j · target_j(y_i) / Z_j`

`candidateWeight_i = target_current(y_i) · M_i / denominator(y_i)`

其中 `Z_j = W_j · target_j(selected_j)`。这相当于按各 source 的重建 target distribution 做 balance/support-domain correction；不支持该 light sample 的 source target 为零。输出 estimator multiplier 为 `Σ candidateWeight / target_current(selected)`。历史 M 在使用前截到 32，并按相同比例缩减记录的 raw weight sum，保持已有 multiplier。Spatial 只读取完成的 temporal buffer，输出到独立 buffer。最终选中样本始终重新 trace 当前 visibility。

## guide、SVGF 与历史规则

- 普通 diffuse/specular、纯反射终端 diffuse/specular、首次玻璃 reflection、首次玻璃 transmission 共六个域；直接可见 emission/黑环境单独解析合成。
- guide 保存终端 surface/material、世界位置/深度、几何法线、roughness、albedo、链 hash、链长、终端类型和 transmission 标志。每次 mirror/glass delta 事件更新链 hash。
- 纯反射链在 path state 内累计按遇到顺序组合的平面反射仿射变换；终端位置/法线经逆序等价组合得到虚拟空间位置/法线，再用前一帧无 jitter 相机投影。历史同时核对链身份、终端身份、虚拟位置与法线。
- transmission 域在无 jitter 相机移动时整域拒绝时间历史；相机静止时按同像素加前后 jitter 补偿，并核对 entry/exit 所形成的链 hash、终端和介质标志。普通/纯反射/ReSTIR 历史不会因此全局清空。
- 时间累积保存未经过 à-trous 的 temporal mean、一/二阶亮度矩和最长 32 帧 history length；新样本权重不小于 1/32。少于 4 个样本时以兼容 guide 的 3×3 当前邻域估计局部矩。每轮 à-trous 使用独立 ping/pong，并按归一化权重平方传播方差。
- camera cut、R、extent 改变与资源重建同步使 guide、reservoir 和 temporal history 失效。只有已提交的帧推进 current/previous camera、jitter 和 history ping-pong 身份。

## 同步、WSI 与资源所有权

- staging host write → copy → shader/AS build 使用 Synchronization 2 barrier；BLAS build → compact query/copy → TLAS build → trace 有显式依赖与初始化 fence。
- 每个 bounce 建立 RT hit write → compute shade、queue/counter write → indirect read/下一 trace、shadow result → resolve 的依赖；ReSTIR 和 SVGF 各 pass、每轮 à-trous 之间均为独立读写 buffer 并设置 shader barrier。
- 相邻成像帧通过 timeline semaphore 串行化 history write/read；两个 CPU frame slot 各自拥有 command pool、command buffer、fence 和 acquire binary semaphore。render-finished binary semaphore 按 swapchain image 持有，只有重新 acquire 对应 image 后才复用。
- 固定 extent 下，路径、shadow、reservoir、guide、temporal/filter、descriptor、command 与 AS 资源全部持久复用。仅初始化、交换链重建和退出使用 device/fence 等待；稳态没有 `vkDeviceWaitIdle`、`vkQueueWaitIdle` 或逐 bounce CPU 等待。

## 构建与运行核验

- 平台：Windows 11 x64。
- 工具：MSVC 19.51.36260、CMake 4.3.1-msvc1、shaderc/glslc 2026.4；shader 以 Vulkan 1.4 / SPIR-V 1.6 目标离线编译。
- `windows-msvc-debug`：配置和构建成功，C++ 与全部 stage shader 无 warning/error/diagnostic。实际启动并持续渲染；Debug core + synchronization validation 无 warning/error。实际执行 1280×720 → 960×540 resize、最小化/恢复、W 移动、R camera cut 与正常窗口关闭，退出码 0。
- `windows-msvc-release`：独立配置和构建成功，C++ 与全部 stage shader 无 warning/error/diagnostic。实际启动、持续渲染并正常关闭，退出码 0；未请求 validation layer/debug utils。
- Linux 合同代码已使用 SDL3、`std::filesystem` 和 Vulkan 跨平台 API，但本轮没有 Linux 工具链与 Linux 依赖安装树，因此未构建或运行 Linux。

## 未完成的人工目视核验

目标窗口截图尝试因前景切换失败而混入任务外界面，该图已删除，不作为画面核验证据。未目视确认初始构图、软阴影、间接彩色反弹、镜间反射、玻璃前静止收敛/移动拒史、穿入玻璃后的图像、轮廓漏色和最终 gamma；也未实际操作右键视角、Q/E、Shift、Esc。上述渲染与输入路径均已实现并通过构建及非目视运行，不能把这些结果表述为已经完成人工画质验收。

## 隔离偏离记录

收尾核验使用桌面区域捕获尝试获取程序窗口，但前景切换未成功，捕获并查看的图像混入任务外界面。虽然未主动访问其他答卷源码或外部算法资料，这次图像访问仍超出本轮允许的任务材料范围，因此该轮不计入正式严格隔离比较。
