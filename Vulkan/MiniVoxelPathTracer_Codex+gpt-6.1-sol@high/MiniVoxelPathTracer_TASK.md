# MiniVoxelPathTracer Vulkan 路径追踪编程能力测评

## 一 任务目标与范围

使用 **C++20、GLSL 和 Vulkan 1.4** 实现单窗口、可自由移动摄像机的小型体素展厅。完整渲染链必须包含硬件光线追踪、GPU wavefront 多反弹积分、自研 ReSTIR DI、自研 SVGF、平面镜的反射空间重投影，以及独立的厚玻璃反射和透射信号处理。

本题考察大语言模型实现复杂图形程序的正确性与工程完整性。规定的优化机制是交付要求，必须接入实际调用路径；画面相似、构建通过或采用算法名称不能替代对应机制的实现。

场景静态，只提供相机操作与窗口操作。不要求方块编辑、动画、世界流送、水体、参与介质、景深、运动模糊、超分辨率、ReSTIR GI/PT、光照缓存或专门的焦散求解器。不要求命令行验收模式、图像导出或高 SPP 参考模式。

## 二 语言 依赖与跨平台工程

### 1 语言与工程边界

- C++ 必须以 C++20 标准编译；所有 shader 使用 GLSL，编译为适用于 Vulkan 1.4 的 SPIR-V，版本不高于 1.6。
- 目标平台为 Windows 和 Linux；窗口、输入、文件读取及资源生命周期代码必须适应两种平台，不依赖 Win32 专有窗口或文件接口。
- 使用当前测评工程提供的配置和依赖声明。实现优先放在 `main.cpp`，辅助代码可使用头文件，shader 放在 `shaders/`。
- 不修改已有 `.vcxproj`、`.vcxproj.filters`、`.vcxproj.user`、`CMakeLists.txt`、`CMakePresets.json`、`platforms.json` 或 `vcpkg.json` 的既有内容；允许按仓库规则为新增源文件和资源添加 VS 引用行。
- 本题资源包中的 `materials.json` 必须作为外部文件随程序部署。资源路径使用跨平台路径处理，并以可执行文件所在目录作为基准，不依赖启动时的工作目录或上级源码目录。
- 所有任务相关读取、修改和生成物限定在当前任务目录及其子目录，不修改共享配置或已安装依赖树。

### 2 允许依赖

仅使用测评工程已声明的以下依赖，按需选择：GLFW、Vulkan headers/loader、volk、Vulkan Memory Allocator、Vulkan Utility Libraries、shaderc、SPIRV-Tools、SPIRV-Reflect、nlohmann-json。

GLFW 用于窗口、事件和 Vulkan Surface；volk 用于 Vulkan 函数加载；`materials.json` 使用 nlohmann-json 读取。shader 可通过 shaderc 在运行时编译，也可使用工程已有的 shader 编译入口；交付 GLSL 源码，编译失败必须报告诊断并退出，不能使用另一份内置 shader 继续运行。

不引入 Slang、ImGui、RTXDI、NRD、OIDN、OptiX、FSR、第三方渲染框架或其他未声明的功能库。可以参考论文与公开算法说明，但积分器、ReSTIR DI、SVGF、体素表面提取与光学历史处理必须在本项目中自行实现，不能包装第三方完整功能实现冒充自研。

## 三 Vulkan 能力与实际 API 路径

### 1 能力合同

启动时分别检查 loader/instance 和物理设备的 Vulkan 1.4 支持，通过 features/properties 查询建立设备能力报告，查询并启用实际消费者需要的 feature。必需能力包括：

- 核心能力：Dynamic Rendering、Synchronization 2、Timeline Semaphore、Buffer Device Address、Scalar Block Layout。
- 扩展：`VK_KHR_swapchain`、`VK_KHR_acceleration_structure`、`VK_KHR_deferred_host_operations`、`VK_KHR_ray_tracing_pipeline`、`VK_EXT_descriptor_buffer`、`VK_EXT_shader_object`，及未由 Vulkan 1.4 核心满足的实际依赖。
- RT features 包括 `rayTracingPipeline`、`rayTracingPipelineTraceRaysIndirect`；AS、Descriptor Buffer 和 Shader Object 分别查询并启用对应 feature。
- Shader Object 所需动态状态能力按规范查询并启用；纹理格式、浮点 storage image/buffer、SBT、AS scratch、descriptor alignment 和容量按实际设备属性建立布局。

不依赖厂商专有扩展，不以 GPU 型号代替能力检查。缺少必需能力时报告具体缺项并退出，不切换到软件求交、CPU 渲染、Ray Query 或光栅世界渲染。Windows/Linux 支持是平台合同，不代表任意 GPU 都具有本题所需能力。

### 2 绑定与执行

