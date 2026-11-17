> 历史归档（2026-11-17 整理）。本文保留当时的设计、接口交接或验证记录；版本、路径、命令和临时授权仅对应原记录，不作为当前操作依据。当前说明见 [文档入口](../README.md)。

# 机械臂专用 ROS1 节点开发文档

## 1. 目标

本文档用于指导你在下位机环境中开发一个**机械臂专用 ROS1 节点**。这个节点只做三件事：

1. 监听上位机 `hostcomputer` 发来的 TCP JSON 指令
2. 只接收机械臂相关控制命令
3. 将命令转换为 ROS Topic 发布给下位机内部控制链路

本文档**不包含代码实现**，只定义开发边界、协议映射、推荐话题和节点行为。

> 协议 v1 后，上位机高频键盘/手柄输入统一通过 `operator_input` 进入下位机 `host_bridge_node`。机械臂专用节点可以继续只接收 bridge 解析后的机械臂 topic/service；如果它直接监听 TCP JSON，则应处理 `operator_input` 的 `mode="arm"` 输入快照，而不是等待旧版 `cmd_vel` 或上位机直接生成的速度语义。

## 2. 范围与边界

本节点第一阶段只处理以下输入：

- `joint_control`
- `cartesian_control`
- `system_command` 中的 `executor_control`
- `emergency_stop`
- `heartbeat`

本节点第一阶段明确**不处理**以下输入：

- `cmd_vel`
- `motor_command`
- `control_command`
- 摄像头、CO2、IMU、系统状态上报

建议做法是：

- 对不支持的消息类型记录 `warn`
- 不要因为收到不支持消息就断开 TCP

## 3. 上位机当前真实行为

这个部分很关键，以下内容不是“理想协议”，而是当前上位机代码实际会发出的机械臂相关指令。

### 3.1 键盘机械臂模式

来源：`C:/Users/Lenovo/Documents/hostcomputer/mainwindow.cpp`（原 Windows 路径）、`C:/Users/Lenovo/Documents/hostcomputer/src/controller/KeyboardController.cpp`（原 Windows 路径）

键盘切到机械臂模式后会发送：

- `joint_control`
- `system_command`，其中 `command = "executor_control"`
- `emergency_stop`

其中最容易误解的一点是：

- 当前上位机发出的 `joint_control.position` 实际是**步进增量**，不是绝对目标位
- 例如按一次 `W`，发送的通常是 `+0.1`
- 按一次 `S`，发送的通常是 `-0.1`

所以你下位机桥接节点如果直接把它当“绝对目标位置”使用，语义会错。

### 3.2 手柄机械臂模式

来源：`C:/Users/Lenovo/Documents/hostcomputer/mainwindow.cpp`（原 Windows 路径）

手柄切到机械臂模式后会发送：

- `cartesian_control`
- `emergency_stop`

其中：

- `x/y/z` 是末端位置增量
- `roll/pitch/yaw` 是末端姿态增量
- 这些量都应按“增量命令”处理，不应按绝对位姿处理

### 3.3 全局消息

无论什么模式，上位机还会发送：

- `heartbeat`，约 1Hz
- `emergency_stop`

## 4. 推荐节点职责

建议将节点职责收敛为“桥接层”，不要在这个节点里混入运动学、轨迹规划或驱动细节。

建议职责如下：

1. 启动 TCP Server，监听 `0.0.0.0:9090`
2. 接收一行一个 JSON、以 `\n` 结尾的 UTF-8 消息
3. 解析消息并按 `type` 分发
4. 校验字段合法性
5. 发布到 ROS Topic
6. 维护连接状态和心跳超时
7. 输出日志，方便联调

不建议放在这个节点里的职责：

- 正逆运动学
- 轨迹插补
- 关节限位策略
- 驱动器控制细节
- 硬件急停逻辑

这些应交给后续机械臂控制节点或驱动节点。

## 5. 推荐 ROS Topic 设计

如果你下位机已有命名规范，可以整体放进 `/arm` namespace。为了兼容当前仓库里的对接文档，下面先给出一套直接可用的名字。

### 5.1 必需 Topic

| Topic | 类型 | 来源 TCP 指令 | 说明 |
|------|------|------|------|
| `/joint_command` | 自定义 | `joint_control` | 单关节增量控制 |
| `/cartesian_command` | 自定义 | `cartesian_control` | 末端位姿增量控制 |
| `/executor_command` | 自定义或 `std_msgs/Float32` | `system_command/executor_control` | 夹爪/执行器开合 |
| `/emergency_stop` | `std_msgs/Bool` | `emergency_stop` | 急停事件 |

### 5.2 建议增加的调试 Topic

