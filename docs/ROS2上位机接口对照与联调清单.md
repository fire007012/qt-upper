# ROS2 上位机接口对照与联调清单

日期：2026-11-08。依据：只读检查 `ros2_ws_car/src/host_bridge/` 的节点、适配器和协议核心，以及上位机现有客户端。下表是参考源码中的映射，不是现场已运行接口或实机验收结果；默认路径仍可能被档案、参数和 remap 覆盖。

Qt 继续发送 TCP/JSON，ROS2 类型和内部路径由下位机管理。下位机改动必须在其独立工程实施，不修改本仓库的参考副本。

## 业务映射

| 上位机入口 | ROS2 参考映射 | 联调重点 |
| --- | --- | --- |
| `operator_input` 底盘输入 | `geometry_msgs/Twist` → `/cmd_vel` | 线速度 m/s、角速度 rad/s；实际速度限值和方向由下位机确认 |
| `operator_input` 机械臂输入 | `geometry_msgs/TwistStamped` → `/servo_server/delta_twist_cmds` | 帧、方向、Servo 就绪与规划互斥；启动参数默认帧为 `grasp_tcp`，不能沿用旧文档的 `catch_camera` 假设 |
| `operator_input` 摆臂输入 | `control_msgs/JointJog` → `/flipper_control/jog_cmd` | 关节名称、方向修正、速度、持续时间及丢输入后的停止行为 |
| 摆臂 profile 切换 | `robot_hardware/SetProfile` → `/flipper_control/set_control_profile` | 参考适配器将 `csv_velocity` 转为 `csv`，`csp_position/csp_jog` 转为 `csp`；确认实际允许运行的 profile |
| `operator_input` 夹爪输入 | `std_msgs/Float64` → `/arm_control/gripper_velocity` | 当前是速度接口；不能按旧桥接的位置目标解释 |
| `system_command` 的 init/enable/disable/halt/resume/recover/shutdown | `std_srvs/Trigger` → `/robot_runtime/{command}` | 接收、完成、Armed/Running 的区别；不能以 TCP 连接或 ACK 替代运行许可 |
| `set_control_mode` | `/arm_control/set_servo_mode` | 核对实际请求类型和模式值，服务成功后再同步实际模式 |
| `request_arm_named_targets` | 读取 `car_moveit_config/config/car_urdf.srdf` 中 arm 分组的命名目标 | 列表来自配置，不等于动作当前可执行 |
| `move_arm_named_target` | `arm_control/ExecuteArmGoal` action → `/arm_control/execute_goal` | 目标拒绝、Servo 互斥、长任务结果及超时结果未知的语义 |
| `joint_runtime_states` | 订阅 `/robot_runtime/state` 的 `std_msgs/String` JSON，转换为 TCP 关节状态列表 | `joint_name/backend/lifecycle_state/online/enabled/fault`；当前 `ready=false` 被映射为 fault，需确认 UI 是否应区分未就绪与真实故障 |
| `co2_data` | `std_msgs/Int32`，默认 `/co2_ppm` | ppm 单位、有效性和过期状态 |
| `imu_data` / `motor_state` | Qt 已能解析；本批未确认活动桥接中的真实数据订阅和发布路径 | 由下位机确认来源、频率、单位和缺测行为，不能以 mock 数据或默认值验收 |
| 摄像头列表/流控制 | TCP 相机协议 → 视频管理 IPC；RTSP 独立拉流 | 相机 ID、实际播放地址、管理服务不可达与视频流不可达分开显示 |
| 急停 | TCP `emergency_stop`、桥接归零与下位机运行约束 | 由下位机明确锁存及解除语义、桥接崩溃时的独立停止保障；不能仅验证 ACK |

## 当前控制v2与管理v1

下位机文档40/41、构建系列 `robot24-p0-config-2` 已提供控制v2与配置事务。Qt构建 `host-ros2-v2-config-2` 已接入，不再将旧v1运动路径作为生产兼容方式。

- 管理v1完成hello/profiles/preflight/start/stop/operation/snapshot/logs及文档41配置方法；64KiB NDJSON，独立连接。软件ready、档案匹配及模块健康后，向Qt交付v2端点及机器人身份。
- 控制v2核对session、epoch、机器人身份、六项必需能力，100ms刷新freshness，明确操作/运动两级授权、实际runtime许可；状态不能自动授予本地权限。业务写入走异步operation，未知结果只能显式原ID恢复。
- 旧v1仅保留heartbeat、sync_request、camera_list_request与遥测/相机观测。Qt隔离旧ACK/状态对v2授权的影响。未纳入v2的原始关节/电机/末端写入拒绝发送；模式与服务边沿从连续输入中过滤。
- 急停使用独立短连接，不依赖普通会话或令牌；桥接锁存不等于物理停止。重连和解除急停均不恢复授权。
- 配置维护仅覆盖首批启动超时/视频必需策略；不写设备参数。管理源激活、实际运行版本、实时硬件许可分别展示。

使用见[运行管理](Qt运行管理使用与联调.md)、[控制与诊断](Qt控制v2与诊断使用.md)、[配置维护](Qt配置维护使用与联调.md)。独立回归与只读真实协议实现的注入后端契约已提供；真实分组许可、ROS2接口、物理归零及跨机器时间仍须共同验收。

## 联调出口

- 分别登记 Qt、桥接与 ROS2 的构建版本、有效配置和接口清单。
- 先用 mock/仿真核对上述映射，再执行实机单轴、控制归零、运行许可和故障恢复验收。
- 注入掉网、ACK 丢失、旧序号、时钟偏差和输入积压；分别保存接收、接受、拒收、输出及实际运行状态证据。
- 一键启动另按重构计划第 5.5 节验收，不以管理请求 accepted 或端口可连作为整体成功。

后续以 [当前重构计划](上位机ROS2适配与可靠性重构计划.md) 为准，执行真实部署、运行许可与物理输出联调；[增量协作提示词](下位机P0与配置事务增量协作提示词.md)保留为本次下位机交付的历史需求。