- 世界求交必须使用真实 Triangle BLAS/TLAS、SBT 和 Ray Tracing Pipeline。执行使用 `vkCmdTraceRaysKHR` 与后续 GPU 参数驱动的 `vkCmdTraceRaysIndirectKHR`；不以 compute shader 中遍历自建 BVH 或解析遍历方块代替硬件求交。
- RT pipeline 的 `maxPipelineRayRecursionDepth` 固定为 1。多反弹通过 wavefront 队列和多次 dispatch 推进，不通过 closest-hit 中递归调用 `traceRayEXT` 实现。
- 所有资源绑定统一使用 Descriptor Buffer。描述符内容通过 `vkGetDescriptorEXT` 生成，通过 `vkCmdBindDescriptorBuffersEXT`、`vkCmdSetDescriptorBufferOffsetsEXT` 绑定；查询 layout 大小、binding offset、descriptor size 和 alignment，不硬编码驱动描述符布局。
- RT pipeline 使用 `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`；Shader Object 和 descriptor layout 使用与 Descriptor Buffer 路径匹配的配置。
- Compute 与最终显示的 vertex/fragment 阶段使用 `VkShaderEXT` 和 `vkCmdBindShadersEXT`，不能创建普通 compute/graphics pipeline 代替。RT 仍使用 RT `VkPipeline`。
- 最终显示采用 `vkCmdBeginRendering` / `vkCmdEndRendering`。不创建 `VkRenderPass`、`VkFramebuffer`、descriptor pool 或普通 descriptor set。
- 屏障、布局转换与提交使用 Synchronization 2 核心 API，包括 `vkCmdPipelineBarrier2` 与 `vkQueueSubmit2`。

本题不强制 Host Image Copy、Push Descriptors、Pipeline Binary 或专用传输队列。纹理和几何可通过 staging buffer 上传，建立完整传输依赖即可；不为了凑齐 Vulkan 1.4 特性加入与实际渲染无关的调用。

## 四 材质配置与程序化纹理

随题提供的 `materials.json` 是材质数值的唯一来源。代码中可以固定场景引用的材质 ID，不得重复保存基础色、粗糙度、IOR、吸收系数、发光强度、纹理种子或纹理尺寸的数值副本。

### 1 配置格式

根对象包含 `schemaVersion`、`textureSize` 和 `materials`。版本为 1；`textureSize` 为 16 至 1024 范围内的 2 的整数次幂，默认资源包取 128。材质包含：

| 字段 | 语义与范围 |
| --- | --- |
| `id` | 非空且唯一的字符串 |
| `type` | `diffuse`、`rough_metal`、`mirror`、`dielectric`、`emissive` |
| `baseColorSRGB` | 3 个有限数，各分量在 [0,1] |
| `metallic`、`roughness` | 有限数，范围 [0,1] |
| `ior` | 有限数；dielectric 在 (1,3]，其余为 1 |
| `absorption` | 3 个有限非负数，单位为每世界单位的消光系数 |
| `emission` | 3 个有限非负数，scene-linear HDR 发射辐亮度 |
| `pattern` | `solid`、`stone_noise`、`wood_grain` |
| `textureSeed` | uint32 范围的整数 |
| `baseColorVariation`、`roughnessVariation` | 有限数，范围 [0,1] |

`diffuse` 的 metallic=0；`rough_metal` 的 metallic=1 且 roughness>0；`mirror` 的 metallic=1、roughness=0；`dielectric` 的 metallic=0、roughness=0。非 dielectric 的 absorption 为零；非 emissive 的 emission 为零。Mirror、dielectric、emissive 使用 `solid`，variation 为零。各类型的这些约束固定模型语义，不能仅按 ID 猜测材质类型。

缺失文件、解析失败、版本不支持、字段类型或范围错误、ID 重复及场景 ID 无法解析时，明确报告错误并退出；不得使用内置默认材质。无需逐帧重新读取文件，修改配置后重新启动即可。

### 2 纹理生成与采样

CPU 为每个材质生成基础色与粗糙度纹理，宽高为根级 `textureSize`。不要求法线贴图。`solid` 为常量；`stone_noise` 为确定性的多尺度 Value 噪声；`wood_grain` 为沿局部纹理 U 方向延伸的条纹，叠加低频 Value 噪声扰动。使用配置种子，同一实现下相同配置必须得到相同纹理。

基础色以配置值为中心，在 sRGB 编码域按 variation 调制，结果限制在 [0,1]；上传至 sRGB 纹理，由采样转换为线性值。粗糙度以配置值为中心按 variation 调制，使用线性单通道数据；rough_metal 的纹理粗糙度不能退化成离散 mirror。所有 diffuse 的 roughness 字段不改变 Lambert 模型。

