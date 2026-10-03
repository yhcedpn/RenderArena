# MiniVoxelPathTracer 实现与核验

实现入口为 main.cpp，CPU 材质、纹理及 greedy meshing 在 scene.hpp，全部 GPU pass 为 shaders/ 内交付的 GLSL。材料数值只来自外部 materials.json。资源按可执行文件路径解析；CMake 原有部署入口复制材料和 shader。既有 CMake、preset、vcpkg、平台配置及 materials.json 未改动，VS 仅添加新文件引用。

## 实际执行顺序

初始化生成确定性 sRGB 基础色/线性粗糙度纹理及完整 mip，删除不透明内部面，按平面、朝向和材质 greedy 合并；玻璃直接生成平移后的闭合 2×2×2 边界。展厅和玻璃分成两个 Triangle BLAS，分别 allow-compaction、真实 compact-size query、compact copy，再以压缩地址构建两实例 TLAS。实测总计 230 三角形，发光表为提取后的 40 个三角形；BLAS 分别从 35828/2364 字节压缩到 12020/1212 字节。

每帧：primary/贡献与 guide 清理 → 第一次直接 trace → compute shading/原子 append → ReSTIR initial/temporal/spatial/最终 shadow task → GPU indirect 参数 → 独立 shadow trace/resolve。后续反弹通过双队列与 vkCmdTraceRaysIndirectKHR 推进，终止路径不 append。总共最多 8 次散射，另一次 terminal trace 允许预算内路径最终命中 emission/miss。最后为 SVGF temporal/moments → 短历史局部方差 → 5 轮 à-trous → 分域重建合成 → 全屏三角形 Reinhard → sRGB attachment。

RT recursion depth=1。closest-hit/miss 仅写距离及稳定三角形事实，scatter 与队列推进在 compute 中完成。Compute 和显示使用 VkShaderEXT；所有 26 个绑定都由 Descriptor Buffer 提供，包括每个实际消费的路径、shadow、guide、reservoir、history 和显示 buffer，不通过 shader BDA 绕过资源描述符。

## 容量、积分和直接光

N 为真实 framebuffer 像素数。首次玻璃界面最多替换为两个继续分支，之后不再次扩张，因此每个活跃队列、hit、shadow/result 均覆盖 2N。每个像素保留三个独立 radiance slot：拆分前、反射、透射；前缀贡献不会复制。每条活跃路径每阶段最多生成一个 NEE shadow，primary ReSTIR 与常规 NEE 互斥，因此 shadow 容量 2N 足够。计数、紧凑队列及两个 12 字节 indirect command 都由 GPU 写入。多维展平和末尾 padding 先检查活跃计数；零计数用 1 个 invocation 加 early-out。创建容量时检查 RT dispatch、compute group、storage range 和格式/layer/mip limits。

Lambert 与 GGX VNDF 的采样和 PDF 同源；GGX 使用 height-correlated Smith、roughness² alpha 和线性 F0。镜面为理想 delta；玻璃使用精确 dielectric Fresnel、Snell/TIR、radiance eta²，并按介质段实际长度吸收。第四次散射起实施有补偿的 RR。ray cone 在段传播、粗糙散射及 eta 变化时更新，世界单位 UV 与显式 textureLod 消费 footprint。普通 NEE 遇玻璃完全遮挡，独立 trace 查询真实几何。

