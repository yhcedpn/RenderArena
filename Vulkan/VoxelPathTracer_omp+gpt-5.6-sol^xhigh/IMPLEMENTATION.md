# VoxelPathTracer 实现说明

## 实现范围

程序使用 SDL3 创建单窗口并定位可执行文件目录，使用 volk 加载 Vulkan 1.4。运行时严格读取同目录的 `materials.json`，CPU 生成确定性的 sRGB 基础色纹理、线性粗糙度纹理及完整 mip 链；基础色 mip 在线性空间降采样。材质、场景引用和数值约束均在创建 Vulkan 资源前校验，不提供内置材质回退。

静态场景先按题面顺序写入 16×8×16 格子，再按三个轴的正反方向提取暴露面。每个切片掩码按材质和法线执行确定性 greedy meshing，UV 直接取世界坐标以保持每单位重复。玻璃单独生成为 `[7,9] × [0.125,2.125] × [7,9]` 的六面闭合体。暴露 emissive 三角形按面积乘发射亮度建立唯一 CDF。

所有三角形进入一个静态空间 BLAS。BLAS 使用 `ALLOW_COMPACTION` 构建，查询真实 compact size 后执行 `vkCmdCopyAccelerationStructureKHR`，销毁旧 AS，再以压缩后地址构建 TLAS。实例关闭三角形朝向剔除，使玻璃入射面和出射面都可命中。

## GPU pass 顺序

每个已提交帧的顺序如下：

1. `primary.comp`：清空两个分支输出和本帧 primary surface，按 framebuffer 实际尺寸生成带 jitter 的主射线及初始介质状态。
2. `surface.rgen/rchit/rmiss`：第 0 次使用 `vkCmdTraceRaysKHR`；后续反弹使用 GPU 写出的 `VkTraceRaysIndirectCommandKHR` 和 `vkCmdTraceRaysIndirectKHR`。
3. `shade.comp`：消费命中事实，执行材质采样、NEE、Fresnel 拆分、throughput/PDF 更新、Russian Roulette，并以原子 append 写入另一侧紧凑路径队列和独立 shadow 队列。
4. `prepare_indirect.comp`：从真实计数生成二维 ray dispatch 和二维 compute dispatch 参数；零任务使用单 invocation 并由 shader early-out。
5. `shadow.rgen/rchit/rmiss`、`shadow_resolve.comp`：独立追踪当前反弹的遮挡任务，并归约到像素/分支的唯一 slot。步骤 2–5 最多推进 8 次散射。
6. `restir_initial.comp` → `restir_temporal.comp` → `restir_spatial.comp`：生成候选、时间复用和空间复用；最终样本再次进入独立 shadow trace，当前 visibility 通过后才进入直接光信号。
7. `resolve.comp`：把两个玻璃分支的本帧信号归约到只读 raw signal。
8. `svgf_temporal.comp`：按信号 guide 重投影，更新一阶矩、二阶矩和 history length；短历史使用 guide 兼容的 3×3 当前样本估计空间方差。
9. `svgf_atrous.comp`：ping-pong 执行 5 轮，步长为 1、2、4、8、16，使用 `[1,4,6,4,1]/16` 的二维可分离乘积权重，并用平方归一化权重传播方差。
10. Shader Object vertex/fragment 全屏三角形：重调制漫反射，合成所有光学域及解析 emission，执行固定 `c/(1+c)` Reinhard 映射；sRGB attachment 完成唯一一次编码。

RT pipeline 的 `maxPipelineRayRecursionDepth` 为 1。compute 和显示阶段均通过 `VkShaderEXT`/`vkCmdBindShadersEXT` 执行；仅 RT 使用 `VkPipeline`。资源绑定只使用 Descriptor Buffer，不创建 descriptor pool、descriptor set、render pass 或 framebuffer。

## 队列、分支与计能

设像素数为 `P`。每个主样本仅在首次玻璃界面确定性拆分，之后按 Fresnel 概率选择，因此双缓冲路径队列、shadow 队列和 branch output 容量均为 `2P`。每个活跃分支在一个反弹最多产生一个常规 shadow task；ReSTIR 开始前会清零并重新使用 shadow 队列，所以 `2P` 同样覆盖该阶段。计数超过数学容量时设置可见的 magenta 错误输出，不静默丢弃或覆写。

首次玻璃拆分前的贡献只写 branch 0；反射继续使用 branch 0，透射使用 branch 1。两条路径分别保存 throughput、介质、散射计数、随机维度、重复计能状态、光学链哈希及反射空间仿射变换。玻璃介质段按真实命中距离施加 Beer–Lambert 吸收。镜面和玻璃事件都计入 8 次预算；第 4 次散射后执行有补偿的 Russian Roulette。

