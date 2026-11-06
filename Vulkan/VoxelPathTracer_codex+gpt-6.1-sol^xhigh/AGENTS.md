# VoxelPathTracer 测评执行规范

## 开始与修改边界

- 功能实现以同目录 [VoxelPathTracer_TASK.md](VoxelPathTracer_TASK.md) 为准；材质输入为 [materials.json](materials.json)，依赖及版本锁定在 [vcpkg.json](vcpkg.json)。启动上下文应包含组织者提供的根目录通用规范。
- 新增 `src/` 下的 `.cpp/.c` 与 `shaders/` 下的 stage shader 会在下次配置时自动纳入构建；新增文件按根目录“工程约定”同步追加 VS 项目与 filter 引用。
- VS“资源文件”仅展示 `VoxelPathTracer_TASK.md` 和 `materials.json`；规范、配置与实现说明不添加到该分组。
- 可修改 `main.cpp`，可在 `src/` 新增和修改 `.cpp/.c/.hpp/.h`，在 `shaders/` 新增和修改独立 GLSL 源文件，并编写 `IMPLEMENTATION.md`。辅助头文件也可放在任务目录。
- `AGENTS.md`、`VoxelPathTracer_TASK.md`、`materials.json`、`platforms.json`、`vcpkg.json`、`CMakeLists.txt`、`CMakePresets.json` 及既有 VS 设置为只读；`.vcxproj/.filters` 仅可追加新增文件的项目引用行，保留原有内容。读取范围遵循根目录“测评隔离”，其中依赖头文件和使用说明仅从本任务目录的安装树查阅。
- `obj/dependencies/` 为只读安装树；`bin/<预设>/` 与 `obj/<预设>/` 仅供构建工具和程序生成产物。本轮诊断日志与运行检查输出可写入 `obj/`，不修改生成脚本或二进制来绕过源码与构建要求。

## 构建与运行入口

- 在本目录执行根目录“构建与交付”规定的预设配置与构建；配置只查找已安装依赖。缺失依赖由组织者处理。
- 程序为 `bin/<预设>/VoxelPathTracer.exe`（Windows）或 `bin/<预设>/VoxelPathTracer`（Linux）；`materials.json` 部署到程序同目录。独立 stage shader 从 `shaders/` 编译到 `bin/<预设>/shaders/<相对源文件名>.spv`，保留 stage 后缀，如 `trace.rgen.spv`。包含文件使用 `.glsl`。
- shader 版本与目标格式见题面“版本与 API 形式”，离线编译由 CMake 调用 glslc。实际核验使用的工具版本写入 `IMPLEMENTATION.md`。

## 交付

在 `IMPLEMENTATION.md` 记录题面“交付与验收 / 交付内容”要求的机制和实际证据；逐项说明未实现或未核验的要求。