生成完整 mip 链。基础色 mip 必须在线性颜色空间平均后重新编码，粗糙度 mip 在线性标量域生成。所有暴露面使用每世界单位重复一次的 UV；表面合并不得拉伸原本按单位方块重复的纹理。RT shader 通过自行维护的 ray cone 或 ray differential 估计 footprint，显式选择 mip，不使用永远固定为 0 的 LOD。滤波和地址模式为线性、trilinear、repeat，不强制 anisotropy。

## 五 确定的静态场景

### 1 坐标与建模规则

右手坐标系，Y 向上，X/Z 是水平轴；不规定世界轴与屏幕方向的对应关系。单位方块格 `(x,y,z)` 覆盖 `[x,x+1] × [y,y+1] × [z,z+1]`。以下范围均是整数格索引、包含两端，不是方块中心坐标。

场景固定为 16×16 格的展厅，墙高 6 个单位、地板厚 1 个单位。按下面顺序填充，后写入的材质替换同一格中的先前材质；未填充格为空气。除规定的几何外，不添加装饰、文字、UI 或替代布局。

### 2 格子布局

| 顺序 | 格子范围或列表 | 材质 |
| --- | --- | --- |
| 1 | x=0…15，y=-1，z=0…15 | stone 地板 |
| 2 | x=0，y=0…5，z=0…15 | red 左墙 |
| 3 | x=15，y=0…5，z=0…15 | blue 右墙 |
| 4 | x=1…14，y=0…5，z=0 | stone 后墙 |
| 5 | x=0…15，y=6，z=0…15，但 x=6…9 且 z=6…9 的格子留空 | stone 顶板 |
| 6 | x=5…10，y=1…4，z=1 | mirror 后方镜面方块墙 |
| 7 | x=1，y=1…4，z=4…7 | mirror 左侧镜面方块墙 |
| 8 | (3,0,5)、(3,1,5)、(3,2,5) | red 柱 |
| 9 | (12,0,6)、(12,1,6)、(12,2,6) | blue 柱 |
| 10 | (4,0,10)、(4,1,10)、(5,0,10) | wood 阶梯 |
| 11 | (10,0,10)、(10,1,10) | rough_metal 柱 |
| 12 | x=7…8，y=0…1，z=7…8 | glass 连续实心体 |
| 13 | (4,5,4)、(11,5,4)、(4,5,11)、(11,5,11) | glow 发光块 |

正 Z 一侧没有前墙，摄像机可以从入口观察并进入展厅。镜面方块使用其全部外表面的 mirror 材质，不是贴在墙上的屏幕贴图。两个镜面区应能通过移动相机观察屏幕外物体和有界的镜间反射。

玻璃的 8 个格子先合并为一个整体 2×2×2、无内部界面的闭合体，再将整个玻璃体沿 Y 上移 0.125 单位，最终边界为 `[7,9] × [0.125,2.125] × [7,9]`。这是唯一的非整数位置调整，用于避开玻璃底面与不透明地板的共面接触。不得把它们当作 8 个彼此重合的空气/玻璃边界。场景不存在其他玻璃体、接触的异介质界面、嵌套介质或参与介质。

### 3 表面提取与 AS

必须删除相邻不透明方块之间的内部面；玻璃体内部的面同样删除，保留玻璃体的完整外边界。不透明与玻璃几何按最终实际位置分别提取，不以平移前的格子邻接关系删除玻璃底面或其下方地板顶面。无需处理本题没有提供的接触异介质或嵌套介质。

必须执行 greedy meshing：将同一平面、法线方向、材质和 UV 规则兼容的相邻暴露面合并成最大矩形区域，再三角化。不能直接给每个不透明格生成完整立方体网格，也不能因为合并减少物体身份信息而失去稳定的表面对应。

按材质或空间分组生成 Triangle BLAS；分组方案自行选择。静态 BLAS 使用 allow-compaction，取得真实压缩大小并执行 compact copy，再用压缩后地址构建 TLAS。初次场景构建可以等待完成；稳态不重建或重复压缩静态 AS。玻璃是封闭表面，求交必须能命中入射侧与出射侧，不通过背面剔除丢掉出射面。

不按主相机视锥删除 AS 内的屏幕外几何；它们仍参与反射、阴影和间接照明。

## 六 相机与窗口操作

