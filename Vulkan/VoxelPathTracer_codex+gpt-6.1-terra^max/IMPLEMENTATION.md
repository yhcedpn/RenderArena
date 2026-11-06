# VoxelPathTracer 实现说明

本实现尚未满足题面全部必选要求。下文区分已接入的渲染路径、实际核验和已知缺口。

## 场景与资源

程序从可执行文件目录读取 `materials.json`，校验字段、材质类型约束和场景引用。CPU 按配置生成确定性的颜色和粗糙度纹理；颜色 mip 在线性空间平均后编码回 sRGB，粗糙度 mip 在线性空间平均。GPU 使用线性、trilinear、repeat 采样，射线传播时扩展的 ray cone 决定显式 mip。

不透明格子的暴露面按平面、法线和材质做 greedy meshing；玻璃独立生成为上移后的闭合长方体。每个合并面有稳定的 surface ID。发光表来自最终暴露的 emissive 三角形，以面积乘发射亮度建立 CDF。默认资源生成 230 个三角形，其中 40 个为暴露发光三角形。

按材质分组建立三角形 BLAS，实际查询压缩大小并执行 compact copy，再由压缩后的地址构建 TLAS。原始 BLAS 和 scratch 在构建完成后释放；纹理、几何、材质、光源表、压缩 BLAS 和 TLAS 在固定场景期间复用。

## 帧流程和积分

每帧实际调用顺序：

```text
primary compute → 主射线 trace
  → ReSTIR initial → temporal → spatial → 当前 shadow trace → resolve
  → scatter → 后续活跃队列参数生成
  → [indirect trace → scatter → shadow indirect trace → resolve → 参数生成] × 后续反弹
  → 普通信号 temporal/moments → variance → 5 次 à-trous
  → 光学信号 temporal/moments → variance → 5 次 à-trous
  → 全屏三角形、Reinhard、sRGB attachment → present
```

RT pipeline 的递归深度为 1。每帧每像素生成一个抖动主样本；每个路径队列容量为 `2 × 像素数`，shadow 队列同为 `2 × 像素数`。首次玻璃界面最多拆成反射和透射两条路径，原子 append 连续写入下一队列；后续 dispatch 参数由 GPU 计数生成。两条分支写入独立辐亮度槽，首次拆分前的贡献保存一次。

散射实现 Lambert、GGX VNDF、理想镜面和 dielectric Fresnel/Snell；玻璃内部按传播距离应用吸收。最多进行 8 次散射，第 4 次后对继续路径执行有补偿的 Russian Roulette。直接可见非 delta 主表面由 ReSTIR 独占直接光；其 BSDF continuation 若立即命中发光面则不重复计能。后续非 delta 顶点使用发光面 NEE、独立 shadow trace 和方向 measure 下的 power-heuristic MIS。环境为黑色。

## ReSTIR 与 SVGF

初始 reservoir 对每个有效主表面抽取 4 个三角形面积候选，候选保存三角形身份和重心坐标，权重为 `target / areaPdf`。时间复用对上一 reservoir 的 `M` 限制为 32，并按相同比例缩减其 weight sum；空间阶段从 temporal 输出读取 4 个半径不超过 8 像素的邻居。各源在目标位置重新求值：源权重为 `targetCurrent × Wsource × Msource`，选择后归一化分母为 `targetCurrent(selected) × Σ(Msource × 1[targetSource(selected) > 0])`。最终样本每帧重新查询 visibility。

普通主表面使用其位置、几何法线、材质和 surface ID 做重投影；漫反射先按非零 albedo 分量 demodulate，粗糙金属保留独立类型语义。时间历史保存颜色和一、二阶亮度矩，长度上限 32；短历史使用邻域矩估计方差。5 次 à-trous 的步长为 1、2、4、8、16，核为 `[1,4,6,4,1]` 的二维乘积，并以归一化权重的平方传播方差。历史保存 temporal 结果，不反馈最终空间滤波结果。

