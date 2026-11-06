# VoxelPathTracer 实现说明

## 工程与资源

- C++20 主程序使用 SDL3 创建 Vulkan 窗口、处理输入并从可执行文件目录定位 `materials.json` 与离线 SPIR-V；Vulkan 调用全部经 volk 加载。
- 启动时检查 Vulkan 1.4、统一 graphics/compute/present 队列，以及 Dynamic Rendering、Synchronization 2、Timeline Semaphore、Buffer Device Address、Scalar Block Layout、RT Pipeline、AS、Descriptor Buffer、Shader Object 和实际使用的动态状态 feature。能力不足会列出具体缺项并退出。
- Debug 构建启用 `VK_LAYER_KHRONOS_validation` 的 core validation 和 synchronization validation；Release 不请求 validation layer、debug utils 或 validation features。
- `materials.json` 是所有材质数值的唯一来源。加载器验证根字段、全部材质字段、有限性、范围、唯一 ID 和类型语义，不提供缺省材质。场景引用的 8 个 ID 也逐项解析。
- CPU 按配置种子生成 solid、三尺度 Value noise 和受低频噪声扰动的 wood grain。基础色在 sRGB 编码域调制；其 mip 在解码到线性空间后平均并重新编码，粗糙度 mip 在线性标量域平均。GPU 使用 sRGB array texture、线性粗糙度 array texture及 linear/trilinear/repeat sampler；RT 路径维护 ray cone，并以 `textureLod` 显式选择 mip。

## 场景、几何与加速结构

- 按题面顺序填充 16×8×16 的不透明体素区，后写入覆盖先前材质；正 Z 入口、顶板开口、两组镜面、柱、阶梯、粗糙金属和 4 个发光块均由同一体素表生成。
- 每个轴向平面先比较相邻占用并删除内部面，再对相同材质、方向和 UV 规则的面掩码执行确定性 greedy meshing。矩形 UV 跨度等于世界单位长度，因此合并面仍每单位重复一次纹理；每个矩形获得由平面、方向、材质、起点和尺寸构成的稳定 surface ID。
- 玻璃不进入不透明体素邻接：它单独生成边界 `[7,9] × [0.125,2.125] × [7,9]` 的六个矩形面，因而没有内部界面，也不会删除地板顶面。
- 最终三角形按材质形成多个 BLAS。所有静态 BLAS 使用 `ALLOW_COMPACTION`，通过 compacted-size query 取得真实尺寸、执行 `vkCmdCopyAccelerationStructureKHR(COMPACT)`，等待完成后释放未压缩 AS；TLAS 只引用压缩后地址。实例禁用三角形面剔除，玻璃入射面和出射面都可命中。稳态不重建 AS。
- 发光面表只从最终保留下来的 emissive 三角形生成，以 `triangle area × emission luminance` 建立功率 CDF；Triangle metadata 保存唯一 light index，删除的内部面不会成为光源。

## 实际 GPU pass 顺序

每个成功提交帧依次执行：

1. `clear`：清空本帧信号、分支槽、guide、reservoir 暂存和 GPU 计数。
2. `primary.rgen`：每像素生成一个带 Halton 子像素 jitter 的针孔主射线；近裁面按相机前方 0.05 平面换算为各射线的真实起点距离。
3. `guide_init`。
4. ReSTIR DI：`restir_initial` → `restir_temporal` → `restir_spatial` → `shadow.rgen` → `restir_resolve`。
5. 对 8 次表面事件依次执行 `reset_counts` → `shade` → `prepare_args` → 间接 shadow trace → `shadow_resolve` → 下一段间接 continuation trace；第 8 次散射后的存活路径再由 `terminal_resolve` 只解析 emissive/miss 或终端 guide，不执行第 9 次散射。
6. `signal_resolve`：把解析 emission、ReSTIR 直接光、公共前缀和两个独占分支结果分域合并。
7. SVGF：`svgf_temporal` → `svgf_variance` → 5 轮 `atrous`（步长 1、2、4、8、16）→ `compose`。
8. Shader Object vertex/fragment 全屏三角形，经 Dynamic Rendering 写入 sRGB swapchain；fragment 在所有域合成后执行固定 `c/(1+c)` Reinhard，attachment 完成 sRGB 编码。

RT Pipeline 的 `maxPipelineRayRecursionDepth=1`。初始 trace 使用 `vkCmdTraceRaysKHR`；后续路径与 shadow 均由 GPU 生成 `VkTraceRaysIndirectCommandKHR` 并使用 `vkCmdTraceRaysIndirectKHR`。compute 和最终 vertex/fragment 使用 `VkShaderEXT`，仅 RT 使用 `VkPipeline`。工程没有 render pass、framebuffer、descriptor pool、普通 descriptor set、普通 compute pipeline 或 graphics pipeline。

