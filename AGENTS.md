# RenderArena

## 适用范围

本仓库保存图形编程测评题面、资源和各模型答卷。以下测评规则适用于执行题目的智能体；仓库维护者负责准备依赖、调整工程、整理题面和 Git 操作。用户明确授权的维护任务可以修改相应配置。

## 测评隔离

- 组织者为每次测评指定一个任务目录，并在启动时提供适用规范。测评智能体的显式文件操作和 shell 工作目录只能位于该目录及其子目录。
- 可读取随题提供的 `AGENTS.md`、`*_TASK.md`、构建配置、资源、当前答卷源码，以及本轮生成的文件。不得主动读取或检索其他答卷、上级目录、`.git`、Git 历史、其他会话、预先存在的实现说明、评审、截图或运行日志；不得借助构建产物反推其他实现。
- 禁止联网搜索、网页、外部资料连接器、子代理和派生会话。论文或算法材料只有在组织者随题提供时才可使用。
- 允许调用已安装的编译器、CMake、shader 编译器和运行时。工具内部加载系统库、共享构建配置及已安装依赖属于构建行为；不得将其作为主动浏览任务目录外文件的入口。
- 依赖由组织者在测评前准备。测评智能体不得安装、升级或修改依赖、系统环境和工具链。缺少依赖时报告具体缺项。

## 工程与文件约定

- 每份答卷包含 `*_TASK.md`、`materials.json`、`main.cpp` 或题面约定的源码目录，以及 `.vcxproj`、`.vcxproj.filters`、`CMakeLists.txt`、`CMakePresets.json`、`platforms.json`、`vcpkg.json` 和适用的 `vcpkg-configuration.json`。
- C++ 使用 C++20，源文件为 `.cpp`，头文件为 `.hpp`；确需 C 时使用 C17、`.c` 和 `.h`，不得以另一种语言的后缀存放或强制编译。GLSL 使用对应 stage 后缀或 `.glsl`；`.inc` 仅作为包含资源，不作为 C/C++ 编译单元。
- VS 中 `.cpp/.c` 使用 `ClCompile`，filter 为“源文件”；`.hpp/.h` 使用 `ClInclude`，filter 为“头文件”；shader 和 `.inc` 使用 `None`、filter 为“源文件”；题面、说明、JSON 与构建配置使用 `None`、filter 为“资源文件”。
- 测评期间不得修改既有项目设置、依赖、平台声明、题面或 `materials.json`。只允许为新文件添加 `.vcxproj/.filters` 引用行；新增编译单元应遵循现有 CMake 源码列表，优先将辅助实现放入头文件。
- 不修改 `.gitignore` 或 Git exclude，不强制添加被忽略文件。Git 提交、推送与 PR 由组织者处理。
- 使用简体中文沟通。注释解释算法、约束或取舍，不记录临时工作状态。

## 构建与交付

- VS 项目委托 CMake，所有项目共用 `cmake/RenderArena.cmake` 的 C++20/C17、输出目录和诊断设置。Windows 仅提供 `Debug|x64`、`Release|x64`；平台以 `platforms.json` 为准。
- 在任务目录配置并构建 Debug 和 Release：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

- Linux 对应 `linux-gcc-debug` 和 `linux-gcc-release`，同样使用 `cmake --build --preset <预设>`。
- 可执行文件、DLL 和运行资源位于任务目录 `bin/<预设>/`；CMake 缓存、中间文件和独立依赖安装树位于 `obj/<预设>/`。vcpkg 依赖临时构建目录由组织者设置在仓库根 `obj/vcpkg/`。
- 编译器、shader 编译器和运行校验的 warning/error/诊断均按失败处理；正常构建进度和 Vulkan INFO 只记录。不得屏蔽诊断、修改验收条件或加入空调用来制造通过结果。
- 实际运行与改动相称的检查，记录配置、构建、运行平台、诊断和未完成要求。不得把编译成功等同于算法或交互验收通过。

## 题面格式

- 文件名为 `<题目名>_TASK.md`，一级标题为“<题目名> 测评任务”，随后给出图形 API、语言和输入资源表。
- 先写“测评边界与构建”，再写目标、工程约束、功能规格、禁止事项与验收标准。各题可以按算法拆分功能章节，标题不混用中文序号和阿拉伯数字序号。
- 同一道题的不同答卷保留相同题面。资源路径使用相对任务目录或可执行文件目录的明确表述；构建步骤引用预设，不写开发者机器的绝对路径。
- 更改题面格式不改变功能、数值、算法或验收阈值；规格修订须说明具体变更，避免混入纯格式整理。
