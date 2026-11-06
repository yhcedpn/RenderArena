# VoxelPathTracer 实现说明

## 工程与场景

实现使用 C++20、GLSL 460、volk 和 SDL3。资源基准是 `SDL_GetBasePath()` 返回的可执行文件目录。`src/scene.cpp` 读取必需的外部材质字段，检查类型、范围、类型间约束与场景 ID；材质数值不在场景代码中另存副本。合法的零发光功率产生空光源域，直接光估计为零。

场景按题面顺序填充。不透明格与平移后的完整玻璃体分别提取边界。每个轴、方向、平面的面掩码按宽优先、再扩整行的顺序执行 greedy meshing；矩形的两组三角形共享稳定 surface ID。UV 使用面内世界坐标，合并后仍每单位重复。随题配置的结果为 **115 个矩形、230 个三角形、40 个暴露发光三角形**。玻璃边界保留底面，与地板顶面独立。

各材质各有一层基础色和粗糙度纹理。Value 噪声由配置种子确定；木纹沿 U 延伸并加入低频扰动。基础色先在 sRGB 域调制，再以 RGBA8 sRGB 上传；mip 在线性域平均后重新编码。粗糙度使用 R8 UNORM 线性 mip，rough metal 的量化结果至少为一个非零 UNORM 单位。采样器为 linear/trilinear/repeat。路径由像素张角、累计传播长度、散射后的张角扩展及表面倾角估计 ray-cone footprint，使用显式 LOD。

## Vulkan 路径与所有权

`src/renderer.cpp` 分别检查 loader、物理设备的 Vulkan 1.4，查询并启用 timeline、BDA、scalar layout、dynamic rendering、synchronization2、AS、RT pipeline/indirect、descriptor buffer 与 shader object。Shader Object feature 授权所使用的 EXT 动态状态；已入核的基础动态状态、rendering、barrier 和 submit 使用核心命令。SBT、scratch、descriptor offset 和容量由查询的属性计算。

静态三角形按材质建立 8 个 Triangle BLAS。每个 BLAS 使用 `ALLOW_COMPACTION`，实际写入 compacted-size query，读取大小、执行 `vkCmdCopyAccelerationStructureKHR(COMPACT)`，等待完成后释放原 AS。TLAS 实例只引用压缩后的地址，关闭 facing cull。随题场景本轮得到 **40,696 → 16,568 字节**的 BLAS 压缩结果。稳态不更新 AS。

24 个 binding 使用带 descriptor-buffer 标志的 layout，依次覆盖 TLAS、场景、Frame、队列、命中、计数、信号、guide、reservoir、history、滤波、shadow、visibility 和纹理。layout size、各 binding offset 和 descriptor size 均查询自设备；内容由 `vkGetDescriptorEXT` 生成。两个 CPU slot 各有自己的 Frame uniform，descriptor backing 为 slot × 历史 parity 的四个静态区域。RT pipeline 带 `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`，递归深度为 1；compute 与显示只创建 `VkShaderEXT`。

固定尺寸的 GPU 队列、所有信号和历史、两份 command pool/command buffer 持久复用。帧开始的跨提交屏障将上一帧所有读写连接到本帧；单个 graphics/compute/present queue 按提交顺序推进成像历史。CPU slot 的 command、uniform 与 acquire binary semaphore 由 timeline completion 回收。present binary semaphore 按交换链图像保存，只有重新 acquire 同一图像后才复用。初始化、重建、退出使用等待安全点，普通帧和反弹没有 queue/device idle。

上传使用 host→transfer→shader/AS build 屏障；AS build→query/copy→TLAS→trace、RT hit→compute、compute queue/indirect→RT、shadow→计能、各 ReSTIR/SVGF 阶段和最终 fragment 读均有 synchronization2 屏障。附件按 acquire→color attachment→present 转换。

## 实际 pass 顺序

```text
primary 初始化：每像素一个新的 jittered ray，清空本帧三个输出 slot
for segment = 0..8:
    清空下一队列和本阶段 shadow count
    GPU 生成当前活跃计数对应的 RT/compute 间接参数
    硬件 trace：首段 vkCmdTraceRaysKHR，后续 vkCmdTraceRaysIndirectKHR
    compute shading / scattering / 原子 append 压紧下一队列
    GPU 生成 shadow 参数 → 独立硬件 shadow trace → visibility 计能
ReSTIR：4 个新候选 → temporal → 4 邻居 spatial → 当前 shadow trace → 计能
SVGF：分域 temporal/矩 → 短历史空间方差 → à-trous 1/2/4/8/16
全屏三角形：合成三个 slot 及解析 emission → Reinhard → sRGB attachment
```

