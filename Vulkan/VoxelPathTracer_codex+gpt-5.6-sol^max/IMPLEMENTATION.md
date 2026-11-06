# VoxelPathTracer 实现说明

## 实现范围

本答卷实现了以 Vulkan 1.4 为入口的硬件光线追踪体素展厅。运行时只从可执行文件目录读取 `materials.json` 和离线编译的 SPIR-V；窗口、输入、Surface 与程序目录定位使用 SDL3，Vulkan 函数经 volk 加载。

主要源码职责如下：

- `src/materials.cpp`：严格校验全部材质字段、范围、类型约束和 ID 唯一性；生成确定性的 stone/wood 纹理及完整 mip 链。基础色 mip 在线性颜色空间平均后重新编码为 sRGB，粗糙度 mip 在线性标量域平均。
- `src/scene.cpp`：按题面顺序填充静态格子，删除相邻不透明体内部面，执行确定性 greedy meshing；玻璃直接生成平移后的 2×2×2 单一闭合边界。几何按材质分组，并仅从保留下来的 emissive 三角形建立面积×发射亮度功率 CDF。
- `src/vulkan_app.cpp`：Vulkan 能力合同、Descriptor Buffer、Shader Object、RT Pipeline、资源、AS、同步、交换链、输入和帧循环。
- `shaders/`：wavefront 路径积分、ReSTIR DI、shadow trace、SVGF、à-trous 与最终合成。

## Vulkan 资源与 AS

- 启动时检查 Vulkan 1.4，以及 Dynamic Rendering、Synchronization 2、Timeline Semaphore、Buffer Device Address、Scalar Block Layout、RT Pipeline/indirect trace、Acceleration Structure、Descriptor Buffer、Shader Object 和实际使用的动态状态特性；缺项时列出具体能力并退出。
- 所有 shader 资源统一来自一个 Descriptor Buffer。布局大小、binding offset、各种 descriptor size 和 offset alignment 都由驱动查询；描述符由 `vkGetDescriptorEXT` 写入，并用 `vkCmdBindDescriptorBuffersEXT`、`vkCmdSetDescriptorBufferOffsetsEXT` 绑定。不创建 descriptor pool/set。
- compute 与最终 vertex/fragment 使用 `VkShaderEXT`；RT 使用带 `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT` 的 `VkPipeline`，`maxPipelineRayRecursionDepth=1`。
- 每个非空材质组建立一个 Triangle BLAS。BLAS 使用 `ALLOW_COMPACTION`，在 GPU build 后写真实 compacted-size query，创建压缩目标并执行 `vkCmdCopyAccelerationStructureKHR(COMPACT)`；copy 完成后才释放旧 AS。TLAS 的 instance 引用压缩后地址，并关闭三角形 facing cull，使玻璃出射面可命中。
- 顶点、索引、三角形、材质、光源和纹理使用 staging upload，并建立 host write → transfer → shader/AS build 的 Sync2 依赖。

## 实际帧 pass 顺序

固定 framebuffer 尺寸下复用全部队列、reservoir、guide、SVGF、à-trous、command 和 descriptor backing：

1. `prepare.comp`：按 framebuffer 像素生成带逐帧 Halton 子像素 jitter 的主射线，初始化当前 parity 的两个 branch record；相机若位于玻璃 AABB 内则初始化介质状态。
2. `vkCmdTraceRaysKHR`：主可见性 Triangle TLAS trace。
3. `shade.comp`：处理第一表面，生成 primary guide、BSDF continuation 和首次玻璃分支；直接可见非 delta 主表面由 ReSTIR 独占直接光。
4. `restir_initial.comp`：每像素 4 个新发光三角形候选。
5. `restir_temporal.comp`：无 jitter 相机重投影、guide 验证与上一帧 reservoir 复用。
6. `restir_spatial.comp`：从 temporal 只读结果选择 4 个、半径不超过 8 像素的动态邻居。
7. `restir_visibility.comp` → shadow RT indirect → `resolve_shadow.comp`：对最终样本执行当前 shading point 的真实遮挡查询并写回分支辐亮度。
8. 对后续 8 个 ray segment 循环：`vkCmdTraceRaysIndirectKHR` → `shade.comp` → NEE shadow RT indirect → shadow resolve → GPU 生成下一次 trace/dispatch indirect 参数。第 8 次散射后仍 trace 一段以允许 emissive/miss 完成贡献，但不产生第 9 次散射。
9. `svgf_temporal.comp`：按信号域重投影、历史验证、亮度一/二阶矩和短历史局部方差。
10. `atrous.comp` 五轮：步长 1、2、4、8、16，使用 `[1,4,6,4,1]/16` 的二维乘积 kernel，并用平方归一化权重传播方差。
11. `display.frag`：重建 diffuse demodulation、合成解析 emission 与全部光学域、执行曝光 1 的 Reinhard tone mapping；Dynamic Rendering 写入 sRGB swapchain attachment。

