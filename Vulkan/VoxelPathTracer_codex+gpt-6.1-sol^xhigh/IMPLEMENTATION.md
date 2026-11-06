# VoxelPathTracer 实现说明

本实现使用 C++20、GLSL 460、Vulkan 1.4 和 SDL3。运行资源通过 SDL3 提供的可执行文件目录定位；材质数值读取 `materials.json`，shader 读取离线编译的 SPIR-V。

## 源码职责

| 文件 | 职责 |
| --- | --- |
| `main.cpp` | SDL 窗口、输入、真实时间相机移动、暂停与标题帧率统计 |
| `src/scene.cpp` | 必填材质解析、程序化纹理及 mip、格子填充、greedy meshing、光源 CDF |
| `src/renderer.cpp` | 能力合同、内存、上传、压缩 AS、Descriptor Buffer、RT/SBT、Shader Object、同步与呈现 |
| `shaders/path.*`、`surface.rchit` | 硬件求交与 hit/miss 事实输出 |
| `shaders/shade.comp` | 材质采样、throughput、首次玻璃拆分、NEE/MIS、紧凑队列 append |
| `shaders/restir.comp`、`visibility.comp` | ReSTIR temporal/spatial、当前 shadow visibility 和贡献 resolve |
| `shaders/denoise.comp` | 分域时间矩、短历史空间方差、5 轮 à-trous |
| `shaders/display.*` | 全屏三角形、各域重建合成、Reinhard、sRGB attachment 输出 |

## 几何、材质与加速结构

按题面顺序填充整数格，仅保留不透明格与空气之间的面。以方向、平面、材质分组建立暴露面掩码，确定性地先扩展 U、再扩展 V；移除已经覆盖的掩码后继续。每个矩形的扫描起点与扩展规则保证其不能在剩余掩码中继续整边扩大。UV 从世界位置的平面坐标得到，合并面仍每世界单位重复一次。

玻璃独立生成 `[7,9] × [0.125,2.125] × [7,9]` 的六个外面；其底面与地板顶面都保留。每个 greedy 矩形具有稳定 surface ID，三角化后的两个三角形共用该 ID。

材质按配置的类型枚举进入对应 BSDF。所有字段、类型约束、唯一 ID、必需场景 ID、版本与纹理尺寸均在 CPU 解析。基础色按 sRGB 编码域调制，上传为 sRGB 数组纹理；粗糙度为线性 R8 数组纹理。石纹采用多尺度 Value noise，木纹沿 U 延伸并加入低频扰动。颜色 mip 在线性空间平均后重新编码，粗糙度 mip 在线性域平均。采样器使用 linear/trilinear/repeat。RT 路径维护 ray cone 的宽度与角度，并以距离、表面入射角和纹理尺寸选择显式 LOD。

按材质建立 Triangle BLAS，所有几何进入 TLAS。BLAS 的构建标志包含 allow-compaction：设备查询真实 compacted size，完成 compact copy，释放原 BLAS，再将压缩地址写入 TLAS instance。玻璃和其他几何均不依赖背面剔除。静态 AS 在窗口重建时保留。

## 实际 GPU pass 顺序

```text
init：一个新 jitter 主样本 / 像素，清空本帧贡献和 guide
  ↓
prepare：清空下一队列与 shadow count，生成 GPU 间接参数
  ↓
primary RT：vkCmdTraceRaysKHR
  ↓
hit/shading：BSDF、NEE 或 ReSTIR initial、scatter、紧凑 append
  ↓
ReSTIR temporal → spatial → 最终样本 shadow append（仅 primary）
  ↓
shadow 参数生成 → shadow RT indirect → visibility resolve
  ↓
重复 prepare → continuation RT indirect → shading → shadow → resolve
  ↓
temporal radiance / moments → 短历史局部方差 → à-trous × 5
  ↓
分域重建合成 → Dynamic Rendering 全屏三角形 → present
```

主求交和 shadow 使用独立 raygen，RT pipeline recursion depth 为 1。closest-hit 不递归追踪。最多处理 8 次散射，另允许最后一段完成 emitter hit 或 miss。自第 4 次散射后使用 throughput 最大分量、限制到 `[0.05,0.95]` 的 Russian Roulette，并对存活路径补偿。

Lambert 使用余弦半球采样；粗糙金属使用 GGX VNDF、height-correlated Smith 和 Schlick Fresnel，方向 PDF 与采样一致。mirror 是离散反射。玻璃按几何法线、方向及介质状态处理 entry/exit，使用非偏振 Fresnel、Snell、全反射与 radiance transport 的 eta²。玻璃内每段按实际 hit distance 施加 Beer–Lambert 吸收。

每个样本首次玻璃界面确定性拆分反射和透射；后续界面按 Fresnel 概率选择并补偿。分支继承散射计数。相机介质状态从玻璃体边界初始化。普通 shadow ray 对玻璃保持遮挡语义。

