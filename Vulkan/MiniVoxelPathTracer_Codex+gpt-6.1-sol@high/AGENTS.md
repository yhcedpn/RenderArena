# MiniVoxelPathTracer 测评执行规则

本目录是 `MiniVoxelPathTracer_Codex+gpt-6.1-sol@high` 的独立答卷。功能要求以 `MiniVoxelPathTracer_TASK.md` 为准，以下规则用于执行测评。

- 显式文件操作与 shell 工作目录只能位于本目录及子目录。可读取随题提供的规范、题面、构建配置、资源、当前答卷源码和本轮生成物。
- 不得读取或检索上级目录、其他答卷、`.git`、Git 历史、其他会话、既有实现说明、评审、截图或历史运行日志。不得通过构建产物获取其他实现。
- 禁止联网搜索、网页、外部资料连接器、子代理和派生会话。算法材料仅限组织者随题提供的内容。
- 允许调用已安装的编译器、CMake、shader 编译器和程序；工具内部读取系统库、共享构建配置及已安装依赖属于构建行为。不得主动浏览这些任务目录外的文件，不得安装或修改依赖。
- 不修改题面、`materials.json`、既有构建配置、依赖、平台声明、`.gitignore` 或 Git exclude。只可为新文件向 `.vcxproj/.filters` 添加引用行。Git 操作由组织者负责。
- C++20 使用 `.cpp/.hpp`，C17 使用 `.c/.h`，不混用语言后缀。VS 中源文件为 `ClCompile`、头文件为 `ClInclude`、shader/inc 和资源为 `None`；filter 分别为“源文件”“头文件”“资源文件”，shader/inc 归入“源文件”。
- 使用简体中文。实现优先放在 `main.cpp` 或既有源码入口，辅助代码优先使用头文件；必须将题面要求的机制接入真实渲染调用路径。
- 在本目录运行 `cmake --preset windows-msvc-debug` 和 `cmake --build --preset windows-msvc-debug`；Release 对应 `windows-msvc-release`。Linux 对应 `linux-gcc-debug/release`，平台以 `platforms.json` 为准。
- 运行产物位于 `bin/<预设>/`，中间文件与依赖安装树位于 `obj/<预设>/`。编译器、shader 编译器和运行校验的 warning/error/诊断视为失败，Vulkan INFO 仅记录；不得屏蔽或弱化诊断。
- 在 `IMPLEMENTATION.md` 记录实际实现机制、构建与运行平台、验证结果和具体缺口；本轮未执行的验证不得写为通过。