| Topic | 类型 | 说明 |
|------|------|------|
| `/arm_bridge/connection_alive` | `std_msgs/Bool` | 当前 TCP 连接状态 |
| `/arm_bridge/last_command_type` | `std_msgs/String` | 最近一次处理的命令类型 |
| `/arm_bridge/diagnostic` | `diagnostic_msgs/DiagnosticArray` 或自定义 | 可选，便于联调 |

## 6. 推荐消息定义

为了避免语义混乱，建议不要直接把字段命名成 `position` 然后默认理解为绝对位置。对机械臂专用节点，推荐把“增量语义”写进消息定义。

### 6.1 `ArmJointCommand.msg`

```text
std_msgs/Header header
uint8 joint_id
float32 position_delta
float32 velocity
string source
```

说明：

- `joint_id` 范围建议固定为 `0..5`
- `position_delta` 对应上位机发来的 `joint_control.position`
- `source` 可填 `keyboard`，方便联调

### 6.2 `ArmCartesianCommand.msg`

```text
std_msgs/Header header
float32 dx
float32 dy
float32 dz
float32 droll
float32 dpitch
float32 dyaw
string source
```

说明：

- 全部按增量解释
- `source` 可填 `gamepad`

### 6.3 `ExecutorCommand.msg`

```text
std_msgs/Header header
float32 value
string action_hint
```

说明：

- `value > 0` 代表打开趋势
- `value < 0` 代表闭合趋势
- `value = 0` 代表保持
- `action_hint` 可填 `open` / `close` / `hold`

如果你不想定义这个消息，也可以临时用：

- `/executor_command` + `std_msgs/Float32`

但长期不如自定义消息清晰。

## 7. TCP 指令到 ROS Topic 的映射

### 7.1 `joint_control`

上位机示例：

```json
{
  "type": "joint_control",
  "joint_id": 2,
  "position": 0.1,
  "velocity": 0.1,
  "timestamp": 1709971200000
}
```

推荐映射：

- 发布到 `/joint_command`
- `joint_id = 2`
- `position_delta = 0.1`
- `velocity = 0.1`

建议校验：

- `joint_id` 必须在 `0..5`
- `position`、`velocity` 必须是数值
- 缺字段直接丢弃并记日志

注意：

- 这里的 `position` 请按**增量**理解
- 不建议在桥接节点中将其改写成绝对目标位置

### 7.2 `cartesian_control`

上位机示例：

```json
{
  "type": "cartesian_control",
  "x": 0.01,
  "y": 0.0,
  "z": -0.01,
  "roll": 0.0,
  "pitch": 0.05,
  "yaw": 0.0,
  "timestamp": 1709971200000
}
```

推荐映射：

- 发布到 `/cartesian_command`
- `dx = x`
- `dy = y`
- `dz = z`
- `droll = roll`
- `dpitch = pitch`
- `dyaw = yaw`

建议校验：

- 6 个自由度字段缺失时用 `0.0` 还是直接拒收，二选一即可，但要保持一致
- 如果你希望行为更保守，建议缺字段就拒收

### 7.3 `system_command` 中的 `executor_control`

上位机示例：

```json
{
  "type": "system_command",
  "command": "executor_control",
  "params": {
    "value": 1.0
  },
  "timestamp": 1709971200000
}
```

推荐映射：

- 当 `command == "executor_control"` 时，发布到 `/executor_command`
- `value > 0` 解释为打开
- `value < 0` 解释为闭合
- `value == 0` 解释为保持

建议对其他 `system_command` 的处理：

- 第一阶段直接忽略
- 记录日志：`unsupported system_command`

### 7.4 `emergency_stop`

上位机示例：

```json
{
  "type": "emergency_stop",
  "timestamp": 1709971200000
}
```

推荐映射：

- 立即向 `/emergency_stop` 发布 `std_msgs/Bool(data=true)`

建议行为：

1. 优先处理，不排队
2. 记录高优先级日志
3. 可选地同步发布一条零增量或停止命令给后级控制器

注意：

- 桥接节点不一定要负责“解除急停”
- 如果后级控制器需要复位，请通过单独 topic 或 service 设计

### 7.5 `heartbeat`

上位机示例：

```json
{
  "type": "heartbeat",
  "timestamp": 1709971200000
}
```

推荐处理：

- 不必发布机械臂控制 topic
- 只更新 `last_heartbeat_time`
- 可选地刷新 `/arm_bridge/connection_alive`

建议超时策略：

- 连续 `3s~5s` 没有收到 heartbeat 或任意有效指令，则视为上位机离线

## 8. 不支持指令的处理策略

对于以下指令：

- `cmd_vel`
- `motor_command`
- `control_command`

建议统一策略：