`shaders/wave.comp` 的 mode 对应上述不同 dispatch；多反弹不在 raygen 内循环。hit/miss 只记录硬件求交事实。每个像素首次玻璃拆分后最多有两个活跃分支，所以两个 path 队列和单个 shadow 队列均为 **2 × framebuffer pixels**。每阶段每条路径至多生成一个 shadow。atomic append 只保存活跃路径；GPU 使用实际 count 生成二维展平的间接参数，padding 和零活跃 dispatch 均先检查 count。

输出有拆分前、首次玻璃反射、首次玻璃透射三个独立 slot。拆分不复制此前 radiance；同一 slot 每阶段最多有一条路径和一个 shadow 写入，阶段间有屏障，最后显示阶段归约，因此不需要浮点原子。散射数由父路径继承，最多 8 次；第 8 次之后仍可查询下一段 emitter/miss。自第 4 次散射后执行带补偿的 Russian Roulette。

Lambert 使用余弦采样；rough metal 使用 GGX VNDF、height-correlated Smith 与 Fresnel-Schlick；mirror 为离散反射。玻璃用几何边界与介质状态处理 Snell、非偏振 Fresnel、TIR、radiance eta²，以及每段实际长度的 Beer 吸收。相机起始介质由连续玻璃边界初始化。首次界面确定性 F/(1−F) 拆分，之后按 Fresnel 离散概率选择，不增加分支数。

直接可见非 delta 主顶点由 ReSTIR 独占直接光，并抑制紧接着的 BSDF emitter hit。其他非 delta 顶点使用 NEE/BSDF power MIS；三角形离散选择概率乘面积倒数得到面积 PDF，再乘距离平方除发光端余弦转换为方向 PDF。经过 delta 后的实际 emitter hit 使用权重 1。直线 shadow 遇到玻璃即遮挡。

## ReSTIR 归一化

唯一 light table 来自实际暴露 emissive 三角形，CDF 权重为面积乘发光亮度。候选保存 light ID 与 barycentric。面积域 target 是未乘 visibility 的 `luminance(BSDF × emission × cos_receiver × cos_light / distance²)`，初始候选权重为 target/proposal，M=4，包括零权重候选。

复用 source reservoir 的选择权重为 `target_dst(y_i) × W_i × M_i`。选出 y 后重新计算各 source 对 y 的支持：

```text
S = Σ target_dst(y_i) W_i M_i
Z(y) = Σ M_i · 1[target_source_i(y) > 0]
W_out = S / (target_dst(y) Z(y))
```

当前 fresh source 覆盖当前 target 的支持域；Z 按选中样本处的各 source 支持校正，包含给出零权重结果的 source multiplicity。空 reservoir/零 target 输出零权重。历史 M 上限为 32；截短时 W 保持不变，保存的等效 weight sum 同步改为 `W × target × M`。没有亮度 clamp 或额外增益。

Temporal 按普通 primary guide 重投影并检查 surface、material、位置和法线。Spatial 只读已完成的 temporal buffer，选择 4 个帧间变化、距离不超过 8 像素的邻居，输出到独立 spatial/history buffer。最后从当前表面重新发真实 shadow ray，不复用历史 visibility。

## 光学 guide 与 SVGF

三个物理 slot 由终端类型、branch、链身份和透射标志进一步区分普通漫反射、粗糙高光、镜面链、玻璃反射和玻璃透射的历史。所有 slot 保留本帧新的 HDR 原始贡献。直接 emission 与黑环境解析合成，空间滤波不扩散发光轮廓。

纯 delta 反射链逐面累积反射仿射变换；终端点与法线变换到虚拟世界空间，再投影到上一帧无 jitter 相机。历史同时匹配稳定表面/材质、branch、链长度、反射/透射事件的面身份哈希、终端位置、法线、roughness 和 hit distance。普通表面用实际世界位置。投影去除上一帧 jitter；本帧 hit 已包含当前 jitter。