## Wavefront、材质与玻璃

- 两个活跃路径队列均为 `2×pixelCount` 容量。每个主样本最多一次确定性玻璃拆分，因此任一阶段最多两个 branch；shadow 队列同样为 `2×pixelCount`。队列使用 GPU atomic append 形成连续紧凑结果，计数由 GPU 写入 RT 和 compute indirect 参数，不进行逐反弹 CPU readback。
- 路径状态保存 pixel/branch、ray、throughput、last BSDF PDF、散射次数、介质、信号域、光学链 hash 和反射空间 affine transform。每个 branch 使用独立 record，避免浮点原子归约 race。
- diffuse 使用余弦半球采样；rough metal 使用 GGX、height-correlated Smith、Fresnel-Schlick 和 VNDF 采样；mirror 为离散理想反射；dielectric 使用非偏振 Fresnel、Snell、TIR 和 radiance transport 的 eta²；emissive 仅外侧单面发射。
- ray cone 随传播距离和散射粗糙度扩展，RT shading 通过 `textureLod` 显式消费 footprint LOD。UV 直接来自世界平面坐标，greedy rectangle 不会拉伸每世界单位重复。
- 第 4 次散射起执行补偿后的 Russian Roulette，survival probability 为 throughput 最大分量并限制到 `[0.05,0.95]`。
- 首次玻璃界面确定性建立反射与透射 branch：已有贡献只保留在原 branch，未来 throughput 分别乘 Fresnel 权重与 `(1-F)·eta²`。后续界面按 Fresnel 随机选择并补偿，不再增加分支。玻璃内段按实际 hit distance 应用 Beer-Lambert 吸收；透射时切换介质状态。直线 shadow ray 将玻璃作为遮挡体。
- primary ReSTIR 顶点的下一段直接 emitter hit 通过 suppress 标志去重；若先经过 delta 事件则清除 suppress 并保留 emitter contribution。其他非 delta 顶点使用功率采样发光面 NEE，并以同一方向 measure 与 BSDF hit 做 power-heuristic MIS。

## ReSTIR DI 归一化

初始 reservoir 使用面积 measure：三角形离散选择 PDF 乘三角形面积倒数。候选 target 是未乘 visibility 的 RGB 直接光亮度，包含 BSDF、接收端余弦、发光端余弦和距离几何项；初始权重为 `target/proposalPdf`。

reuse 没有采用简单累加 M。每个 source 保存：

- 选中 light triangle 与 barycentric；
- `M`、weight sum；
- `Z_s = weightSum/M`；
- `W_s = weightSum/(M·target_s(selected))`。

对每个待组合 source 样本 `y_i`，在所有参与 source guide 上重新求值 `target_j(y_i)`，用

`m_i(y_i) = M_i·target_i(y_i)/Z_i ÷ Σ_j(M_j·target_j(y_i)/Z_j)`

执行支持域 generalized balance correction；目标像素的 reservoir candidate weight 为 `target_target(y_i)·W_i·m_i(y_i)`。选中后重新生成目标 normalization。历史 M 限制到 32 时，weight sum 按相同比例缩减，保持 `Z` 和 normalization 的意义。空间阶段只读完整 temporal buffer，最终 visibility 始终重新 trace。

## SVGF 与历史规则

物理存储采用每像素最多两个 branch record，但逻辑历史保持六个相互独立的可过滤域：普通 diffuse、普通 specular、反射 diffuse、反射 specular、玻璃反射、玻璃透射；直接可见 emission/黑色环境为独立解析域，不做空间模糊。

