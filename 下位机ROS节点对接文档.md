# 下位机 ROS2 对接说明

当前下位机使用 ROS2。Qt 上位机通过 TCP/JSON 连接下位机已有的 `host_bridge`，不依赖 ROS1、DDS 或 ROS 开发库。原 ROS1 节点教程已移至 [历史归档](archive/ros1/docs/下位机ROS节点对接文档.md)，其中的话题、服务名和部署命令不适用于当前系统。

```text
Qt RobotTcpClient ── TCP/JSON 9090 ── ROS2 host_bridge ── ROS2 控制与安全模块
Qt ManagementClient ── 管理 v1 9091 ── 独立管理服务 ── 启停 / 预检 / 状态 / 日志
Qt FFmpeg 播放器 ── RTSP ── 下位机视频服务
```

- 管理协议的消息、认证、任务恢复、部署边界和能力要求，以 [文档39](ros2_ws_car/src/docs/39_下位机管理协议与Qt对接.md) 为准。
- 控制 TCP 消息见 [TCP/JSON 协议](TCP_JSON协议文档.md)；ROS2 话题、类型、单位和失败语义核对见 [ROS2接口清单](docs/ROS2上位机接口对照与联调清单.md)。清单是源码参考映射，现场路径可能由配置或 remap 改变。
- Qt 日常操作与 Qt6 构建见 [运行管理使用与联调](docs/Qt运行管理使用与联调.md)，视频见 [ROS2视频联调说明](docs/ROS2视频联调说明.md)。

上下位机分别维护各自代码。本仓库 `ros2_ws_car/` 始终为只读参考，不得修改或写入构建/测试产物；下位机服务、ROS2 和系统安装在独立获授权工作区实施。不要从上位机归档复制旧桥接到 ROS2 工作区。

管理 ready 只表示软件可服务，不能代替初始化、使能、急停状态或运动许可。Qt已接入文档40控制v2的session/freshness、两级授权、异步任务和独立急停，见[控制与诊断](docs/Qt控制v2与诊断使用.md)。真实运行许可、物理输出、网络及24h仍须共同验收，不能以无设备契约成功代替实机验收。
