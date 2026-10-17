# VoxelPBRFrustumCulling（历史）

本页记录旧答卷，已移出当前解决方案。题面链接固定到保留答卷的历史提交。

## 题目与规格

以 OpenGL 4.6 Core Profile 渲染像素体素场景，要求配置驱动 PBR、程序化 IBL、阴影、实例化、CPU 视锥剔除及调试视图。完整规格以题面为准。

完整任务定义：[`VoxelPBRFrustumCulling_TASK.md`](https://github.com/yhcedpn/RenderArena/blob/e6249c90946b758e1aab3885e6c50e70ec6f7ef1/OpenGL/VoxelPBRFrustumCulling_Codex+gpt-5.6-terra@max/VoxelPBRFrustumCulling_TASK.md)

## 相关评审 issue

- [gpt-5.6-luna 执行 VoxelPBRFrustumCulling 任务（#21）](https://github.com/yhcedpn/RenderArena/issues/21)
- [gpt-5.6-terra 执行 VoxelPBRFrustumCulling 任务（#17）](https://github.com/yhcedpn/RenderArena/issues/17)

## 构建核验（2026-10-17）

两份答卷均通过 Windows CMake Debug/Release 与 VS 解决方案构建，最终日志无 warning/error。可执行文件、匹配配置的 GLFW DLL 和 `materials.json` 位于各任务目录 `bin/<预设>/`。
