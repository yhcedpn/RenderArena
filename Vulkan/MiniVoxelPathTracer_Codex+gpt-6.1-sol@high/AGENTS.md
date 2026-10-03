# MiniVoxelPathTracer 测评执行规则

本目录是 Codex + gpt-6.1-sol@high 的独立测评工作目录。完整需求以 MiniVoxelPathTracer_TASK.md 为准。

- 禁止联网搜索、访问网页或使用外部资料连接器；禁止查看、读取或修改本目录之外的内容，包括其他答卷、上级文件、外部技能文件与其他会话。不得派生会话或启动子代理。
- 所有显式文件操作及 shell 工作目录限定为本目录与子目录。允许调用已安装编译器、CMake 和构建所需工具；工具内部读取系统库和已安装依赖属于构建行为，不允许自行浏览这些目录。依赖准备由主会话负责，测评期间不联网获取依赖。
- 使用简体中文回复。新增普通注释和 XML 注释使用简体中文，仅解释有价值的原因、约束与取舍。
- 优先在 main.cpp 实现，辅助代码使用头文件，GLSL 放在 shaders/。必须将全部必选机制接入真实调用路径，不以 mock、硬编码画面、空调用、软件求交或静默降级替代。
- 禁止修改 CMakeLists.txt、CMakePresets.json、platforms.json、vcpkg.json、vcpkg-configuration.json 及已有 VS 项目配置。仅允许为新增文件向 vcxproj / filters 添加引用行：cpp 使用 ClCompile，头文件使用 ClInclude，shader/inc 使用 None 与源文件 filter，资源使用 None 与资源文件 filter。
- 不修改题面或 materials.json，不修改 gitignore / Git exclude。Git 提交、分支、推送和 PR 全部由主会话负责。
- main.cpp 当前仅是构建框架的明确失败入口，必须替换为完整实现，不能将框架编译成功视作任务完成。
- 运行与变更相称的构建与验证；编译器、shader 编译器和 Vulkan 的 warning/error/诊断均视为失败，Vulkan INFO 只记录。不得屏蔽或弱化有效诊断与测试。
- Windows 构建：cmake --preset windows-msvc-debug，然后 cmake --build out/windows-msvc-debug --config Debug；Release 使用 windows-msvc-release。Linux 使用 linux-gcc-debug/release，未验证的平台如实报告。
- 遇到缺口，报告具体缺失合同，不改变成功标准。完成前有限复查最终代码与实际调用路径，清除本次任务无用代码；在 IMPLEMENTATION.md 记录 pass、容量、归一化、历史规则、同步所有权及实际核验结果。