ReSTIR 每个有效 primary 非 delta 点生成四个面积候选。CDF 为面积乘发光亮度，proposal=q(light)/area，target 是含 BSDF、两端余弦、距离平方几何项和 emission 的未遮挡 RGB 贡献亮度。初始权重 target/proposal；reuse 权重为 target_dest × W_source × M_source。选出样本后采用支持域校正：
W_dest = weight_sum / (target_dest(selected) × Σ M_source·1[target_source(selected)>0])。
源位置重新求值支持域，不能只累加 M。时间源有效 multiplicity 限 32，代表 weight sum 同比例缩小，W 的归一化意义保持；空间阶段只读 temporal buffer，从 x²+y²≤64 的全部 196 个非零整数偏移中无放回随机选择四个不同邻居；编号按整数圆盘逐行解码，不经过浮点圆周取整，因此实际欧氏半径严格不超过 8 且不包含自身。越界及不兼容表面仍按原规则拒绝。最终当前 visibility 经真实 shadow trace。空 reservoir/零 target 有零估计量定义；全部 emission 合法取零时保持黑场景，不引入默认光。ReSTIR 独占该顶点直接 emitter hit，delta 链后的 emission 保留；其他 NEE/BSDF 使用 power MIS。

## 光学 guide 和 SVGF

普通、纯镜面、首次玻璃反射、透射分别使用 domain 0/1/2/3，slot 及终端材质再区分独立历史和 diffuse/rough specular 模型。guide 包括实体/虚拟位置、法线、终端表面/材质、roughness、albedo、hit distance、介质、链长度及完整八个平面事件身份；反射和透射不共享随机选择的 guide。

纯反射链累计仿射平面变换，等价于对终端逆序反射，再以虚拟位置/法线投影到上一无 jitter 相机。投影补偿前后 jitter，并验证完整链与终端。透射在无 jitter 相机改变时仅拒绝自身时间历史；静止时使用像素/jitter 对应及 entry/exit 全链、终端和距离验证。随机非 delta 前缀之后首次遇玻璃的分支缺少合法纯反射相机映射，保存实际分支 guide 与当前贡献，不能把它声明为有效投影历史。

各域保存 temporal HDR、一/二阶亮度矩与长度，最长 32，当前权重至少 1/32。漫反射按终端 albedo demodulation，零分量为确定的零贡献；重建时再乘回。短于四样本的历史采用 guide 引导的 5×5 局部方差。à-trous 步长 1/2/4/8/16，kernel [1,4,6,4,1] 的二维乘积，结合拓扑、位置、法线、roughness 及方差归一化亮度权重，并按归一化平方权重传播方差。有效中心总有正权重。反馈仅保存 temporal 结果，空间滤波结果不递归反馈；直接 emission/miss 不向邻域模糊，无有效 guide 的当前贡献仍保留。

位置容差以世界单位及视距比例计，滤波尺度随实际像素足迹/步长增长；1e-8 只用于方差零除稳定。0.0002 世界单位偏移远小于玻璃与地板的 0.125 间隙。没有亮度 clamp、曝光补偿或材质 ID 画面修补。

## 同步和生命周期

单 graphics/compute/present family，资源 exclusive，无跨 family 转移。Sync2 屏障覆盖 host/staging、AS build/query/copy/TLAS、RT→shading、compute queue/indirect→trace、shadow→resolve、reservoir pass、temporal/每轮 filter/显示和 present。SBT、descriptor host write 有对应读依赖。两 CPU frame slot 的 command、frame 常量与 descriptor backing 持久复用；共享成像历史由 timeline 相邻提交依赖串行化。前后相机/jitter/帧身份只在成功 submit 后推进；acquire 失败、零尺寸跳帧不推进。

Acquire 使用 frame-slot binary semaphore；present semaphore 按交换链图像管理，重新 acquire 对应图像后才复用。稳态没有 device/queue idle，没有每反弹 CPU readback。帧资源等待用短 timeline timeout 继续处理 GLFW 输入。resize 和退出允许安全等待；cut/extent 重建同步清空所有历史。初始化临时资源带作用域回收；退出排空后逆序释放，并将退出期间 validation/释放失败计为失败。

按更新后的题面，仅 Debug（未定义 NDEBUG）请求 VK_LAYER_KHRONOS_validation、core/synchronization validation 和 debug utils messenger；Release 不请求校验层及 debug utils 扩展，也不创建 messenger。标题栏以 steady_clock 的正常帧循环墙钟时间与成功 present 帧数统计平均 FPS 和 ms/frame，约每 0.5 秒更新；时间包含资源等待，不是 GPU pass 独立计时。初始化、最小化和交换链重建不计入统计，暂停或重建后清空旧样本并显示占位符。

