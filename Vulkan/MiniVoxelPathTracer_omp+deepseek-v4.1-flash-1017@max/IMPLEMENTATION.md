# MiniVoxelPathTracer 实现说明

## 1. Pass 顺序与实际调用路径

单窗口、单 Vulkan 队列、硬件 RT。每帧录制顺序固定为：

1. `primary.comp`：按像素生成主射线，写入 jitter、双缓冲路径队列和计数。
2. `trace.rgen`（raygen 0）：主路径求交，`primary.rchit` / `primary.rmiss` 写 `HitPayload`。
3. `primary_info.comp`：主表面位置、几何法线、albedo、roughness、material/domain，供 ReSTIR 和主域 SVGF 使用。
4. Wavefront 反弹循环 `bounce = 0..8`：
   - `shade.comp` 处理当前活跃路径；NEE 追加 shadow 任务。
   - `build_indirect.comp` 依据活跃路径数/ shadow 数生成 trace 与 compute 的间接参数。
   - shadow raygen + `shadow.rahit/rchit/rmiss` + `shadow_resolve.comp` 完成当前可见性。
   - `trace.rgen` 追踪下一反弹。
5. ReSTIR DI：`restir_init.comp` → `restir_temporal.comp` → `restir_spatial.comp` →
   `restir_shadow.comp` → shadow trace/resolve。仅对直接可见的非 delta 主表面生效。
6. SVGF：`svgf_temporal.comp`（重投影、矩、初始空间方差）→ 5 轮 `svgf_atrous.comp`
   （步长 1/2/4/8/16，可分离 5x5 核）→ 全屏三角形 `composite.frag` 读滤波结果、解析 emission、
   Reinhard `c/(1+c)`，写 sRGB 交换链。
7. 每个 GPU 阶段之间使用同步 2 barrier；交换链 acquire/present 使用 binary semaphore，
   timeline semaphore 负责跨帧 GPU 完成顺序。

显示使用 `VK_EXT_shader_object`（顶点/片元同次创建并显式 link）、动态渲染、
全屏三角形和 descriptor buffer。默认绘制不经过光栅化场景几何。

## 2. 容量、分支与队列

- 路径队列：双队列，每个容量 `2 * pixelCount`。原因：每个主样本最多一次首次玻璃拆分，
  活跃路径 ≤ `2N`；之后后续玻璃随机选择，不再扩大分支数。
- shadow 任务容量：`2 * pixelCount`；每个活跃路径每反弹最多一个 NEE shadow 任务。
- 信号槽：每像素 3 个（0=主路径，1=首次玻璃反射分支，2=首次玻璃透射分支）。
  slot radiance/guide/tag 均为独立存储；首次拆分前的主路径贡献保留在 slot 0，只计一次。
- 每路径预算：最多 8 次表面散射；镜面反射和玻璃折射/反射都计入。第 8 次散射后的
  emissive/miss 仍可完成贡献；不再产生第 9 次散射。第 4 次散射后执行 Russian Roulette。
- ReSTIR：`reservoirInit` N、`reservoirTemporal` N、跨帧 `reservoirHistory` 2N；
  primaryInfo 和 jitter 均为 2N（两个 CPU frame slot）。历史 multiplicity 上限 32。
- SVGF：3 个信号槽 × 2 个 frame slot 的时间颜色/矩；空间滤波每信号槽 2 个 ping-pong 块。
  历史长度上限 32，新样本权重下限 1/32。
- 固定尺寸稳态不重建 path/shadow/reservoir/guide/降噪/descriptor/SBT/command 对象；
  只有 resize 或初始化时重建帧尺寸资源。

## 3. ReSTIR DI 归一化

- 初始 reservoir：每像素 4 个候选，proposal 为发光面功率 CDF + 三角形均匀面积采样，
  `w = target / pdfArea`；保存选中样本、weight sum、M=4 和 target luminance。
- Temporal/spatial 复用：对每个 source 使用当前 shading point 重新计算选中样本的 target，
  并做支持域校正：

  `sourceWeight = target_current(selected) * weightSum_source / target_source(selected)`

- 组合时以 `sourceWeight` 做 weighted reservoir sampling，输出 M 为各 source M 之和；
  最终 shadow 估计为 `target_rgb(selected) * weightSum / (M * targetLum(selected))`。
- 历史 M 超过 32 时按 `weightSum *= 32 / M` 同步缩减；不是只改 M。
- 空间 pass 之后再执行当前 visibility shadow；历史 visibility 不直接复用。
- 主路径 depth 0 的 NEE 被跳过，由 ReSTIR 独占该顶点的直接光；更深顶点的 emitter
  命中使用 NEE/BSDF power-heuristic MIS。

