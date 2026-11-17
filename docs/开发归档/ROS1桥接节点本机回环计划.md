> 历史归档（2026-11-17 整理）。本文保留当时的设计、接口交接或验证记录；版本、路径、命令和临时授权仅对应原记录，不作为当前操作依据。当前说明见 [文档入口](../README.md)。

# ROS1 host_bridge_node 本机回环计划

## 目标

在接真实 NUC 和硬件前，先在本机跑一个下位机 bridge MVP，验证上位机协议闭环：

- TCP JSON 长连接监听 `127.0.0.1:9090`。
- 连接后主动发送 `hello` 和 `capabilities`。
- 响应上位机自动发出的 `sync_request` 和 `camera_list_request`。
- 接收 `operator_input`，解析键盘/手柄输入意图。
- 处理 `emergency_stop` 并返回 ACK。
- 在输入过期或连接断开时进入安全输出。

## 运行模式

bridge 节点支持两种模式：

- `--dry-run`：不依赖 ROS1，只打印或记录 `/cmd_vel` 意图，适合在开发机本机回环测试。
- `--ros`：使用 `rospy` 发布真实 ROS topic，适合部署到下位机 NUC。

默认先实现和验证 `--dry-run`。`--ros` 保留同一套协议逻辑，只替换输出适配层。

## 协议职责

### 连接握手

客户端连接后，bridge 立即发送：

- `hello`
- `capabilities`

上位机随后应自动发送：

- `sync_request`
- `camera_list_request`

bridge 分别返回：

- `system_snapshot`
- `camera_list_response`

### 高频输入

`operator_input` 不逐条 ACK。bridge 按以下规则处理：

- `seq` 小于等于最近处理序号时丢弃。
- 当前时间超过 `timestamp_ms + ttl_ms` 时丢弃。
- 急停激活时忽略运动输入。
- 键盘 `pressed_keys` 和手柄 axes/buttons 同时参与解析。
- 超过 watchdog 时间未收到有效输入时输出零速度。

### 关键命令

`emergency_stop` 必须返回 ACK，并进入急停状态：

- 返回 `ack`，`ack_type=emergency_stop`。
- 主动广播 `emergency_state`。
- 进入急停后输出零速度，并忽略普通运动输入。

后续可补：

- `clear_emergency`
- `motor_init`
- `motor_enable`
- `motor_disable`
- `set_control_mode`

## ROS 映射

MVP 映射：

| 协议输入 | ROS 输出 |
|----------|----------|
| `operator_input.keyboard.pressed_keys` | `/cmd_vel`、`/servo_server/delta_twist_cmds`、`/arm_control/gripper_position` |
| `operator_input.gamepad.axes/buttons` | `/cmd_vel`、`/servo_server/delta_twist_cmds` |
| `emergency_stop` | 零速度 + 急停状态 |

初始键盘映射由 bridge 维护：

| 输入名 | 语义 |
|--------|------|
| `1` | 车体/机械臂模式切换 |
| `space` | 急停兜底触发；上位机仍应同时发送 `emergency_stop` |
| `6` / `7` / `8` / `9` / `0` | 五档速度，1 档到 5 档 |
| 车体 `w` / `s` | 前进 / 后退 |
| 车体 `a` / `d` | 左转 / 右转 |
| 机械臂 `w` / `s` | linear z + / - |
| 机械臂 `a` / `d` | linear y + / - |
| 机械臂 `u` / `o` | linear x + / - |
| 机械臂 `q` / `e` | roll + / - |
| 机械臂 `i` / `k` | pitch - / + |
| 机械臂 `j` / `l` | yaw + / - |
| 机械臂 `f` / `h` | 夹爪开 / 关 |

这符合“上位机只发协议层输入名，下位机负责解释运动语义”的设计。

## 自动化回环测试

新增 `scripts/bridge_loopback_test.py`：

1. 启动 `ros1_bridge/host_bridge_node.py --dry-run`，监听本机临时端口。
2. 作为客户端连接 bridge。
3. 验证 `hello` / `capabilities`。
4. 发送 `heartbeat`，验证 `heartbeat_ack`。
5. 发送 `sync_request`，验证 `system_snapshot`。
6. 发送 `camera_list_request`，验证 `camera_list_response`。
7. 发送 `operator_input`，验证 bridge 上报最近输入状态。
8. 发送 `emergency_stop`，验证 ACK 和 `emergency_state`。
9. 等待 watchdog，验证零速度安全输出。

当前落地文件：

- `ros1_bridge/host_bridge_node.py`：bridge 启动入口，支持 `--dry-run` / `--ros` / `--debug-ui`。
- `ros1_bridge/bridge_core.py`：协议解析、状态机、watchdog、响应生成。
- `ros1_bridge/output_adapters.py`：dry-run 和 ROS 输出适配。
- `ros1_bridge/debug_events.py`：调试事件 ring buffer。
- `ros1_bridge/debug_ui.py`：只读 HTTP 调试 UI。
- `scripts/bridge_loopback_test.py`：本机回环自动化测试，覆盖协议和调试 UI API。
- `scripts/run_all_tests.sh`：默认运行 build smoke、protocol smoke 和 bridge loopback。

## 人工 GUI 回环测试

1. 启动 bridge：

   ```bash
   python3 ros1_bridge/host_bridge_node.py --dry-run --debug-ui --host 127.0.0.1 --port 9090
   ```

2. 启动上位机 GUI。
3. TCP 连接 `127.0.0.1:9090`。
4. 确认 UI 日志出现：
   - `hello`
   - `capabilities`
   - `system_snapshot`
   - `camera_list_response`
5. 按键触发 `operator_input`，观察 bridge 控制台输出速度意图。
6. 点击急停，确认 UI 收到 ACK，bridge 进入急停状态。
7. 断开并重连，确认上位机重新触发同步请求。
8. 打开 `http://127.0.0.1:18080`，确认 Debug UI 能看到 state、camera list 和最近事件。

## 完成标准

- `scripts/protocol_smoke_test.py` 通过。
- `scripts/bridge_loopback_test.py` 通过。
- `scripts/run_all_tests.sh` 默认包含 bridge loopback。
- 上位机 GUI 能本机连接 dry-run bridge 并完成同步。
- 急停、ACK、watchdog、安全零输出可以在 dry-run 下验证。
- Debug UI 的 `/api/state` 和 `/api/events` 能忠实展示 Core 行为。