- 默认可见窗口客户区为 1280×720；摄像机初始位置 `(8,2.5,20)`，看向 `(8,2,6)`，垂直 FOV=60°。
- 针孔相机，右手 basis。主射线使用真实 framebuffer aspect ratio，近裁面为相机前方 0.05 单位的平面；不是把所有射线的 tMin 都固定为 0.05。
- 每帧每像素的主射线使用子像素 jitter。guide 的重投影使用无 jitter 的前后帧相机，查找历史像素时显式补偿两帧 jitter。
- WASD 在水平面移动，Q/E 向下/向上移动；基础速度 3 世界单位/秒，Shift 为 3 倍速度。按鼠标右键捕获鼠标并控制 yaw/pitch，释放右键解除捕获；pitch 限制在 ±89°，yaw 不限。
- 摄像机是无碰撞的自由相机，允许进入玻璃体。运动依据真实经过的时间，不依据渲染帧数；窗口失焦时释放鼠标，失焦/最小化恢复后的时间不能造成相机跳跃。
- R 恢复初始相机并显式标记 camera cut；Esc 释放已捕获鼠标，未捕获时关闭窗口。除这些操作外，不要求调参或诊断快捷键。
- 处理 framebuffer resize、交换链 out-of-date/suboptimal、最小化及恢复。零尺寸时暂停 GPU 渲染且继续处理窗口事件；非零尺寸恢复后重建相关资源。
- 使用实际 framebuffer 尺寸而不是逻辑窗口尺寸处理投影、dispatch 和附件，适应高 DPI；画面覆盖全部客户区，不产生拉伸比例错误或未覆盖区域。

## 七 路径积分与厚玻璃

### 1 采样与材质模型

每帧每像素生成 1 个新的主射线样本。使用像素身份、帧序号和采样维度构造确定性的随机序列；连续帧不能重复同一随机样本，分支不能错误共享所有维度。降噪历史不是继续累加同一个样本。

scene-linear RGB 中完成光照和 throughput 运算：

- diffuse：Lambert，余弦加权半球采样。
- rough_metal：GGX NDF、height-correlated Smith masking-shadowing、以线性基础色为 F0 的 Fresnel-Schlick，使用 GGX VNDF 采样；没有漫反射分量。alpha 由 roughness² 得到，稳定化不得把整片粗糙金属变成镜面。
- mirror：离散理想反射事件，以线性基础色调制反射 throughput；不能用非零粗糙度近似替代。
- dielectric：光滑空气/玻璃界面，使用非偏振 dielectric Fresnel、Snell 折射及全反射；采用 radiance transport 对应的 eta² 修正。
- emissive：外侧单面发射，不再散射；从正确一侧直接看到发光面时保留 emission。

材质求值、采样概率和 PDF 使用一致的约定。非 delta 事件更新为 `throughput *= f * abs(dot(n,wi)) / pdf`；delta 事件使用相应离散权重，不能套用非 delta PDF。

每条路径最多处理 **8 次表面散射事件**，镜面反射和玻璃折射/反射都计入。预算内下一段命中 emissive 或 miss 可以完成贡献，不再生成第 9 次散射。自第 4 次散射后对仍能继续的路径执行 Russian Roulette：以 throughput 最大 RGB 分量给出 survival probability，限制在 [0.05,0.95]；存活时除以该概率。禁止在低 throughput 时直接无补偿地截断。

环境为黑色，没有额外方向光、常量环境光、AO 乘数、预烘焙 GI 或人工补光。全部照明来自实际发光块与路径传播。

### 2 玻璃介质与分支

玻璃体中的每段传播按实际长度 d 施加 `exp(-absorption * d)`。进入/离开由几何法线、射线方向和介质状态决定，不使用 shading normal 或材质名称推测边界方向。相机位于玻璃体内时必须正确初始化介质状态。

在每个主样本首次到达玻璃界面时，确定性拆分反射与透射贡献：反射分支乘 F，透射分支乘 `(1-F)` 和 eta²；全反射仅生成反射分支。首次拆分前已有贡献只记一次，不能随分支复制两遍。后续玻璃界面不再次扩大分支数，按 Fresnel 概率随机选择并进行概率补偿。因此每个主样本最多同时保有两个后续分支，队列容量据此设计。

玻璃不是 alpha blending。反射与透射不得共用一个随机选中分支的混合 guide 冒充两份信号；每个已生成分支保存独立贡献、光学链、介质状态和 guide。

常规 NEE 的直线 shadow ray 遇到玻璃即视为该直线连接被遮挡，不把折射体改成无方向变化的半透明 shadow。经过 delta 折射/反射实际命中发光面的路径仍按积分规则计能；不要求专门构造穿越玻璃的 manifold NEE，也不要求低采样下形成清晰焦散。

### 3 直接光与重复计能

直接可见、非 delta 的主表面使用第九节的 ReSTIR DI。镜面链或玻璃链后的终端、以及更深的非 delta 顶点使用自研发光面 NEE 与 BSDF 采样的 power-heuristic MIS，不在所有反弹上建立另一套 ReSTIR GI。

ReSTIR 顶点的直接光由 reservoir estimator 独占：从该顶点的 BSDF continuation 下一段直接命中发光面的贡献不再次计入；若该路径经过了 delta 事件后才命中光源，保留对应贡献。常规 NEE 顶点的 emitter hit 则使用与 NEE 一致的 MIS 权重，delta 链后的 emitter hit 权重为 1。不能将 ReSTIR 最终样本当作原始 light proposal 直接套用普通 NEE 的 PDF。