- 普通 guide 保存实际世界位置；纯反射链把每个反射平面复合为 affine transform，使终端位置/法线等价于按反射面逆序映射到虚拟空间，再用前一帧无 jitter 相机投影。
- 光学链 hash、链长、终端 surface/material、虚拟位置/法线、roughness 和 hit distance 共同参与历史验证。普通链使用稳定常量身份；反射/透射事件按 surface identity 更新 hash。
- 玻璃透射域在无 jitter 相机发生位置/朝向/FOV/projection 变化时整域拒绝时间历史；静止时使用 jitter 补偿后的同像素对应并验证 entry/exit 拓扑 hash。其他域不会因此全局 reset。
- camera cut、resize 和实际资源重建使全部历史失效；普通连续移动只做局部重投影拒绝。
- 时间历史上限 32，当前样本权重不小于 `1/32`。历史少于 4 时使用兼容 guide 的 3×3 当前邻域估计局部矩和方差。时间 history 保存 temporal 结果，不反馈五轮空间滤波结果。
- diffuse 仅在非零 albedo 分量上 demodulate；重建时再乘 albedo。零 albedo 分量保持零贡献。

## 同步与帧所有权

- 两个 CPU frame slot，各自拥有 command pool/buffer 和 acquire binary semaphore；slot 复用等待 timeline value。
- 成像历史只有一条有序链。每次 submit 显式等待上一成功提交的 timeline value，当前 parity 的 history write 完成后才允许下一帧读取。
- present binary semaphore 按 swapchain image 分配；只有重新 acquire 同一 image 后才复用，避免把 timeline completion 误当作 presentation engine 已释放 image。
- pass 间通过 `vkCmdPipelineBarrier2` 建立 RT write → compute、queue/parameter write → indirect read、shadow result → resolve、ReSTIR initial → temporal → spatial → visibility、temporal → 每轮 à-trous → fragment，以及 color attachment → present 的依赖。
- 稳态不调用 queue/device idle；初始化的 upload/AS build、swapchain 重建和最终退出使用允许的等待安全点。

## 实际构建与运行结果

- Windows Debug：已执行 `cmake --preset windows-msvc-debug` 和 `cmake --build --preset windows-msvc-debug`，C++ 与全部 GLSL shader 编译成功，无编译诊断。
- Debug 真实运行：启用 core validation 与 synchronization validation。修正 query reset、fragment readonly SSBO、acquire/layout-transition 依赖后，多次连续运行及正常关闭未出现 validation warning/error；validation INFO 不计为诊断失败。
- 实际观察到标题栏约每 0.5 秒更新 FPS 与 ms/frame；固定窗口下约为每秒数帧，统计是完整帧墙钟时间。
- 实际观察到静止后分域 temporal/SVGF 从高噪声逐步收敛；修正普通链错误使用 pixel hash 后，跨像素重投影建立了稳定历史。
- 已实际执行进入展厅、横移、玻璃前静止、向玻璃移动/进入、窗口 resize、最小化与恢复、R 重置和正常关闭。最小化恢复曾暴露 SDL restore 与 WSI 零 extent 的短暂竞态，现改为继续处理事件并延迟资源重建，复测通过且无 validation 诊断。

## 未完成或未核验

- Windows Release 未配置、未构建、未运行；Linux 未构建、未运行。
- `glslc` 的具体版本号未在停止核验前单独采集；只确认本轮离线编译实际成功且无 shader 编译诊断。
- 自动化环境的 Windows 前台锁阻止了可靠的相对鼠标注入，因此右键捕获后的 yaw/pitch、两个镜面区域的定向观察和镜间反射未完成可信的交互核验；相关实现只经过代码审查和普通 RT 画面运行。
- 未通过临时修改 `materials.json` 实测合法数值变化与各类非法配置；外部读取、严格字段/范围/语义校验和具体错误路径已实现。
- 未单独验证高 DPI 跨显示器移动、swapchain out-of-date/suboptimal 的每一种驱动触发方式；实际 framebuffer resize、最小化/恢复和资源重建已运行。
- 新增源码和 shader 尚未追加到现有 `.vcxproj/.filters` 的 VS 展示项；CMake glob 已将它们纳入实际配置、编译和运行路径。

## 隔离偏离记录

本轮为尝试控制本地窗口完成相对鼠标交互，读取了任务目录之外的 Codex `computer-use` 技能及其 guidance/API/confirmation 操作说明。该读取超出了本轮“显式读取仅限任务目录和本目录依赖公共头文件/说明”的严格隔离边界；未读取上级仓库源码、其他答卷、Git、历史实现、评审、历史截图或网络资料。组织者已决定本轮不计入正式严格隔离测评，并将另开独立会话重跑。
