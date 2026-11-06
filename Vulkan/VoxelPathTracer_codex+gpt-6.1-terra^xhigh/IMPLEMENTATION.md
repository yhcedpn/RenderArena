# 实现说明

本答卷尚未实现渲染器，不满足题目的完成标准。程序在准备 CPU 数据并检查部分 Vulkan 能力后明确报错退出，不显示替代画面。

## 现有实现

启动时从 SDL3 指定的可执行文件目录读取 `materials.json`，检查必填字段、类型、数值范围、材质语义、重复 ID 和场景使用的 ID。按配置种子生成可重复的基础色及粗糙度纹理；石材使用周期性多尺度 value noise，木材使用沿 U 延伸的条纹和低频扰动。基础色 mip 在线性空间平均并重新编码，粗糙度 mip 在线性域平均。数据目前仅保存在 CPU 内存，未上传或采样。

场景按题面覆盖顺序填充不透明格子，删除相邻不透明格的内部面，按轴、方向、平面和材质扫描并贪心合并矩形；UV 使用世界坐标，保持每单位重复。玻璃单独生成偏移后的闭合长方体，保留底面及其下的地板顶面。网格仅保存在 CPU 内存；没有构建光源表或加速结构。

经 volk 加载 Vulkan loader，检查 loader 和物理设备的 Vulkan 1.4 版本、指定的主要设备扩展和部分 feature。该检查尚未涵盖 surface/present 支持、全部依赖、Shader Object 所需动态状态、格式和设备 limits，也未创建逻辑设备或启用 feature，不能代表完整能力合同。

## 实际执行顺序与缺口

当前顺序为读取并校验材质 → CPU 生成纹理和 mip → CPU 提取网格 → 查询部分 Vulkan 能力 → 报告未实现并退出。没有 GPU pass、队列、分支容量、历史、同步或 GPU 资源所有权。以下要求均未实现或未接入：

- SDL 窗口、摄像机、输入、交换链、呈现、缩放和帧率标题；
- Triangle BLAS/TLAS、压缩、SBT、RT pipeline、Descriptor Buffer、Shader Object、动态渲染与 Synchronization 2 的实际调用路径；
- 光源功率表、GPU wavefront 活跃队列、间接 trace、路径积分、BSDF、NEE/MIS、玻璃拆分及介质吸收；
- ReSTIR DI 的候选、时间和空间复用、无偏归一化与当前 visibility；
- 各光学域的 guide、反射空间重投影、透射历史规则、SVGF 和最终合成；
- GPU 上传、纹理采样和 ray footprint mip，以及帧资源生命周期、历史依赖和 WSI 同步；
- Debug 的 Vulkan core/synchronization validation。

## 核验

在 Windows 上使用 CMake 4.3.1-msvc1、MSVC 19.51.36260.0，分别配置并构建 `windows-msvc-debug` 和 `windows-msvc-release`，均成功且未见编译诊断。没有 shader 文件，因此未执行 shader 编译；已安装的 glslc 报告 shaderc v2026.4。

Debug 和 Release 启动均读取 8 个材质，生成每材质 8 级 mip 和 230 个场景三角形；所列的部分设备能力检查通过，随后按未实现渲染器报错并以状态码 1 退出。额外检查缺失材质文件、重复 ID 和超范围 roughness，均报告错误并以状态码 1 退出。上述结果不证明网格、纹理或能力合同的全部正确性。

Linux 未构建或运行。未创建窗口，题面列出的全部相机、镜面、玻璃、降噪、窗口生命周期及画面交互检查均未执行；没有窗口截图或人工目视检查。不存在实际渲染，因此也没有 Vulkan validation 运行结果。