普通 primary 非 delta 顶点由 ReSTIR estimator 独占直接光；该顶点的下一段普通 emitter hit 被抑制，经过后续 delta 事件的 emitter hit仍保留。常规 NEE 使用面积 proposal，经距离平方和发光端余弦转换到方向 measure 后与 BSDF PDF 做 power heuristic MIS。GGX 使用 VNDF 采样及相匹配的 visible-normal PDF。

## ReSTIR DI 归一化

每个有效 primary 非 delta 表面产生 4 个新候选。候选保存 light triangle ID 和三角形参数，目标函数为未乘 visibility 的 RGB 直接光亮度，包含 BSDF、接收余弦、发光余弦和距离几何项；初始权重为 `target / areaProposalPdf`。

时间复用把历史 multiplicity 限制到 32。限制时按相同比例缩小它代表的 weight sum，因此代表样本的 `W = weightSum / (M × selectedTarget)` 不变。空间阶段从已完成且只读的 temporal buffer 取得自身和 4 个随像素/帧变化、半径不超过 8 的邻居。对于 source `i` 的代表样本 `y`，使用支持域校正

`c_i(y) = M_i target_i(y) / Σ_j(M_j target_j(y))`

并以 `target_current(y) × W_i × M_i × c_i(y)` 参与目标像素的 reservoir sampling。输出仍以总 weight sum、总 multiplicity 和目标像素下 selected target 得到最终 `W`。最后只对选中样本执行当前帧 RT shadow visibility。

## SVGF 与历史规则

物理存储使用四个域，同时通过 terminal type、材质、surface、chain hash、chain length 和 guide 身份保持题面要求的逻辑语义：

- 普通漫反射；
- 普通粗糙金属高光；
- 纯镜面或首次玻璃反射（guide 保留终端漫反射/高光/发光类型）；
- 发生过玻璃透射的信号。

直接看到的 emission 单独解析合成，不进入空间滤波。漫反射域按实际终端 albedo 逐分量 demodulate；零 albedo 分量确定为零，最终再重调制。

普通表面以世界位置投影到上一无 jitter 相机，并按上一帧 jitter 修正 raster lookup。纯反射链在路径推进时累计平面反射仿射变换，终端位置和法线先变换到虚拟空间，再投影到上一相机；链中每个 surface 都进入稳定哈希。透射域在相机位置、朝向或投影变化时仅拒绝透射时间历史；相机静止时使用同像素 jitter 补偿并验证 entry/exit 链、终端及 guide。camera cut、resize 和资源重建使全部相关历史失效。

时间历史只保存 temporal radiance、矩和 guide，不回灌 5 轮 à-trous 结果。历史上限为 32，当前样本权重不低于 1/32。

## 同步与所有权

- 初始化上传使用 staging，显式建立 transfer write → shader/AS read。
- BLAS build → compact query/copy → TLAS build → trace 使用 Synchronization 2 barrier 和初始化 fence。
- 每次反弹建立 RT write → compute read、queue/counter write → indirect/RT read、shadow result → resolve 的依赖。
- ReSTIR initial、temporal、spatial、visibility 逐阶段使用只读输入和独立输出区域。
- SVGF temporal 与每轮 à-trous 之间使用 shader write/read barrier。
- 相邻成像历史通过 timeline semaphore 值串联；两个 CPU frame slot 各有 command pool、fence 和 acquire binary semaphore。
- present binary semaphore 按 swapchain image 分配；重新 acquire 同一图像后才重用，frame slot 和交换链图像身份分离。
- acquire wait、动态渲染 attachment 转换和 present 转换均使用 Synchronization 2。

固定 extent 下复用所有队列、reservoir、history、guide、descriptor backing、command 和 shader/AS 资源。仅初始化、交换链重建和退出使用等待安全点。

## 实际核验

- 平台：Windows 11 x64。
- 工具：CMake 4.3.1-msvc1；shaderc/glslc 2026.3、SPIRV-Tools 2026.3；构建使用任务预设提供的 MSVC 工具链。
- GPU/驱动能力路径：AMD Radeon 780M Graphics，Vulkan 1.4；启动时逐项通过 features/properties 和格式能力查询。
- Debug：`cmake --preset windows-msvc-debug`、`cmake --build --preset windows-msvc-debug` 成功；GLSL 460 离线编译到 Vulkan 1.4 / SPIR-V 1.6，无 shader 编译诊断。
- Debug 运行：实际窗口和完整 GPU frame loop 持续运行 30 秒；启用 core validation 与 synchronization validation，未产生 warning/error/diagnostic。
- Release：`cmake --preset windows-msvc-release`、`cmake --build --preset windows-msvc-release` 成功；Release 不请求 validation layer/debug utils。实际窗口曾持续运行 30 秒。

Linux 未在本机构建。自动运行环境未执行鼠标/键盘注入，因此 WASD/QE、右键捕获、R、Esc、缩放/最小化/恢复和进入玻璃等人工交互尚未逐项操作核验；对应事件、重建和历史失效路径已接入主循环。