ReSTIR 主顶点的下一段直接 emitter hit 被排除；若经过 delta 事件再到达 emitter 则保留。其他非 delta 顶点使用功率光源 NEE 与 BSDF continuation 的 power-heuristic MIS。面积 proposal 用距离平方和光源余弦转换为方向 PDF 后才参与 MIS。

## 队列与贡献所有权

令 `N = framebuffer width × height`：

- 两个活跃队列各容量 `2N`，状态槽为主分支 `pixel` 与第二分支 `N + pixel`。首次拆分只发生一次，因此同时最多两条路径。
- 每阶段每条路径最多一项 NEE shadow；primary ReSTIR 取代该阶段的 NEE。shadow 容量为 `2N`。
- 三组贡献槽分别保存拆分前前缀、首次玻璃反射分支、首次玻璃透射分支。前缀不会随拆分复制。每组的 raw radiance、guide、历史和方差互相独立。
- 队列通过 GPU uint atomic append 紧凑写入。prepare 根据真实计数生成 trace 和 compute 间接参数；RT launch 使用二维展平，并以实际计数排除 padding。CPU 不读取每次反弹的活跃计数。
- 一个阶段中同一贡献槽最多一条 shadow；shading 与 shadow resolve 之间有 GPU 屏障。两个分支不并发写同一个 RGB 地址。

固定 framebuffer 尺寸时复用队列、计数器、shadow、reservoir、guide、history、滤波缓冲区和 command 资源。

## ReSTIR DI 归一化

光源表只来自最终暴露 emissive 三角形。离散三角形选择概率按面积乘 emission 亮度归一化，面内按面积均匀采样。reservoir 保存稳定 light-table 身份、barycentric 坐标、代表质量、M 和估计器权重 W。

每个有效 primary 非 delta 像素生成 4 个新候选。目标 `t_i(y)` 为未计 visibility 的 RGB 直接贡献亮度，包含 BSDF、两端余弦和距离几何项，使用面积 measure。初始选择权重为 `t_i(y) / q_A(y)`。

复用时将 source `i` 的选择权重设为：

```text
w_i = t_current(y_i) × W_i × M_i
S   = sum(w_i)
Z(y_selected) = sum(M_i × [t_i(y_selected) > 0])
W_out = S / (t_current(y_selected) × Z(y_selected))
```

所选样本在每个参与 source 的 shading point 与相应相机下重新求值支持域。这个 Z 校正避免把互不支持该样本的来源也简单计入归一化分母。空样本和零 target 的 W 定义为零。保存的代表质量为 `W_out × t_current × M_out`，使下一次复用具有相同估计意义。

时间来源的 M 上限为 32；缩减 M 时同比缩减代表质量，W 不变。空间阶段读取完整 temporal 结果，选择 4 个随像素和帧变化、整数半径不超过 8 的邻居。初始、temporal、空间结果和前帧结果使用不同存储区域。最终选中样本在当前 shading point 发出真实 shadow ray；历史 visibility 不参与替代。

## Guide、光学重投影与 SVGF

guide 保存终端 surface/material ID、位置、法线、roughness、albedo、hit distance，以及最多 8 项带反射/透射事件编码的表面链身份。材质模型、光学链和分支是历史匹配条件；同一物理存储中模型变化会拒绝对应旧历史。

普通表面从世界位置投影至前帧无 jitter 相机。纯反射链将终端位置和法线通过反射平面逆序展开至虚拟空间，再投影前帧相机。因本场景的平面均与坐标轴对齐，反射组合可准确保存为逐轴符号与平移，而不需要通用矩阵。比较链身份、终端、虚拟位置/深度、法线和 roughness，避免仅凭第一块镜面保留历史。

透射域在无 jitter 相机发生变化时拒绝时间历史；静止时按像素对应与前后 jitter 补偿查找历史，并核对 entry/exit、分支链和终端。首次反射分支后来发生透射也转入该规则。若玻璃拆分之前已经经过随机非 delta 散射，或冻结终端之后再发生玻璃事件，该条贡献没有确定的相机光学对应，保留原始 radiance 并标记 guide 无效，不把该随机路径声明为可重投影的纯反射链。

每个有效信号独立维护 temporal RGB、一阶矩、二阶矩和 history length。长度最多 32，当前样本权重不低于 1/32；少于 4 个历史样本时使用 guide 引导的 5×5 本帧空间矩补充方差。漫反射在过滤前按终端 albedo demodulate，零 albedo 分量定义为零贡献，合成时再乘回 albedo。