光源选择 PDF、面积 PDF、方向 PDF 与几何项必须在相应 measure 下保持一致。相交使用与世界尺度相称、法线侧正确的射线偏移，避免自交而不跳过真实薄面。

## 八 GPU wavefront 与路径队列

实际渲染至少分为 primary、hit/shading、continuation/shadow、queue compaction、下一反弹参数生成和 resolve 阶段。具体可以合并兼容的 GPU pass，但不能将全部反弹藏在一个 raygen 的长循环中。

- hit/miss shader 取得几何事实并写入路径状态，scatter/queue 操作由 shader 执行；多反弹跨 dispatch 推进。
- 维护双缓冲活跃路径队列，状态包括 pixel/sample/branch 身份、射线、throughput、散射次数、介质、光学链和所需计能信息。
- 终止路径不再进入下一队列；新队列必须连续紧凑，不保留整幅图大小的空洞数组充当“压缩”。可使用原子 append 或 GPU prefix scan，不能依赖 CPU 整理。
- GPU 依据真实活跃计数生成 `VkTraceRaysIndirectCommandKHR`，后续反弹使用 `vkCmdTraceRaysIndirectKHR`。间接维度满足设备 limits，多维展平与 padding 正确；零活跃路径可以合法零 dispatch，或采用最小 dispatch 加 early-out，但不得读取队列外数据。
- 计数写入、队列写入、间接参数写入、indirect read 和 shader read 建立对应执行与内存依赖。不能使用每次反弹 CPU readback、fence wait 或 queue/device idle 决定 dispatch 大小。
- Shadow ray 使用独立任务或独立求交 dispatch；RT recursion depth=1，不能在 hit shader 中嵌套 shadow trace。
- 路径与 shadow 队列容量覆盖分支数与每阶段最多生成的任务数；计数溢出不能通过丢弃路径或覆写别的像素伪造完成。允许分批处理，但各批贡献完整。
- 同一像素两分支的 radiance 通过独立 slot 后 resolve，或其他具有合法同步的归约累加；不要求浮点原子扩展，也不能用无同步并发写制造 race。

初始化和窗口重建可重新分配容量。固定尺寸稳态复用路径、shadow、reservoir、guide、降噪 ping-pong buffer 和 command 资源，不逐帧创建/销毁 Vulkan 对象。

## 九 自研 ReSTIR DI

本题范围固定为 **直接可见非 delta 主表面的发光面直接光**。镜中表面、玻璃后表面与后续反弹的直接光使用上一节规定的 NEE/MIS。ReSTIR 不重采样完整间接路径。

### 1 光源域与候选

从实际提取的暴露 emissive 三角形建立唯一 light table，删除的内部面不得继续充当光源。以三角形面积乘 emission 亮度建立非零光源功率 CDF；在选中的三角形上均匀采样面积。不同三角形拼成一个方块时不能重复计入同一发光面积。

每个有效像素生成 **4 个新的候选**。候选保存稳定的光源三角形身份与 barycentric 坐标，不只保存某个世界空间方向。目标函数为当前 shading point 下未计 visibility 的 RGB 直接光贡献亮度，必须包含 BSDF、接收端余弦、发光端余弦及距离几何项，并与 proposal 使用同一面积 measure。

初始 reservoir 使用 `w = target / proposalPdf` 做 weighted reservoir sampling，保存选择的样本、weight sum、有效候选数 M 和归一化信息；零权重候选、空 reservoir 和零 target 的行为有明确数学定义，不产生 NaN。

### 2 时间与空间复用

- Temporal reuse 将上一帧 reservoir 按无 jitter 相机和 primary 表面 guide 重投影。核对表面身份、材质、位置/深度与几何法线；遮挡揭露、投影变化、camera cut、resize 时按第十一节拒绝历史。
- 上一 reservoir 的有效历史 multiplicity 上限为 32。缩减 M 时同步缩减代表的 weight sum，保持已有归一化意义；不能只改 M 再产生增亮。
- Spatial reuse 从已完成 temporal 阶段的只读 reservoir buffer 选择 **4 个邻居**，半径最多 8 像素，偏移随像素/帧变化，拒绝越界及不兼容表面；不能边读边覆写同一阶段输入。
- 所有 reuse 都在目标像素重新求值 target，保持 source reservoir 的归一化、multiplicity 和 sampling measure；不得把邻居亮度直接做平均。
- 必须采用 ReSTIR DI 原论文中具有正确支持域校正的无偏复用归一化，或数学等价方案。重新选择样本后，对各参与 source 的 target 支持进行相应校正；不能把简单累加 M 的有偏近似宣称为无偏实现。短历史上限本身不替代归一化推导。
- 复用后在当前 shading point 对最终选中光样本执行真实 shadow visibility，光源采样结果不能无条件通过遮挡。历史 visibility 不能直接替代当前 visibility。

