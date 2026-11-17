> 历史归档（2026-11-17 整理）。本文保留当时的设计、接口交接或验证记录；版本、路径、命令和临时授权仅对应原记录，不作为当前操作依据。当前说明见 [文档入口](../README.md)。

# 下位机 Bridge 双层架构与调试 UI 计划

## 背景

当前 `ros1_bridge/host_bridge_node.py` 已经能在本机 dry-run 下跑通上位机协议闭环，但协议解析、状态机、输出适配、调试输出还集中在一个文件里。下一步要把它拆成两个职责清晰的部分：

1. **Bridge Core**：解析来自上位机的命令，并转换为 ROS topic / service / 内部状态变化。
2. **Debug UI**：忠实展示 Bridge Core 前半部分的行为，不自行解释协议，也不参与控制决策。

调试 UI 的定位是“观察者”，不是第二套控制逻辑。

## 设计原则

- Bridge Core 是唯一可信状态源。
- Debug UI 只展示 Core 产生的事件和快照，不直接修改 Core 状态。
- 协议输入、解析结果、ROS 输出、service 调用、ACK、watchdog、错误都必须进入统一调试事件流。
- dry-run 和 ROS 模式共用同一套 Core，只替换输出 Adapter。
- 自动化测试优先覆盖 Core 行为，再覆盖 Debug UI 的只读接口。

## 目标结构

```text
ros1_bridge/
  host_bridge_node.py      # 启动入口：参数解析、组装 Core/Adapter/Debug UI
  bridge_protocol.py       # JSON line、协议常量、基础帧构造
  bridge_core.py           # 协议解析、状态机、命令路由、watchdog
  bridge_state.py          # BridgeState、TwistCommand 等数据模型
  output_adapters.py       # DryRunOutput、RosOutput
  debug_events.py          # DebugEvent、EventSink、RingBuffer
  debug_ui.py              # 只读 HTTP 调试 UI
```

## Bridge Core 职责

Core 负责“把上位机协议变成下位机行为”：

- 接收 JSON object。
- 做协议版本、类型、关键字段校验。
- 处理 `heartbeat`，生成 `heartbeat_ack`。
- 处理 `sync_request`，生成 `system_snapshot`。
- 处理 `camera_list_request`，生成 `camera_list_response`。
- 处理 `operator_input`：
  - 丢弃旧 `seq`。
  - 丢弃过期 `ttl_ms`。
  - 急停状态下忽略普通运动输入。
  - 解析键盘和手柄输入。
  - 键盘按 `1` 在车体/机械臂模式之间切换。
  - 键盘按 `6` / `7` / `8` / `9` / `0` 设置 1-5 档速度。
  - 车体模式按 `w/s/a/d` 生成 `/cmd_vel`。
  - 机械臂模式按 `w/s/a/d/u/o/q/e/i/k/j/l` 生成 `/servo_server/delta_twist_cmds`，按 `f/h` 调整夹爪位置。
  - 生成 `TwistCommand`、`ServoCommand` 和可选 `GripperCommand`。
  - 通过 OutputAdapter 输出。
- 处理 `emergency_stop`：
  - 进入急停状态。
  - 输出零速度。
  - 返回 ACK。
  - 广播 `emergency_state`。
- 处理 watchdog：
  - 输入超时后输出零速度。
  - 生成 `system_status`。

## Output Adapter 职责

Adapter 只执行 Core 给出的输出动作：

- `DryRunOutput`
  - 打印/记录 `/cmd_vel`、servo 和夹爪意图。
  - 用于本机回环和自动化测试。
- `RosOutput`
  - 发布 `geometry_msgs/Twist` 到 `/cmd_vel`。
  - 发布 `geometry_msgs/TwistStamped` 到 `/servo_server/delta_twist_cmds`。
  - 发布 `std_msgs/Float64` 到 `/arm_control/gripper_position`。
  - 后续增加 service client：电机初始化、使能、清急停、模式切换等。

Adapter 不解析上位机协议，不维护 ACK，不维护 watchdog。

## Debug Event 模型