à-trous 步长为 1、2、4、8、16，二维 kernel 是 `[1,4,6,4,1]/16` 的乘积。权重使用表面/链兼容性、位置/深度、法线、roughness 和方差归一化亮度差。每轮使用不同输入输出，方差按平方权重传播并重新归一化。历史仅反馈 temporal 结果及其矩。直接 primary emission 与黑色 miss 不参与空间模糊。没有有效 guide 的贡献仍进入本帧合成。

R/camera cut、extent 变化和帧资源重新初始化同步拒绝所有域和 reservoir 的旧历史。连续相机运动保留普通与纯反射域的局部有效历史，只使透射域的时间历史失效。

## Vulkan 同步与生命周期

所有资源描述符由 `vkGetDescriptorEXT` 生成；layout size、binding offset、descriptor size 和 alignment 来自设备查询。RT pipeline 使用 Descriptor Buffer 标志，compute 和最终 vertex/fragment 使用 `VkShaderEXT`。显示使用核心 Dynamic Rendering，没有 render pass、framebuffer、descriptor pool/set 或普通 compute/graphics pipeline。

上传与 AS 构建在初始化时完成必要等待。稳态通过核心 `vkCmdPipelineBarrier2` 在 RT 写入、compute shading、队列与间接参数、shadow resolve、ReSTIR 各阶段、temporal moments、各轮滤波和 fragment 读取之间建立依赖。SBT 读取使用当前 RT 扩展合同适用的 shader-read 访问范围。

两个 CPU frame slot 分别拥有 command pool/buffer、acquire binary semaphore、参数区及 timeline 完成值。相邻提交等待上一渲染帧的 timeline 值，以保护共享成像资源和前帧历史。只有成功 `vkQueueSubmit2` 后才推进 frame identity、前帧相机和 jitter。

present-ready binary semaphore 按交换链图像分配，重新 acquire 同一图像后才再次使用。交换链图像身份与 frame slot 分开。附件转换覆盖 acquire → color attachment → present。窗口重建与最终退出允许 device idle，普通帧与每次反弹不调用全局 idle。关闭先排空 GPU，再释放 shader、AS、同步对象、纹理、缓冲区、设备与 SDL 窗口。

## 实际核验

核验日期：2026-11-07。实际平台为 Windows。工具版本：MSVC 19.51.36260、CMake 4.3.1-msvc1、shaderc/glslc v2026.4、SPIR-V Tools v2026.4、SDL 3.4.12、volk/Vulkan Headers 1.4.350、nlohmann-json 3.12.0。loader 报告 Vulkan 1.4.363；生成 SPIR-V 的版本字为 `0x00010600`（1.6）。

已执行任务预设的四个配置/构建命令：

```text
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

最终 Debug/Release 的 C++ 和全部 13 个 stage shader 构建成功，无编译 warning/error/诊断。Debug 启用 Khronos core 与 synchronization validation；Release 不请求校验层、debug utils 扩展或 debug messenger。

实际输入产生 1,702 个不透明暴露单位面，连同玻璃共生成 115 个 greedy 矩形、230 个三角形和 40 个有效发光三角形。8 组 BLAS 的真实构建大小合计 40,696 字节，compact query/copy 后合计 16,312 字节，TLAS 使用压缩地址。1280×720 下路径和 shadow 容量均为 1,843,200，三个贡献域共有 2,764,800 个槽。

最终运行记录如下。核验程序按本轮启动返回的进程对象发送关闭请求，通过 SDL 事件循环退出并完成 GPU 排空。原始配置、构建、运行日志和已修正诊断保存在本轮 `obj/session-*` 工作记录中。

| 构建 | 实际提交帧数 | 关闭 / GPU 排空 | 进程退出码 | 诊断 |
| --- | ---: | --- | ---: | --- |
| Debug | 107 | 完成，`VkResult=0` | 0 | core / synchronization validation 无 warning/error |
| Release | 110 | 完成，`VkResult=0` | 0 | stderr 为空，未启用校验层 |

### 未核验的行为

本轮没有桌面/浏览器操作，也没有采集窗口图像。以下检查尚未由人工执行，不能用构建或提交帧数代替：

- 从入口进入展厅；WASD/Q/E/Shift 的实际移动速度与方向；右键鼠标捕获、释放、失焦及 Esc 行为。
- 横向移动时的遮挡揭露、普通表面历史保留、镜面屏幕外内容和镜间反射、反射空间重投影的视觉正确性。
- 玻璃前静止积累再移动、反射/透射同时可见、吸收、穿入与离开玻璃、全反射以及各光学轮廓的视觉表现。
- 窗口缩放、高 DPI、最小化/恢复、R 重置和标题栏平均 FPS/ms 的人工交互核验。
- 改动合法材质参数后的画面变化，以及缺失/错误输入和 shader 的真实失败运行。
- 间接照明、软阴影、ReSTIR 计能、各 SVGF 域收敛、重影、漏色和最终 gamma 的人工画面验收。
- Linux 配置、Debug/Release 构建和运行均未执行。

