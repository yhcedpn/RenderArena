# RenderArena Wiki

这里是 `RenderArena` 的文档主页。

## 如何构建

构建配置仅支持 `Debug|x64` 与 `Release|x64`。

### 方法 1：Windows + Visual Studio

在 Windows 11 系统运行，安装 Windows SDK；GPU 和驱动需满足所选题目的 OpenGL 或 Vulkan 版本与特性要求。

1. 安装 Visual Studio 2026，确保安装 `使用 C++ 的桌面开发` 模块。
2. 确保本机已正确集成 `vcpkg` 包管理器，并且 MSVC 版本为 v145。
3. 克隆本仓库，打开 `RenderArena.slnx`，构建并运行你想测试的项目，只可选择 `Debug|x64` 或 `Release|x64` 构建配置。

`RubikCube_Copilot+glm5.2@max` 的 VS 项目直接调用任务目录的 CMake 预设，复用与 CI 相同的编译、链接及资源复制规则。它使用 `VCPKG_ROOT` 指定的 vcpkg；未设置时使用 VS 自带的 vcpkg。输出位于 `out/windows-msvc-<配置>/<Debug 或 Release>/`，调试工作目录为该输出目录。原始答卷的构建状态与接入边界见 [RubikCube](Vulkan-BenchmarkTasks/RubikCube)。

### 方法 2：CLI + CMake（Windows / Linux）

同一份任务代码可在 Windows 与 Linux 上使用 CMake + vcpkg 构建。

1. 安装 CMake（≥ 3.28）与 Ninja 构建器；Windows 下 CMake 随 VS2026 自带，Linux 下用 `apt install cmake ninja-build`（并安装 GCC）。
2. 设置环境变量 `VCPKG_ROOT` 指向本机 vcpkg 目录。
3. 进入任务目录（如 `OpenGL/HelloWindow_Codex+gpt-5.4@xhigh`）：
   - Windows：`cmake --preset windows-msvc-release`，然后 `cmake --build out/windows-msvc-release --config Release`
   - Linux：`cmake --preset linux-gcc-release`，然后 `cmake --build out/linux-gcc-release`
   - Debug 对应 `windows-msvc-debug` / `linux-gcc-debug`；预设定义集中在仓库根 `CMakePresets.json`，各任务目录的 `CMakePresets.json` 引用同一份。
4. 依赖由各任务目录 `vcpkg.json`（manifest 模式）声明，vcpkg 按 `x64-windows` / `x64-linux` triplet 自动安装；构建产物在任务目录 `out/` 下，不提交。
5. 运行产物：Windows 下为 `out\windows-msvc-release\<Config>\<可执行名>.exe`；Linux 下为 `./out/linux-gcc-release/<可执行名>`。

> 说明：
> - 渲染程序需要图形环境运行。Linux 上请在带 X11 会话与 GPU 驱动（或 mesa 软件渲染）的机器上运行；CI 仅做双平台编译验证，不运行。
> - 平台适用性由各任务目录 `platforms.json` 声明（`{"build_platforms": ["windows","linux"]}`，缺失视为全平台支持）；平台专有实现（如两份 VoxelPBR，使用 Windows 专有 API 且 `platforms.json` 仅声明 `windows`）不会在 Linux 构建。

## 任务与发布

- [OpenGL 图形编程任务](OpenGL-BenchmarkTasks)
- [Vulkan 图形编程任务](Vulkan-BenchmarkTasks)

新增答卷放在 `OpenGL/` 或 `Vulkan/` 的直接子目录；构建矩阵按 `CMakeLists.txt` 和 `platforms.json` 自动发现，并使用任务目录的共享 CMake 预设。平台声明表示应参与构建的目标，不代表答卷已经通过编译或运行验收。

合并测评不会自动发布 Release。发布由推送 `v主版本.次版本` 标签（如 `v1.1`）触发，也可对已存在的标签手动补发。打包脚本包含可执行文件、Windows DLL、`shaders/` 和 `materials.json`。