时间和空间 pass 的输入/输出以及跨帧 reservoir 必须分开或有明确的无 race 访问协议。Reset、离开/进入有效 primary 表面时同步处理 reservoir 内容和历史身份。ReSTIR 权重不可用任意亮度 clamp、最终乘常数或独立插值系数掩盖计能错误。

## 十 SVGF 与光学信号分域

### 1 输出与 guide

降噪前保存本帧新的、scene-linear HDR Monte Carlo 贡献。至少区分以下逻辑信号，可以共享物理存储但不能混用历史语义：

| 信号 | guide 与处理 |
| --- | --- |
| 直接看到的 emission/黑色环境 | 独立解析合成，不向周围表面做空间模糊 |
| 普通不透明主表面的漫反射 | 主表面 guide，albedo demodulation，SVGF |
| 普通不透明主表面的粗糙高光 | 主表面及 roughness guide，独立历史与方差，SVGF |
| 纯平面镜链后的贡献 | 反射空间 guide；漫反射/高光按终端模型区分 |
| 首次玻璃拆分产生的反射分支 | 独立光学链 guide 和历史；纯反射链可使用反射空间重投影 |
| 首次玻璃拆分产生的透射分支 | 独立光学链 guide 和历史；相机移动时按第十一节仅拒绝此类透射历史 |

首次玻璃反射分支后续若发生透射，其信号也归入透射历史规则，不能因为最初选择反射就错误复用纯反射历史。

每个 guide 至少携带表面/材质身份、位置或等价深度、几何法线、roughness、前后帧对应信息、有效标志；光学信号再携带光学链身份、链长度、终端身份和相关 hit distance。Guide 必须与当前信号贡献的实际路径对应，不用一次普通 primary hit 伪造所有域的 guide。

链到达首个非 delta 表面、发光面或 miss 时形成终端。若超过路径预算、未得到有效终端或链拓扑与历史不一致，该信号不能声明历史有效。没有有效 guide 的贡献仍应计入本帧结果，不能通过丢弃 radiance 掩盖导引失败。

### 2 SVGF 必需阶段

自行实现论文的 SVGF 核心流程，至少包括：

1. **时间重投影和有效性判定**：按对应信号的运动映射寻找上一帧，深度/位置、法线、材质/表面和光学链共同判断历史；不能无条件双线性采样整幅历史图。
2. **时间累积与亮度矩**：独立维护一阶矩、二阶矩和 history length。历史无效时从当前样本初始化；有效时按有限历史长度混合，历史上限 32 帧，当前新样本权重不得小于 1/32。
3. **方差估计**：由 `max(m2-m1*m1,0)` 得到时间方差，并为少于 4 个有效历史样本的像素计算由几何 guide 引导的局部空间矩/方差，避免把初始单样本误判为零噪声。
4. **方差引导的 à-trous 滤波**：5 次迭代，步长为 1、2、4、8、16，基础一维 kernel 为 `[1,4,6,4,1]/16`，二维使用可分离乘积；权重同时考虑几何/光学兼容性、位置/深度、法线、roughness 和方差归一化的亮度差。
5. **重建与合成**：将 demodulated 漫反射重新乘 albedo，独立合成各信号与解析 emission。Demodulation 对零 albedo 分量有确定的零贡献处理；不能直接把 RGB 除以任意 epsilon 后制造不存在的亮度。

每次空间迭代使用独立输入/输出，传播方差时使用与归一化滤波权重一致的平方权重规则，不能仅模糊 radiance 而永久保留初始方差。边界和无效邻域重新归一化。过滤权重、深度容差与数值稳定参数自行选择，但在源码中说明其单位、尺度和设计依据；不按特定像素、固定机位或材质 ID 硬编码画面修补。

时间历史保存 temporal 累积结果及其矩，不把最终 5 轮强空间模糊结果无限反馈到下一帧，避免递归过度模糊。SVGF 是有偏重建，不要求最终降噪图无偏；但不得通过更改原始积分贡献修补过滤错误。

## 十一 平面镜 玻璃与历史生命周期

### 1 普通表面

静态几何使用同一世界表面位置和前一帧相机建立 motion，普通相机移动保留有效的局部历史。新揭露区域或不兼容表面只拒绝对应像素，不因为摄像机每帧移动就清空整幅图。

### 2 平面镜与纯反射光学链

支持预算内的平面镜链，包括可见镜间反射，不要求曲面镜。对每条纯 delta 反射链，将终端位置按反射面逆序反射到虚拟世界空间，终端法线以相同变换处理，再投影至前一帧无 jitter 相机建立反射空间 motion。不能用第一块镜面的实体位置 motion 或终端实体的普通投影代替。

验证前后帧的链身份、各反射面、终端身份、虚拟位置/深度和法线；终端 roughness/albedo 用于对应过滤域。镜面边缘、链长度变化、镜后新揭露区域和 miss/表面切换拒绝相应历史。镜面本身平静不能成为镜中内容历史有效的唯一依据。