Core 的每个关键动作都生成事件：

```jsonc
{
  "id": 123,
  "timestamp_ms": 1709971200000,
  "level": "info",              // debug / info / warning / error
  "category": "operator_input", // tcp / protocol / output / ack / emergency / watchdog / service
  "message": "operator input accepted",
  "data": {
    "seq": 42,
    "pressed_keys": ["w", "d"],
    "cmd_vel": {
      "linear_x": 0.6,
      "angular_z": -1.0
    }
  }
}
```

事件流至少覆盖：

- TCP client connected / disconnected。
- 收到的上位机消息类型、seq。
- 协议错误。
- `operator_input` accepted / dropped_old_seq / dropped_expired / ignored_emergency。
- 输出的 `cmd_vel`。
- watchdog 触发。
- `emergency_stop`、ACK、`emergency_state`。
- `sync_request` / `system_snapshot`。
- `camera_list_request` / `camera_list_response`。
- ROS topic publish / service call。

## Debug UI 职责

第一版 Debug UI 使用只读 HTTP 服务，默认监听 `127.0.0.1:18080`。

接口：

- `GET /`：HTML 页面。
- `GET /api/state`：当前 Core 状态快照。
- `GET /api/events?limit=100`：最近事件。
- `GET /api/cameras`：当前摄像头列表。

页面显示：

- 连接状态和最近客户端地址。
- 急停状态。
- watchdog 状态。
- 最近 `operator_input` seq 和处理结果。
- 当前 `cmd_vel`。
- 最近 ACK。
- 最近协议错误。
- 摄像头列表。
- 最近事件表。

Debug UI 不提供控制按钮。后续如果要做人工测试按钮，必须走同样的协议入口或显式标记为调试注入。

## Commit 计划

1. `docs(bridge): plan core debug ui split`
   - 记录本文档。

2. `refactor(bridge): split protocol state and adapters`
   - 从 `host_bridge_node.py` 拆出：
     - `bridge_protocol.py`
     - `bridge_state.py`
     - `output_adapters.py`
   - 保持现有 `bridge_loopback_test.py` 通过。

3. `refactor(bridge): move command handling into bridge core`
   - 新增 `bridge_core.py`。
   - Core 接管协议解析、状态机、watchdog、响应生成。
   - TCP server 只负责收发和调用 Core。

4. `feat(bridge): add debug event ring buffer`
   - 新增 `debug_events.py`。
   - Core、Adapter、TCP server 写入统一事件流。
   - 回环测试验证事件中包含 accepted、watchdog、emergency。

5. `feat(bridge): add read only debug http ui`
   - 新增 `debug_ui.py`。
   - `host_bridge_node.py` 增加 `--debug-ui`、`--debug-host`、`--debug-port` 参数。
   - 默认 dry-run 可开启 HTTP 调试页面。
   - 已完成。

6. `test(bridge): cover debug ui endpoints`
   - 扩展 `scripts/bridge_loopback_test.py`。
   - 验证 `/api/state`、`/api/events`。
   - `scripts/run_all_tests.sh` 继续默认覆盖。
   - 已完成。

7. `docs(bridge): document debug ui workflow`
   - 更新回环计划和人工联调命令。
   - 写明 Debug UI 只读边界。

## 完成标准

- `scripts/bridge_loopback_test.py` 通过。
- `scripts/protocol_smoke_test.py` 通过。
- `scripts/run_all_tests.sh` 通过。
- `host_bridge_node.py --dry-run --debug-ui` 能启动 HTTP 调试页面。
- `/api/state` 能返回急停、watchdog、last_operator_seq、last_twist。
- `/api/events` 能看到 operator_input、cmd_vel、watchdog、emergency 等事件。
- 上位机 GUI 连接 dry-run bridge 后，Debug UI 能忠实展示 Core 的行为。

## 非目标

本阶段不做：

- 完整前端复杂交互 UI。
- 实机电机/机械臂 service 细节。
- RTSP 播放器状态机。
- 权限认证和远程访问安全策略。

这些在 bridge 结构稳定后再进入下一阶段。