## Wavefront、材质与玻璃

- 活跃路径采用 A/B 双缓冲紧凑队列。`shade` 用 atomic append 只写存活路径，`prepare_args` 从真实计数生成下一 trace 和 compute dispatch 参数；零活跃时发射一个 invocation 并由 shader 边界检查提前退出。没有 CPU readback、每反弹 fence 或 idle。
- 路径容量为 `2 × pixelCount`：首次玻璃界面最多把唯一未拆分路径变成反射、透射两条，之后各分支只随机选择一个 Fresnel 事件，因此该容量覆盖理论上限。shadow task 同样按两倍像素分配。每条分支使用 `pixel × 2 + branch` 独占结果槽，最终 resolve 避免并发浮点累加竞态。
- 路径状态携带 pixel/sample/branch、ray、ray cone、throughput、radiance、事件数、介质、前一 BSDF PDF、光学 hash/长度、域和用于平面反射的 3×4 仿射变换。
- diffuse 使用 Lambert 与余弦半球采样；rough metal 使用 GGX NDF、height-correlated Smith、Fresnel-Schlick 和 Heitz visible-normal 采样；mirror 是离散理想反射；dielectric 使用精确非偏振 Fresnel、Snell/TIR 以及 radiance transport 的 eta²。
- 第一次到达玻璃时确定性生成 `F` 反射与 `(1-F)eta²` 透射分支；TIR 只生成反射。公共前缀单独保存，不随分支复制。后续玻璃界面按 Fresnel 概率选择并作概率补偿。介质内每段按实际 hit distance 施加 `exp(-absorption × distance)`；主相机是否位于玻璃盒内在 raygen 初始化。
- 预算内最多处理 8 次散射；第 4 次及以后以 throughput 最大分量构造 `[0.05,0.95]` 的 Russian Roulette，并对存活 throughput 除以概率。第 8 次散射后的最后一段仍可命中 emissive/miss 并完成贡献，但 `terminal_resolve` 不再采样 BSDF 或扩展队列。
- 普通后续非 delta 顶点从功率光表执行发光面 NEE，与 BSDF emitter hit 使用方向 measure 下的 power heuristic MIS；shadow ray 把玻璃当作遮挡。ReSTIR 主顶点由 reservoir estimator 独占，直接的下一段 emitter hit 不重复计能，经过 delta 后的 emitter hit保留。

## ReSTIR DI

- 范围仅是直接可见的非 delta 主表面。每像素生成 4 个新候选，保存 light triangle ID 和两个面积采样随机数；proposal 是离散光源概率乘三角形面积 PDF，target 是未乘 visibility 的 RGB 直接光贡献亮度。
- 初始 reservoir 使用 `target/proposal` weighted reservoir sampling，并保存 weight sum、M、最终 W 和所选样本 target。
- 时间阶段以无 jitter 相机投影加当前/前帧 jitter 补偿重投影，检查 surface/material、位置、法线和 roughness。前帧 M 在并入当前 4 个候选时缩至总计不超过 32，对应候选权重同步按有效 M 缩放。
- 空间阶段只读已完成的 temporal buffer，以随像素/帧旋转的偏移在 8 像素半径内取 4 个兼容邻居，输出到独立 history-write buffer。
- 复用候选权重为 `target_current(y) × W_source × M_source`。选定最终样本后，在每个参与 source guide 上重新检查该样本的 target 支持；只把支持该样本的 source multiplicity 纳入分母，使用 `W = weightSum / (M_support × target_current(selected))`。这是本实现的支持域校正，避免简单累加不支持该样本的 M。
- 最终样本在当前 shading point 发射真实硬件 shadow ray；只有当前 visibility 为真才把 RGB estimator 写入直接光信号。

## SVGF、镜面与历史

- 物理存储分为 5 个独立历史域：普通漫反射、普通粗糙高光、纯反射链、首次玻璃反射、发生透射的光学链；解析 emission/黑环境单独合成。一个样本的反射终端用 guide flag 区分 diffuse/specular，物理 buffer 可共享但历史身份不混用。
- Guide 保存虚拟/实际终端位置和深度、法线、roughness、surface/material、光学链 hash、链长、终端模型和 albedo。没有有效 guide 的 radiance 仍进入最终结果，只是不复用历史。
- 对纯平面反射链，路径逐面组合反射仿射变换；终端位置和法线经逆序等价的组合映射到虚拟空间，再用前一帧相机投影。链 hash、链长、终端 surface/material、虚拟位置/法线共同判定历史。
- 任何透射都会切到 transmission 域。相机无 jitter 状态改变时只拒绝该域时间历史；静止时使用同像素+jitter 补偿，并验证 entry/exit 形成的光学 hash、链长和终端 guide。普通和纯反射域不会因连续相机移动而全局清空。
- SVGF 时间阶段维护独立 RGB、亮度一/二阶矩和最长 32 帧的 history length；每帧新样本权重不小于 1/32。少于 4 帧时由 3×3 兼容 guide 邻域估计空间矩/方差。
- 5 轮 à-trous 使用 `[1,4,6,4,1]/16` 的二维可分离乘积，权重包含光学身份、世界位置/深度、法线、roughness 和方差归一化亮度差。ping-pong buffer 保证每轮只读上一轮；方差按归一化权重平方传播。历史只保存 temporal 输出和矩，不回馈最终强空间滤波结果。
- 漫反射信号先按非零 albedo 分量确定性 demodulate，滤波后再乘 albedo；零 albedo 分量贡献保持零。