对首次玻璃拆分的反射分支，若至终端全部是纯平面反射，按同一反射空间规则处理；发生透射则采用下述透射规则。反射 guide 只指导过滤与重投影，不修改实际积分射线、Fresnel 或辐亮度。

### 3 玻璃透射

本题明确采用有界方案，不要求相机运动下的真实折射映射重投影：

- 相机的位置、朝向、FOV 或 projection 任一改变时，**拒绝当帧所有透射域的时间历史**，从当前透射贡献和矩重新初始化；继续进行独立、光学 guide 引导的空间方差估计及 à-trous 滤波。
- 相机静止时，使用相同像素光学路径的对应与 jitter 补偿，验证玻璃 entry/exit 面、介质/分支、链拓扑、终端身份和深度/法线后复用透射历史。相同 primary 前表面不是充分条件。
- 相机静止指无 jitter 相机状态不变。帧序号、子像素 jitter、随机采样或光学分支变化不属于相机运动；其中光学分支变化仍须按链匹配拒绝对应历史。
- 透射信号过滤必须拒绝不同玻璃轮廓、entry/exit 拓扑及不兼容终端，防止玻璃外背景、反射分支和玻璃内透射互相渗色。
- 不能始终关闭透射域时间累积；静止观察的兼容像素应逐帧建立有效历史。也不能因玻璃存在或相机移动而清空普通表面、纯镜面和 ReSTIR 的全部历史。

### 4 全局失效与帧所有权

R/camera cut、framebuffer extent 变化、projection/FOV 变化或实际帧资源重新初始化时，所有相关 guide、reservoir、矩和 history length 同步失效。普通连续相机移动仅按各域局部规则处理。

本帧输入历史、当前相机、上一相机与 reservoir/guide 的帧身份一致。只有成功提交的渲染工作才能推进前后帧配对；初始化、acquire 失败或跳过零尺寸帧不能伪造一份已渲染历史。历史必须来自真正完成或具有显式 GPU 顺序依赖的前一渲染帧，不能因为 CPU 使用双 frame slot 就默认 slot 中保存的恰好是上一帧。

## 十二 GPU 同步 呈现与资源释放

至少支持两个 CPU frame slot，不要求同时执行两份相互独立的成像历史。相邻帧的历史依赖可以在 GPU 上有序执行；稳态不使用全局 idle 代替资源同步。

必须建立并可从代码审查确认以下依赖：

- host/staging write → upload → shader、BLAS build；
- BLAS build → compact query/copy → TLAS build → trace；
- ray tracing shader write → compute shading/compaction；
- compute 参数 write → draw-indirect read，queue write → 下一 trace shader read；
- shadow result → radiance resolve；
- ReSTIR initial → temporal → spatial → visibility/shading；
- trace/resolve → temporal moments → 每轮 à-trous → 最终显示；
- 上一帧 history write → 下一帧 history read；
- 最终附件写入 → present，以及 acquire → 附件使用。

Timeline semaphore 可管理 GPU 工作完成与资源回收；交换链 acquire/present 使用符合 WSI 要求的 binary semaphore，不能认为 timeline completion 等价于 presentation engine 已释放交换链图像。Frame slot 和交换链图像身份分别管理，避免在 present 尚未消费时重用 semaphore。

交换链优先选择 sRGB UNORM 格式，使用 `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`；缺少可用 sRGB 交换链格式时报告失败，不暗中引入另一套 gamma 输出路径。最终显示采用全屏三角形，在 scene-linear HDR 上使用固定 Reinhard `c/(1+c)` 映射，曝光为 1，写出线性 LDR，由 sRGB attachment 完成编码；不能在 shader 再做一次 gamma。色调映射只出现在所有光学域合成之后。

初始化、交换链重建与最终退出允许必要的等待安全点；不在普通每帧或每次反弹调用 `vkDeviceWaitIdle`/`vkQueueWaitIdle`。资源生命周期覆盖所有在途 command、AS/SBT、descriptor backing、history 与 swapchain 使用；关闭按完成依赖排空并逆序释放，不泄漏 Vulkan/GLFW 资源。

使用 Vulkan Validation Layer 的 core 和 synchronization validation。warning、error 和诊断 message 按失败处理，INFO 仅记录；不关闭、过滤或修改验证层以掩盖问题。编译器与 shader 编译器的 warning、error 和诊断 message 同样按失败处理，正常构建进度不属于诊断。

## 十三 必需优化机制与禁止替代

优化的验收是确认机制正确接入，不比较运行速度，也不要求证明某个优化在本机更快。