光学分支记录实际经过的反射/透射面、方向侧和终端身份。纯反射链将终端位置与法线按各反射面逆序映射到虚拟空间；历史比较逐项核对光学事件及终端；miss 终端记录方向与链身份。含透射或非 delta 前缀的链只在相机静止时尝试时间复用，并对两帧 jitter 做补偿；反射、透射和普通信号按各自 guide 和历史过滤。当前没有有效 guide 的贡献仍以未滤波样本合成。

## 同步与所有权

所有 shader 资源使用 Descriptor Buffer，布局大小、binding offset、描述符大小和对齐来自设备查询。Compute 和最终 vertex/fragment 使用 Shader Object；最终附件使用 Dynamic Rendering。上传、AS 构建/压缩、各 trace、compute、indirect 读取和显示之间以 Synchronization 2 建立依赖，队列提交使用 `vkQueueSubmit2`。

两个 CPU frame slot 各自持有 command pool、acquire binary semaphore 和 fence；present semaphore 按交换链图像分配。前后帧共用成像缓冲，由同一 GPU 队列及帧起始屏障排序。前帧相机只在成功提交后推进；camera cut 和资源重建使相关历史失效。初始化、交换链重建和退出使用等待安全点，稳态帧循环不调用全局 idle。

## 实际核验

- Windows 11：`windows-msvc-debug` 与 `windows-msvc-release` 均配置和构建成功，编译器及 shader 编译无诊断。使用 CMake 4.3.1-msvc1、MSVC 19.51.36260、shaderc/glslc 2026.4、Vulkan Headers 1.4.350、SDL 3.4.12 和 nlohmann-json 3.12.0。Debug 生成的所有 stage SPIR-V 文件头均为 1.6。
- 最终 Debug 和 Release 各短时运行并通过进程请求关闭，均返回 0，分别呈现 94 帧和 97 帧。Debug 启用 core 与 synchronization validation，未观察到 warning/error；Release 未启用校验层。
- 文件缺失、JSON 错误、版本不支持、重复 ID 和 diffuse 材质使用极小非零 metallic 均明确失败。合法的接近 1 的 dielectric IOR 已通过输入语义检查；该检查随后因测试目录没有 shader 而退出，没有作为画面验证。
- 对两个不完全重叠的离散支持域做枚举：上述支持域校正的期望为 6，与目标积分 6 一致；固定源数分母得到 4。该例只检验归一化公式的一个离散情形，不验证整个 GPU 实现的计能。
- Linux 未构建或运行。没有采集窗口图像，也没有进行目视验收。

## 未完成与未核验

- 首次玻璃事件发生在普通主表面的多次后续散射之后，或光学终端之后又经过未记录的其他散射再进入玻璃时，当前实现不为该后续段声明有效光学 guide。多个较早光学段的贡献保存在未滤波的 prefix 中；它们没有各自独立的 SVGF 历史。因此完整的深层反射/透射分域与历史要求尚未完成。
- 每帧缓冲按 framebuffer 全尺寸分配，约需每像素 1604 字节，不含几何、纹理和驱动开销；1280×720 约为 1.38 GiB。没有将超出 `maxStorageBufferRange` 或单次 dispatch 限额的尺寸分批处理，遇到该情况会报告失败并退出，尚未完整满足大尺寸 resize 的要求。布局需要每阶段 25 个 storage buffer 描述符，设备不足时会报告失败。
- 输入和 GPU 材质数据使用 float 表示：规范允许但超过 float 最大值的有限 emission/absorption 当前会被拒绝；极接近 1 的合法 IOR 可能在传给 GPU 时舍入为 1。
- 未实际交互检查进入展厅、横向移动与遮挡揭露、两个镜面、玻璃前静止和移动、穿入及离开玻璃、resize、最小化恢复、高 DPI、R、鼠标捕获/释放、Esc 和标题栏统计。进程请求关闭只验证了窗口关闭路径。
- 未通过画面或参考积分验证光照、MIS/ReSTIR 能量、镜间反射、厚玻璃吸收、降噪稳定性、轮廓漏色或运动重投影。构建和无校验诊断不能替代这些验收。