1. 记录一次 `warn`
2. 不发布任何机械臂 topic
3. 保持 TCP 连接不断开

这样上位机即使误操作到车体模式，也不会导致你的机械臂测试节点异常退出。

## 9. 推荐节点结构

建议拆成以下几个逻辑模块，即使最终只写成一个文件，也最好按这个思路组织。

### 9.1 `TcpSessionManager`

职责：

- 监听 9090 端口
- 接受客户端连接
- 维护收包缓冲区
- 按 `\n` 切分消息

建议：

- 同时只允许一个上位机连接
- 新连接到来时，可拒绝或踢掉旧连接，二选一即可

### 9.2 `JsonCommandParser`

职责：

- JSON 反序列化
- 字段存在性检查
- 类型检查
- 输出内部命令结构

建议：

- 不要在这个层里直接 publish ROS Topic

### 9.3 `ArmCommandDispatcher`

职责：

- 根据 `type` 分发
- 把 TCP 指令映射成 ROS 消息
- 调用对应 Publisher

### 9.4 `ConnectionWatchdog`

职责：

- 维护最近一次 heartbeat 时间
- 维护最近一次有效命令时间
- 超时后更新连接状态

## 10. 推荐运行时行为

### 10.1 启动

节点启动后：

1. 初始化 ROS Publisher
2. 启动 TCP Server
3. 输出监听地址和端口
4. 发布 `/arm_bridge/connection_alive = false`

### 10.2 建连

上位机连接后：

1. 输出连接日志
2. 清空上一次会话缓冲
3. 发布 `/arm_bridge/connection_alive = true`

### 10.3 收包

收到一条完整 JSON 后：

1. 先解析 JSON
2. 校验字段
3. 分发到对应 Topic
4. 记录最近命令时间

### 10.4 断连

连接断开后：

1. 发布 `/arm_bridge/connection_alive = false`
2. 输出日志
3. 不自动发布任何运动命令，除非你希望加保护停止逻辑

## 11. 日志建议

建议至少输出以下日志：

- 节点启动成功
- 开始监听 TCP
- 上位机连接/断开
- 收到的命令类型
- 字段校验失败
- 不支持的命令类型
- 急停触发
- 心跳超时

建议日志节流：

- 对连续重复的 `heartbeat` 不要每条都打印
- 对高频 `cartesian_control`、`joint_control` 可做 0.5s 节流摘要

## 12. 联调建议

### 12.1 与当前上位机联调

用当前 `hostcomputer` 测试时，建议按这个顺序验证：

1. 启动下位机机械臂专用 ROS1 节点
2. 在上位机里连接下位机 `IP:9090`
3. 切到机械臂模式
4. 键盘按 `1` 切到机械臂模式，按 `6/7/8/9/0` 选择速度档，再按 `W/S` 等方向键，看 bridge 输出的机械臂控制话题
5. 键盘按 `A/D`，看 `/executor_command`
6. 手柄切到机械臂模式，推动摇杆和扳机，看 `/cartesian_command`
7. 按空格或手柄 `L3+R3`，看 `/emergency_stop`

### 12.2 推荐观察命令

```bash
rostopic echo /joint_command
rostopic echo /cartesian_command
rostopic echo /executor_command
rostopic echo /emergency_stop
rostopic echo /arm_bridge/connection_alive
```

## 13. 第一阶段最小可交付版本

如果你想先快速打通链路，最小版本只需要做到：

1. TCP 监听 `9090`
2. 正确解析 `joint_control`
3. 正确解析 `cartesian_control`
4. 正确解析 `system_command/executor_control`
5. 正确解析 `emergency_stop`
6. heartbeat 超时检测
7. 发布对应 ROS Topic

第一阶段可以先不做：

- 上位机回传 `motor_state`
- 上位机回传 `imu_data`
- 摄像头相关
- 系统状态回传

## 14. 第二阶段扩展建议

等机械臂链路打通后，再考虑补这些能力：

1. 回传 `motor_state` 给上位机，驱动关节显示
2. 回传 `imu_data` 给上位机，驱动姿态视图
3. 增加 `control_command` 支持
4. 增加 `system_status` 回传
5. 增加诊断和统计信息

## 15. 与现有仓库文档的关系

这份文档是“机械臂专用版本”，用于你当前只测试机械臂链路的场景。

更通用的整体对接文档可参考：

- `C:/Users/Lenovo/Documents/hostcomputer/下位机ROS节点对接文档.md`（原 Windows 路径）
- [TCP_JSON协议文档.md](../../TCP_JSON协议文档.md)
- [手柄键盘控制说明.md](../手柄键盘控制说明.md)

如果你后面确认要把底盘、摄像头、环境数据一起打通，再回到通用文档扩展即可。
