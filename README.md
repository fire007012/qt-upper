# 机器人 Qt6 上位机

Qt 通过 TCP/JSON 连接下位机 ROS2 `host_bridge`（默认9090）；独立管理服务（默认9091）提供预检、启动/停止、任务恢复和日志。视频使用 RTSP + FFmpeg。Qt 本身不依赖 ROS。

- [当前文档入口](docs/README.md) / [实车与模拟启动](docs/上下位机启动与控制说明.md)
- [控制与诊断](docs/Qt控制v2与诊断使用.md) / [运行管理与构建](docs/Qt运行管理使用与联调.md)
- [配置维护](docs/Qt配置维护使用与联调.md) / [发布与验收](docs/上位机发布与验收.md)
- [ROS2接口清单](docs/ROS2上位机接口对照与联调清单.md) / [兼容声明](deploy/compatibility.json)
- [视频联调](docs/ROS2视频联调说明.md) / [手柄键盘](docs/手柄键盘控制说明.md)
- [当前计划与职责](docs/上位机ROS2适配与可靠性重构计划.md) / [独立通信测试](tests/communication/README.md)

日常联动使用独立部署，不依赖源码工作区的构建目录：

```bash
# 终端 1：下位机管理服务
bash ~/robot24-portable/run_manager.sh
# 终端 2：上位机
bash ~/hostcomputer/scripts/run_linked.sh
```

上位机默认读取 `~/robot24-portable/runtime/manager.json`。源码工作区 `~/ros2_ws_car` 只保留 build、install、log、src；旧 linked-* 目录、兼容链接及旧部署备份已删除。详细操作见 [启动与控制](docs/上下位机启动与控制说明.md)。

本机 Qt6.12 环境的一次性构建与回归入口：

```sh
CMAKE_BIN=/home/fire/Qt/Tools/CMake/bin/cmake \
CMAKE_PREFIX_PATH=/home/fire/Qt/6.12.0/gcc_64 \
HOST_BUILD_WIDGET_TESTS=ON bash scripts/run_all_tests.sh
```

默认构建、测试和日志产物写入 `/tmp`。不启动 ROS、实机或历史桥接；界面进程冒烟默认关闭，可用 RUN_APP_SMOKE=1 显式启用。mock和构建通过不表示实机或现场一键启动验收完成。

`ros2_ws_car/` 是下位机只读参考，禁止改动及写入产物，详见 [AGENTS.md](AGENTS.md)。旧 ROS1 桥接、部署脚本和视频说明已移至 [历史归档](archive/ros1/README.md)，不参加当前构建、测试或部署。
