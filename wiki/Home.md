# RenderArena Wiki

RenderArena 用固定的 C++ 图形编程任务比较模型在渲染算法、资源管理、工程实现与实际性能上的表现。本页说明评测方式、通用构建命令和项目结构。

## 测什么

- 每道题给出固定的功能与资源规格、依赖配置和构建入口；答卷从零实现完整程序，而不是补全模板。
- 验收关注规定机制是否接入实际调用路径。构建通过、截图相似或出现算法名称都不能替代机制本身，具体判据见题面的“交付与验收”。
- 题面规范保存在 `Tasks/`；源码、工程配置和实现说明按答卷组织。

## 怎么比

- **同一题面**：同一道题的不同答卷使用相同题面和工程配置，便于横向比较。
- **隔离执行**：测评智能体只读取题面、任务目录和预装依赖，不使用子代理、不读取其他答卷或历史结果。
- **固定构建**：组织者按任务目录预设构建实际运行平台的 Debug 和 Release 产物；编译器、shader 编译器和运行时校验的诊断按失败处理。
- **综合验收**：结论来自构建、源码审查和真实交互运行；未核验的平台和交互项必须明确列出。
- **证据留存**：答卷的 `IMPLEMENTATION.md` 记录实现机制、适用版本与环境、实际核验结果和未完成项。

## 构建

以下命令适用于所有任务；`<任务目录>` 指具体答卷目录，例如 `Vulkan/VoxelPathTracer_omp+deepseek-v4.1-flash-1017^max`。任务特有的运行要求和资源映射见题面与任务目录 `AGENTS.md`。

### 组织者准备依赖

1. Windows 预设基于 MSVC v145：需要安装 Visual Studio 2026 的“使用 C++ 的桌面开发”工作负载、MSVC v145 和 Windows SDK；命令行 CMake ≥ 3.28，且需支持 `Visual Studio 18 2026` 生成器（已核验 4.3.1）。
2. Linux 预设使用 GCC：安装 CMake ≥ 3.28、Ninja、GCC，以及 vcpkg 需要的 `git`、`curl`、`zip`、`unzip`、`tar` 和 `pkg-config`；使用 X11/Wayland 窗口时还需对应开发包。
3. 设置 `VCPKG_ROOT` 指向已安装的 vcpkg。按题面准备图形 API 工具链，例如 Vulkan 的 `glslc` 可通过 `PATH` 或 `VULKAN_SDK` 定位。
4. 从仓库根目录执行依赖准备脚本：

```powershell
cmake "-DARENA_TASK_DIR=<任务目录>" -P cmake/PrepareDependencies.cmake
```

依赖与版本由答卷的 `vcpkg.json` 声明。安装目录由[依赖准备脚本](https://github.com/yhcedpn/RenderArena/blob/main/cmake/PrepareDependencies.cmake)与构建预设约定；二进制缓存使用 vcpkg 用户级共享缓存或显式设置的 `VCPKG_BINARY_SOURCES`。

测评的依赖准备与操作范围见[通用执行规范](https://github.com/yhcedpn/RenderArena/blob/main/AGENTS.md#测评隔离)。

### 配置与构建

进入任务目录：

```powershell
cd "<任务目录>"
```

Windows 分别配置并构建 Debug、Release：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
```

Linux 使用对应预设：

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
cmake --preset linux-gcc-release
cmake --build --preset linux-gcc-release
```

平台适用性以任务目录 `platforms.json` 为准；Linux 预设仅在 Linux 主机上可用，预设按 `hostSystemName` 过滤。预设定义见[根目录 CMakePresets.json](https://github.com/yhcedpn/RenderArena/blob/main/CMakePresets.json)，任务目录引用共享预设；语言标准和编译诊断设置见 [RenderArena.cmake](https://github.com/yhcedpn/RenderArena/blob/main/cmake/RenderArena.cmake)。

Windows 也可打开 `RenderArena.slnx`，选择 `Debug|x64` 或 `Release|x64`。VS 项目委托相同 CMake 预设；未设置 `VCPKG_ROOT` 时，VS 入口使用随 VS 安装的 vcpkg。依赖仍需先按上述步骤准备。

### 运行与产物

可执行文件、DLL 和运行资源位于任务目录 `bin/<预设>/`；中间文件位于 `obj/<预设>/`，依赖安装树与应用配置目录分开。程序名、资源部署、校验层和设备能力要求见题面与任务目录 `AGENTS.md`。

常见失败：

| 现象 | 处理 |
| --- | --- |
| 配置时报找不到 vcpkg 工具链 | 设置 `VCPKG_ROOT`，或在 Visual Studio 中安装 vcpkg 组件 |
| 构建时报找不到图形 API 工具链 | 按题面安装 SDK 并加入 `PATH`，例如 Vulkan 的 `glslc` |

## 测评项目结构

```text
RenderArena/
├─ Tasks/                 # 题面规范：<题目>_TASK.md
├─ Vulkan/                # Vulkan 答卷目录
├─ cmake/                 # 共享构建配置、依赖准备、shader 编译和诊断设置
├─ wiki/                  # Wiki 源文件，推送 main 后同步
├─ CMakePresets.json      # Debug/Release 预设
├─ RenderArena.slnx       # Visual Studio 解决方案入口
└─ AGENTS.md              # 通用执行规范
```

每份答卷是独立工程：

| 路径 | 内容 |
| --- | --- |
| `main.cpp`、`src/` | C++ 源码 |
| `shaders/` | shader 源文件 |
| `*_TASK.md` | 题面副本；规范文本在 `Tasks/` |
| `AGENTS.md` | 任务执行规范、修改边界与运行入口 |
| `IMPLEMENTATION.md` | 实现机制、环境和核验记录 |
| `vcpkg.json`、`platforms.json` | 依赖锁定与目标平台 |
| `CMakeLists.txt`、`CMakePresets.json` | 构建入口，引用共享预设 |
| `bin/`、`obj/` | 构建产物、中间文件和依赖安装树 |

答卷目录以 `<题目>_<工具>+<模型>` 开头，思考档位等后缀按需追加；当前只有 `Vulkan/`，OpenGL 答卷目录在收录后建立。

## 文档维护

- 任务规范位于 `Tasks/`，任务页的题面链接指向该目录；答卷目录保留同文副本。修订与同步规则见[通用执行规范](https://github.com/yhcedpn/RenderArena/blob/main/AGENTS.md#文档职责)。
- Wiki 源文件位于仓库 `wiki/`，修改后由 [Sync Wiki 工作流](https://github.com/yhcedpn/RenderArena/blob/main/.github/workflows/sync-wiki.yml)同步到 GitHub Wiki。触发条件见工作流配置。