## 描述符、同步与生命周期

- 36 个绑定统一放入 Descriptor Buffer；buffer descriptor 使用真实 device address/range，AS descriptor 使用 TLAS address，纹理使用 combined-image-sampler descriptor。布局大小、每个 binding offset、各 descriptor size 和对齐均由驱动查询；每个 frame slot 持有自己的 descriptor backing，避免 CPU 覆写在途描述符。
- 两个 CPU frame slot 各有 command pool、command buffer、acquire binary semaphore 和 descriptor buffer。相邻成像历史由同队列顺序及 timeline semaphore 显式串联；slot 回收也等待 timeline value。
- render-finished binary semaphore 按 swapchain image 分配，只在该 image 再次 acquire 后复用；不以 timeline semaphore代替 presentation engine 所有权。
- 所有 GPU pass 间使用 `vkCmdPipelineBarrier2`，提交使用 `vkQueueSubmit2`。代码中分别建立 upload→shader/AS、BLAS build→query/copy→TLAS、RT→compute、queue/args→indirect/RT、shadow→resolve、ReSTIR 三阶段、temporal→每轮 à-trous→display、跨帧 history、acquire→attachment 和 attachment→present 的依赖。Debug synchronization validation 实际发现并验证了 acquire 到布局转换的 stage 依赖。
- 固定 extent 下复用路径、shadow、reservoir、guide、history、à-trous、command 和 descriptor 资源。仅初始化、resize/recreate 和退出等待 device idle；普通帧与反弹不调用 queue/device idle。

## 相机与窗口

- 默认 1280×720、位置 `(8,2.5,20)`、朝向 `(8,2,6)`、垂直 FOV 60°。投影和所有 dispatch 使用 framebuffer 像素尺寸。
- WASD 水平移动，Q/E 竖直移动，基础速度 3 unit/s，Shift 为 3 倍；右键捕获相对鼠标，pitch 限制为 ±89°；失焦自动释放。R 恢复初始相机并触发全局 history cut；Esc 优先释放捕获，再次按下关闭。
- 零像素尺寸时停止 GPU 帧并继续事件循环，恢复或 extent 变化后重建 swapchain 及尺寸相关持久资源。标题约每 0.5 秒按成功 present 数和完整墙钟帧时间更新；暂停/重建后回到占位符重新统计。

## 实际核验

- 工具：CMake 4.3.1-msvc1；MSVC 19.51.36260；shaderc/glslc 2026.4、glslang 11.1.0。CMake 的 shader 构建固定传入 Vulkan 1.4 / SPIR-V 1.6 目标。
- Windows Debug：执行 `cmake --preset windows-msvc-debug` 与 `cmake --build --preset windows-msvc-debug` 成功；C++ 与 24 个独立 shader stage 无 warning/error/diagnostic。
- Windows Debug 运行：启用 core + synchronization validation，最终版本持续呈现 20 秒；标题更新到约 5.0 FPS / 200.4 ms/frame，stderr 无 validation warning/error，随后按本轮启动 PID 关闭。
- Windows Release：执行 `cmake --preset windows-msvc-release` 与 `cmake --build --preset windows-msvc-release` 成功；最终版本持续呈现 15 秒，标题更新到约 5.0 FPS / 198.4 ms/frame，stderr 为空，随后按本轮启动 PID 关闭。Release 代码路径不启用 validation/debug messenger。

## 未核验项

- Linux 合同已在 SDL3、filesystem、Vulkan API 和 shader 路径中保留，但本轮只具备并构建了 Windows 工具链，未执行 Linux 配置、构建或运行。
- 按组织者隔离要求，本会话没有调用桌面/截图工具，也没有进行人工目视画质判定。因此进入展厅、横移遮挡揭露、两个镜面、玻璃前静止/移动、穿入穿出玻璃、resize、最小化/恢复、R 和正常窗口关闭的完整交互序列未人工执行；相应事件与资源路径已实现，但不能把本轮自动持续呈现测试声称为这些交互已验收。
- 本轮实例由记录的 PID 关闭，而不是通过窗口关闭事件退出，因此“正常关闭”只经代码审查，未做真实窗口交互核验。