## 实际核验

- Windows Debug 与 Release 按原 preset 配置、构建；最终两套构建均无 warning/error。
- 本次 Debug/Release 校验策略和标题栏修改后，两套 Windows preset 重新配置、构建通过，无编译诊断。实际 Debug 加载一个 Khronos validation 模块，INFO 列出启用 Synchronization 等 core 检查；Release 加载零个 validation 模块，stderr 为空，Release 可执行文件中也不存在校验层名称字面量。两种模式均正常关闭并退出 0，9 个 GLSL stage 运行时编译通过。
- 两种模式均实测标题栏 FPS/ms 数值持续更新，窗口缩放后重新统计，最小化期间保持占位符，恢复后重新显示数值。运行日志为 out/title-validation-debug.stdout.log、out/title-validation-debug.stderr.log、out/title-validation-release.stdout.log 和 out/title-validation-release-exit.stdout.log；标题栏采样为 out/title-validation-debug-titles.json 与 out/title-validation-release-titles.json。
- Radeon 780M 实际通过 Vulkan 1.4/RT/Descriptor Buffer/Shader Object 必需能力查询；9 个 GLSL 文件运行时编译成功。调整构建模式前的运行开启 core 和 synchronization validation，最终运行及正常关闭无 warning/error；INFO 原样记录。
- spatial 半径修复后，按 shader 中的整数圆盘表穷举核验全部 196 个偏移：唯一、非零、完整覆盖 x²+y²≤64 的整数圆盘，最大平方半径为 64。当时 Release 仍开启校验层，重新构建通过；9 个 GLSL stage 运行时编译通过，实际渲染和正常关闭退出 0，core/synchronization validation 无 warning/error，日志为 out/spatial-radius-run.log。四次选择采用无放回编号映射；未改变越界及表面兼容性拒绝规则。删除了未使用的 shaders/.gitkeep。
- 实际执行进入展厅、横向移动、镜中内容观察、玻璃前静止/移动、进入玻璃、离开玻璃、resize、高 DPI framebuffer、最小化恢复、R、右键捕获/Esc 释放及正常关闭。
- 介质内实测相机位置 (8,1.744,8.430)，图像显示介质内折射/TIR；R 后恢复初始位置。静态镜面/玻璃 GPU 核查中 domain 0/1/2/3 的 history length 均达到 32，历史和输出 NaN=0。
- 曾发现零贡献 NEE 的 0/0 MIS 污染历史，已在零目标数学边界修复。非 RT SPIR-V 残留 AS 类型及 fragment NonWritable 声明的验证错误也已修复。
- 十一类配置拒绝检查均退出 1：缺失文件、版本、重复 ID、零金属 roughness、负吸收、非法 IOR、pattern、缺少场景 ID、纹理尺寸、种子、超 int32 范围的纹理尺寸。测试只改 out/config-checks 的复制资源。
- 复制部署中的合法配置改为绿色墙、roughness=0.4、IOR=1.6、新吸收与半强度 emission，重启成功，实际墙色和照明发生变化；该进程从答卷根目录启动，仍读取自身部署目录资源，验证了资源路径不依赖工作目录。
- 临时退出 readback/AUDIT 已从交付源代码移除。核验日志保存在 out/optical-audit.log、out/mirror-audit.log、out/config-checks/results.json 及 out/final-release-clean.log、out/legal-material-run.log；截图只是交互证据，不参与渲染。
- Linux 未构建或运行：本次为 Windows 主机，未提供 Linux 执行环境。Linux 构建及交互仍需在该平台核验；本实现采用 GLFW、标准 C++ 文件/时间接口与条件 CRT PATH 读取，未用 Win32 窗口/文件 API。