若首次玻璃出现在先前非 delta 散射之后，分支保留原主信号的重投影锚点和 demodulation albedo，同时记录玻璃链之后的实际终端位置、法线、材质、surface、roughness 和传播距离，额外核对终端，不用主 hit 代替它。超过预算仍没有有效终端的 guide 不进入历史，radiance 仍合成本帧。

任何实际发生透射的分支均标记为透射域，包括最初反射、后来透射的分支。无 jitter 相机改变时只拒绝透射域的 temporal；静止时采用同像素路径对应和前后 jitter 补偿，并匹配 entry/exit 链与终端。普通和纯反射域继续局部重投影。R/camera cut、extent/资源重建同步重置 guide、reservoir 和 moments 的有效性。

各 slot 独立累积 RGB、亮度一/二阶矩和最长 32 帧的 history length；新样本权重至少 1/32。少于 4 帧时，使用 guide 兼容的 5×5 邻域补充方差。5 轮 à-trous 使用指定 kernel 的二维乘积，综合光学身份、位置/离面距离、法线、roughness 和方差归一化亮度差。每轮独立 ping-pong，并按归一化权重平方传播方差。时间历史只保存 temporal 结果，不反馈最终空间滤波图。漫反射零 albedo 分量显式输出零，其他分量 demodulate 后滤波并重新调制。

当前/上一相机、jitter、guide 和 reservoir 的 parity 只在成功 `vkQueueSubmit2` 后推进；acquire 失败和零尺寸暂停不伪造历史帧。窗口输入、真实经过时间移动、像素尺寸、最小化/恢复与 WSI 重建在 SDL 事件循环中处理。标题统计正常帧循环墙钟时间和成功 present 次数，初始化/暂停/重建后重新统计。

## 实际核验

本轮使用 Windows、MSVC 19.51.36260、CMake 4.3.1-msvc1、shaderc/glslc v2026.4、SDL 3.4.12、Vulkan Headers 1.4.350。运行设备报告 API 1.4.359。离线生成物的 SPIR-V 版本字为 `0x00010600`（1.6）；shader 源为 GLSL 460，编译由随题 CMake 入口完成。

| 项目 | 实际结果 |
| --- | --- |
| `cmake --preset windows-msvc-debug` | 成功 |
| `cmake --build --preset windows-msvc-debug` | C++ 与 shader 编译成功，无 warning/error/诊断 |
| `cmake --preset windows-msvc-release` | 成功 |
| `cmake --build --preset windows-msvc-release` | C++ 与 shader 编译成功，无 warning/error/诊断 |
| 最终 Debug 运行 | 1280×720；完成 50 个渲染提交；按本轮 PID 请求关闭，退出码 0；core/synchronization validation 无 warning/error，INFO 已记录 |
| 最终 Release 运行 | 1280×720；完成 42 个渲染提交；按本轮 PID 请求关闭，退出码 0；未启用校验层、debug utils 或 messenger，stderr 为空 |
| BLAS 压缩 | 8 组，真实 query/copy，40,696 → 16,568 字节 |

本轮构建日志为 `obj/build-debug-final.log`、`obj/build-release-final.log`；运行日志为 `obj/run-debug-final.stdout.log`、`obj/run-debug-final.stderr.log`、`obj/run-release-final.stdout.log`、`obj/run-release-final.stderr.log`。开发期间出现并修正的诊断摘要保留在 `obj/development-diagnostics.log`。

## 未核验项目

以下需要组织者真实交互与目视核验，本实现会话未调用桌面/浏览器工具、未采集屏幕或窗口图像：

- 从初始位置进入展厅、WASD/QE/Shift 移动、横向遮挡揭露，以及普通表面历史的视觉稳定性。
- 右键捕获/释放、yaw/pitch、Esc、失焦后恢复和 R 重置的实际交互。
- 两个镜面的屏幕外内容、预算内镜间反射和移动时虚拟表面历史效果。
- 玻璃前静止、随后移动、穿入/离开玻璃、TIR、反射/透射独立历史与厚度吸收的视觉效果。
- 窗口缩放、高 DPI、最小化/恢复及这些操作后的标题统计与资源重建。
- 间接照明、软阴影、粗糙高光、静止降噪、轮廓渗色、重影和最终计能/gamma 的目视验收。
- 修改合法材质配置重启后的画面变化，以及缺失/错误配置的逐项运行用例。

Linux 未构建、未运行。上述未核验项不由构建成功或 validation 无诊断替代；完整交互验收仍未完成。
