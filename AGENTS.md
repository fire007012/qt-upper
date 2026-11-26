# 上下位机协同工作说明

- 当前会话中上位机与下位机（`ros2_ws_car/`）已同工作区协同维护，可直接联动修改与回归验证。
- 上位机测试、缓存、日志和构建产物建议放在 `/tmp` 等构建目录，避免污染源码树。
- 实施顺序和上下位机职责见 `docs/上位机ROS2适配与可靠性重构计划.md`。没有完成下位机接口及联调时，不将 mock 成功标记为实机验收或一键启动已交付。
- 通信回归可独立构建，不需要 ROS 或完整 GUI：`cmake -S tests/communication -B /tmp/hostcomputer-communication-tests`，随后构建并执行 CTest。完整应用仍使用项目要求的 Qt6 环境。
- **仅在 `fire` 账户下**（当前虚拟机仅 3.8 GiB 内存 + 3.8 GiB Swap，4 核），编译时必须严格限制并发，避免 OOM 触发 `dbus.service` 崩溃导致整个桌面会话重启：
  - CMake 构建必须显式限制并发为 `-j 2`（例如 `cmake --build <build_dir> -j 2`），**禁止**使用不带数字的 `-j`（无参 `-j` 会拉起数十个 `cc1plus` 直接打爆内存）。
  - ROS 2 `colcon build` 必须串行编包并限制 `make` 并发：`MAKEFLAGS="-j2" colcon build --executor sequential ...`。
  - 禁止同时并行执行上位机编译与下位机 `colcon build`。