## 4. SVGF guide 与历史规则

每个 guide 携带：位置、几何法线、roughness、线性 albedo、chainHash（含终端与各 delta 面）、
链长度、终端 surface id、终端 hit distance、motion、domain、material、valid、guide frame。
主表面 guide 使用实际主表面；纯 delta 链使用反射面逆序合成后的虚拟位置/法线；
玻璃透射使用实际终端位置。

- 时间历史读取上一成功渲染帧（另一个 frame slot）的同信号槽。
- 普通/反射：用前后帧无 jitter 相机做重投影，查找历史像素时补偿上一帧 jitter；
  比对位置/深度、法线、材质/表面、链 hash、链长度、终端、hit distance 和 guide frame。
- 反射链：guide 的虚拟位置/法线来自累计镜像变换；镜间反射和镜后新揭露区域按 guide 拒绝。
- 透射：相机位置/朝向/FOV/projection 任一变化时当帧拒绝所有透射历史并重新初始化；
  相机静止时使用相同像素 + guide 链匹配，验证 entry/exit、介质、分支、终端和深度/法线。
  空间方差和 à-trous 继续执行，不全局 reset。
- R/camera cut、resize、暂停/恢复：相关 guide、reservoir、矩和 history length 同步失效。
- 历史矩为一阶/二阶亮度矩；无效时从当前样本初始化；有效时按 history length 混合。
  少于 4 个有效历史样本时，在兼容 guide 的 5x5 当前样本邻域构造局部空间方差。
- 5 轮 à-trous 使用独立输入/输出，权重同时考虑 guide 兼容、位置、法线、roughness 和
  方差归一化亮度差；方差按平方权重传播，边界按有效权重重新归一化。
- 合成使用 guide 的 albedo 做 demod/re-mod；零 albedo 通道确定性输出零贡献。

## 5. 同步与资源所有权

- frame slot 与 swapchain image 身份分开管理；acquire/present 使用 binary semaphore，
  不以 timeline 完成替代 presentation engine 释放交换链图像。
- 每 frame slot 的 fence 保证 CPU 不提前复用 command/globals；timeline semaphore 保证跨帧
  history 在 GPU 上有序；历史 ping-pong 使当前帧写块与上一帧读块无 race。
- 初始化、resize、最小化恢复和退出使用 `vkDeviceWaitIdle` 安全点；普通帧内不调用。
- descriptor buffer 为 host-visible、按 `descriptorBufferOffsetAlignment` 对齐；所有 set layout
  使用 `VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`。
- 静态 BLAS：`ALLOW_COMPACTION` 构建 → query 真实压缩大小 → compact copy → 用压缩后地址
  构建 TLAS → 释放旧 BLAS/scratch。稳态不重建。
- 描述符、shader object、SBT、AS、纹理、frame 对象在 shutdown 逆序销毁；descriptor buffer
  也在 allocator 之前销毁。

## 6. 实际核验结果

- Windows Debug（`windows-msvc-debug`）构建通过；`/W4 /WX` 无警告。
  以 core + synchronization validation 运行，完成初始化、首帧显示、连续帧、静态收敛、
  窗口缩放、最小化/恢复、R 重置和正常关闭；validation 无 warning/error，GPU 同步无
  WRITE_AFTER_READ/WAW 等诊断。GLFW/Vulkan 错误数为 0。
- 窗口请求客户区 1280x720；本机高 DPI 内容缩放 2.0，实际 framebuffer 为 1280x720，
  渲染与标题 FPS/ms 使用该 framebuffer 尺寸。
- 观察结果：初始入口视角可见红/蓝墙、顶板开口、4 个发光块、玻璃体；向前移动后可见
  后方镜面墙及左镜面墙的反射，镜中重投影使用虚拟表面历史；静止时普通/镜面历史累积，
  SVGF 平滑；移动时透射域历史被拒绝而空间滤波继续。发光体与顶板接触处及低采样玻璃
  存在残余噪声/萤火虫，属于低采样噪声而非机制缺失。
- Windows Release：代码以 `/W4 /WX` 和 Release shader 优化通过构建并运行；Release 不请求
  validation layer/debug utils，不创建 debug messenger。
  本机 `windows-msvc-release` 预设重新安装依赖时 vcpkg 的 glfw3 端口构建失败，因此
  Release 验证使用已准备的 x64-windows 依赖树手工配置构建；未改动预设、依赖声明或
  vcpkg 配置。
- Linux：本机无 Linux 环境，未实际构建/运行；代码没有 Win32 专有窗口/文件接口，
  Windows 专有部分仅限 `_WIN32` 下的可执行文件目录和 DPI awareness。
