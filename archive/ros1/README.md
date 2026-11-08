# ROS1 历史归档（已退役）

2026-11-08：下位机已迁移至 ROS2，本目录仅保留历史实现和配置用于追溯，不参加当前 Qt 构建、默认测试或发布部署。

| 内容 | 原位置 | 当前处理 |
| --- | --- | --- |
| 旧桥接、视频脚本与本地配置 | 根目录 ros1_bridge/ | 原文件整体迁移，包含 video_sources.local.yaml；旧参数不得直接当作 ROS2 有效配置 |
| 旧桥接/视频回环测试 | scripts/bridge_loopback_test.py、video_manager_smoke_test.py | 仅作历史测试保存，默认回归已换成独立 Qt 通信测试 |
| 同步部署脚本 | scripts/sync_ros1_bridge.py | 退出活动入口；即使在归档中直接执行也会立即报退役，不搜索、不复制 |
| ROS1 视频服务及环境模板 | deploy/systemd/robot-video-stack.* | 不再作为安装模板，原 ROS_PACKAGE_PATH/roslaunch 不适用于 ROS2 |
| ROS1 视频及节点对接说明 | 原 docs/ 视频文档及根目录节点文档 | 标注已退役，历史命令不可用于当前部署 |

本目录放置 CATKIN_IGNORE、COLCON_IGNORE，避免被误发现为活动工作区。保留旧包名不表示当前系统仍依赖 ROS1，也不是官方 ROS1/ROS2 互通包的部署入口。

当前架构为 Qt → TCP/JSON → 下位机 ROS2 host_bridge；管理服务使用独立 TCP9091。下位机实际安装、桥接和视频配置由独立获授权的下位机工程维护；不得修改本仓库 ros2_ws_car/ 参考副本。

- [当前 ROS2 对接](../../下位机ROS节点对接文档.md)
- [Qt 运行管理](../../docs/Qt运行管理使用与联调.md)
- [ROS2 视频联调](../../docs/ROS2视频联调说明.md)
- [当前通信测试](../../tests/communication/README.md)
