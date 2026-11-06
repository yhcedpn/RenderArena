# VoxelPathTracer 实现说明

## 当前实现

程序从可执行文件目录读取 `materials.json`，验证版本、字段、范围、材质类型约束和场景所需材质 ID。基础色与粗糙度纹理在 CPU 生成，包含确定性 stone/wood 噪声及完整 mip 链；颜色 mip 在 scene-linear 中平均后重新编码，roughness mip 在线性标量域平均。纹理数据目前只保存在 CPU 内存，尚未上传或被渲染器采样。

CPU 场景按题面顺序建立体素占用表，提取不透明方块的暴露面并按固定平面扫描顺序执行 greedy meshing。合并矩形的 UV 长度等于对应世界尺寸，静态表面身份由平面、材质、方向和矩形范围生成。玻璃单独作为整体闭合长方体提取，边界为 `[7,9] × [0.125,2.125] × [7,9]`，不参与方块邻接删除。最终暴露的发光三角形按面积乘发射亮度建立 CDF。

有效配置的运行输出为 8 种材质、460 个顶点、230 个三角形和 40 个发光三角形。此输出只核对资源解析及 CPU 场景生成；程序随后以退出码 2 报告渲染后端未实现。

## 实际执行顺序

```text
定位可执行文件目录
  → 读取并验证 materials.json
  → 生成 CPU 材质纹理及 mip
  → 建立体素布局
  → greedy meshing 与独立玻璃边界提取
  → 建立发光三角形面积功率 CDF
  → 报告尚无 Vulkan 渲染后端并退出
```

当前没有 GPU pass、光线队列或 Vulkan 资源。因此没有路径队列/玻璃分支容量依据、ReSTIR DI 重用归一化、SVGF guide/历史规则、帧资源所有权或 Vulkan 同步方案可供核验。这些内容不能从现有 CPU 场景代码推定为已实现。

## 构建与核验

- Windows `windows-msvc-debug`：配置与构建通过，未出现编译诊断。运行成功解析资源并生成上述场景计数；程序按设计返回退出码 2，因为后续 Vulkan 渲染器不存在。
- Windows `windows-msvc-release`：配置与构建通过，未出现编译诊断。运行得到相同资源计数并以退出码 2 结束。
- GLSL 未实现，因此没有 shader 编译结果。Vulkan 能力查询、验证层、光线追踪、交换链呈现、窗口交互及 Debug 同步验证均未运行。
- Linux 未构建。
- 未进行窗口截图或人工目视检查；当前程序不会创建窗口。

## 未完成要求

尚未实现 Vulkan 1.4 能力检查与设备创建、Descriptor Buffer、Shader Object、RT pipeline、SBT、BLAS compaction、TLAS、wavefront 活跃队列及 indirect trace、BRDF/路径积分、玻璃分支与吸收、NEE/MIS、ReSTIR DI、分域 SVGF、时间重投影、最终动态渲染与呈现、WASD/鼠标相机、FPS 标题栏、resize/minimize 生命周期及资源同步释放。纹理也未上传到 GPU。因而题面规定的渲染和交互验收均未完成。
