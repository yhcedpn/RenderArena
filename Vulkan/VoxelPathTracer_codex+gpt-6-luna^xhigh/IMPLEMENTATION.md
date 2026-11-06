# 实现与核验

## 当前实现

程序从 SDL 提供的可执行文件目录读取 `materials.json`。材质加载器检查必填字段、版本、数值范围、类型约束、唯一 ID 及场景引用；之后以配置种子生成石材多尺度 Value 噪声、木纹和粗糙度数据，并建立完整 mip 链。基础色 mip 在 scene-linear 空间平均，粗糙度 mip 在线性标量域平均。

静态场景按题面规定的写入顺序构造体素格。表面提取按轴、法线方向、平面、V、U 确定性扫描，只将同材质的暴露面贪心扩成矩形；面内 UV 长度对应世界单位，矩形按材质分组并保存稳定的面 ID。玻璃不进入整数体素格，而以 `[7,9] × [0.125,2.125] × [7,9]` 六个外侧矩形单独建立闭合网格。

摄像机数学模块实现题面初始姿态、60° 垂直视场、水平移动速度、Shift 倍速、yaw/pitch 限制和按近平面投影修正的主射线最小距离；当前没有 SDL 事件循环将其接入窗口。

目前实际执行顺序为：

```text
SDL 可执行文件目录定位
  → 解析并验证 materials.json
  → 生成 CPU 纹理及 mip 链
  → 构建静态体素格
  → 贪心提取不透明表面并分材质分组
  → 建立独立闭合玻璃网格
  → 输出几何摘要并以失败状态退出
```

本工程尚无 GPU 渲染 pass、RT/compute shader 或 shader 编译产物。纹理和网格仅为 CPU 数据，没有上传或接入图形 API。程序主动以非零状态退出并说明缺口，避免把未实现的渲染伪装成完成的画面。

## 未实现要求

- Vulkan loader/instance/device 1.4 能力报告、feature 启用、Debug validation、设备队列选择及 swapchain 创建。
- Triangle BLAS/TLAS、BLAS 压缩、SBT、recursion-depth-1 RT pipeline、Descriptor Buffer 及 Shader Object；场景几何未上传至 GPU。
- 窗口、呈现、鼠标捕获、按键事件、缩放/最小化恢复、帧率标题、frame slot、WSI semaphore 与资源生命周期。
- GPU 路径积分：主射线随机采样、材质 BSDF/PDF、多反弹、Russian Roulette、厚玻璃介质传播与分支、NEE/MIS 和 shadow 查询。
- 活跃路径/阴影队列、GPU 压缩、indirect RT 参数及其容量与同步依赖。队列容量尚无实现依据。
- 发光三角形功率 CDF、ReSTIR DI 四候选及时间/空间重用。未实现 reservoir estimator，也没有可报告的 ReSTIR 归一化方案。
- 分域 guide、历史重投影、SVGF 时间矩、空间方差、五轮 à-trous、平面镜虚拟空间重投影及玻璃反射/透射历史。
- GPU staging、图像布局与屏障、timeline/binary semaphore 同步、Reinhard 显示和 sRGB 输出。

因此，当前不存在题面所要求的 GPU pass 顺序、队列容量依据、ReSTIR 无偏复用归一化、SVGF guide/历史规则或 GPU 资源所有权方案；不能据此答卷验收为完成。窗口未创建，题面列出的交互、视觉效果、验证层及运行期同步检查均未运行。未采集窗口截图。

## 构建与运行证据

- 构建环境：Windows、CMake 4.3.1-msvc1、MSVC 19.51.36260.0、Windows SDK 10.0.26100.0。
- Windows Debug：`windows-msvc-debug` 配置与构建成功；C++ 编译无诊断。题面要求的 Vulkan/shader 编译未发生，因为尚无 shader。
- Windows Release：`windows-msvc-release` 配置与构建成功；C++ 编译无诊断。Linux 未构建。
- Debug 与 Release 程序均读取了配置并构建场景，摘要一致：8 种材质、7 个不透明材质组、218 个不透明三角形、12 个玻璃三角形、40 个外露发光三角形。程序随后按上述未实现状态返回退出码 1。
- 当前核验只覆盖配置解析与 CPU 数据生成；构建成功不代表 Vulkan 能力、shader、画面或交互验收通过。