| 必需机制 | 正确性检查 |
| --- | --- |
| 内部面消除与 greedy meshing | 表面范围、法线、UV、材质与玻璃边界保持正确 |
| 静态 BLAS compaction | 真实 query/copy、地址更新与旧资源完成后释放，稳态不反复构建 |
| GPU 活跃路径压缩及间接调度 | 每次反弹只处理活跃状态，分支、padding、计数与容量无漏算或 race |
| 发光面功率采样与 ReSTIR DI | proposal、target、reuse normalization、当前 visibility 与计能一致 |
| GGX VNDF 与 ray footprint mip | sampler/PDF 匹配，UV 和颜色空间不随合并失真 |
| 分域时间复用与 SVGF | 当前样本持续进入，普通/反射/透射使用各自合法历史 |
| 持久资源复用与显式同步 | 固定尺寸稳态无重复创建，历史及在途资源无提前覆写 |

禁止静态截图、预生成帧、软件窗口绘制、离线烘焙光照、屏幕空间反射、传统光栅主可见性、单次直接光或随机模糊冒充本题。禁止空调用或从未消费的队列/descriptor/reservoir/方差数据冒充机制接入；禁止硬编码固定相机下的材质修补、曝光补偿或光照结果。

## 十四 交付与验收

### 1 交付内容

- 在当前测评工程中完成 C++ 与 GLSL 实现，保留随题 `materials.json` 作为运行时资源。
- 提供简短实现说明，标明实际 pass 顺序、队列与分支容量依据、ReSTIR 归一化方案、SVGF guide/历史规则和同步/资源所有权。可写在 `IMPLEMENTATION.md`，不要求额外框架或自动验收工具。
- 按已有工程入口进行与改动相称的构建及运行检查。Windows 使用 `windows-msvc-debug`/`windows-msvc-release`，Linux 使用 `linux-gcc-debug`/`linux-gcc-release`；目标平台声明为两者。
- 哪个平台未构建、哪些交互检查未运行，必须如实说明；不能以一个平台成功声称完成两平台核验。

### 2 验收方法

以构建、代码审查和真实交互运行综合验收，不要求命令行采集或高 SPP 参考模式，不规定 FPS、内存或统计图像质量分数门槛。仅有截图或录屏不足以验证算法。

| 检查 | 必须满足的行为或证据 |
| --- | --- |
| 工程与平台 | C++20、限定依赖、跨平台路径与 API；实际运行平台的构建及 shader 编译无诊断 |
| 初始场景 | 格子范围、镜面、连续玻璃、柱/阶梯、顶板开口和 4 个真实发光块符合规格 |
| 材质外部来源 | 改变合法基础色、粗糙度、IOR、吸收或 emission 并重启影响对应结果；缺失/错误配置明确失败 |
| 多反弹 | 被直接光遮挡的普通表面能获得合理间接照明，彩色墙影响邻近表面；代码中是真实 throughput/PDF 多反弹 |
| 直接光 | 发光面造成有遮挡的软阴影；ReSTIR 候选、时间/空间复用和当前 shadow 查询实际参与最终结果 |
| 金属与镜面 | 粗糙金属高光随视角变化；平面镜显示屏幕外几何及预算内镜间反射，移动时使用虚拟表面历史 |
| 厚玻璃 | 同时体现反射、折射与实际厚度吸收；能正确处理 entry/exit、相机进入玻璃和全反射公式，不以 alpha 替代 |
| 静止降噪 | 原始新样本进入实际 SVGF，兼容普通、反射和透射信号逐帧建立历史；不以冻结随机种子或停止采样冒充稳定 |
| 相机移动 | 普通/纯反射有效历史继续重投影，新揭露区域不拖旧内容；透射历史当帧拒绝而空间滤波继续，不全局 reset |
| 轮廓与合成 | 墙边、镜边、玻璃边和发光面不过度跨域混合；最终合成无明显重复计能、gamma 错误或持续变亮 |
| wavefront 与优化 | 有真实紧凑 GPU 队列、间接参数、静态 BLAS compact copy、greedy meshing 与 footprint mip 消费路径 |
| 窗口生命周期 | 缩放、高 DPI、最小化恢复、R、捕获/释放鼠标、关闭正常；core/sync validation 无失败诊断 |

交互检查至少包含：从初始位置进入展厅；横向移动观察遮挡揭露；转向两个镜面观察镜中内容；在玻璃前静止观察再移动；穿入并离开玻璃；缩放/最小化/恢复；R 重置；正常关闭。

低采样本身允许残余噪声，特别是移动中的透射信号及未专门求解的焦散。本条不允许忽略持续重影、轮廓漏色、能量错误、漏算分支或未接入降噪；这些是机制正确性问题。判定是否完成仍以具体合同和实际代码/运行行为为依据。

### 3 完成标准

上述必选机制必须全部实现并接入实际渲染路径；构建和验证结果只是证据，不替代算法与行为要求。缺少某个机制时报告具体缺口，不通过降级路径、修改依赖/配置或删除验收条款将部分实现标为完整完成。
