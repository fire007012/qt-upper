# TCP JSON 协议文档

通信端口：**9090**（默认）
传输方式：TCP 长连接，每条 JSON 消息以 `\n` 结尾
数据编码：UTF-8

> 当前生产Qt使用控制v2（文档40），管理服务独立使用v1（文档39/41）。使用和恢复见[Qt控制v2与诊断](docs/Qt控制v2与诊断使用.md)，版本见[兼容声明](deploy/compatibility.json)。下文保留v1遥测/相机报文及旧控制基线；其中旧运动写入与ACK授权不能用于当前v2。

控制v2请求用 `protocol_version:2`、`request_id`、session_id/epoch；输入及授权/业务写请求附递增seq、freshness_token和ttl_ms。响应为 `control_result`，回显request_id/seq及ok/result或error.code。token按服务端refresh_ms刷新（当前通常100ms），最多300ms有效；会话租约1500ms，watchdog至多500ms。Qt用本地单调发送时刻扣除RTT，过期必须重新采样，不重放旧输入。

两级授权分别为authorize_operations和authorize；实际runtime许可未知或过期时不能运动。业务命令通过有界异步operation查询完成状态，accepted不等于成功；未知接受只能显式原ID恢复。独立急停响应包含 `physical_stop_confirmed:false`，不能代替硬件停止证据。旧v1心跳、同步、相机只读接口用于观测，不得改变v2授权。

控制 v2 现场增量（文档52）：客户端额外声明 `speed_level_v1` 和 `latched_global_stop_v1`，缺少能力时禁用对应新增操作并提示更新。换档仅用 `system_command set_speed_level`、`params:{"level":1..5}`，连续输入不携带档位或数字按键；必须新中立、操作授权、异步终态及实际档位读回，随后重新显式授权。服务端速度声明原样展示，不在 Qt 推测限值。

独立 `emergency_stop` 仅表示锁存与请求；查询 `global_stop` 和 `safety_halt` 分别展示独立禁止和普通清理，结合新鲜 `runtime_global_inhibit` 判断当前软件禁止。显式 `clear_emergency` 无需操作授权且仅清桥接锁存，`runtime_release_required:true` 后需重新中立/操作授权，显式提交 `system_command reset_global_stop`、`params:{}`。复位不恢复运动，重连不自动复位。三类 `safety_conditions` 的 unknown/expired/stale 不表示已安全。完整操作、修改文件及部署见 [Qt 实车联调控制 v2](docs/Qt实车联调控制v2使用与验收.md)。

## 通用帧约束

- 每条 JSON 帧以 `\n` 结尾。
- 当前生产控制v2及Qt接收单帧上限：`65536` 字节（含换行）。
- v1 帧应包含 `protocol_version: 1`、`seq`、`timestamp_ms`。
- 上位机接收侧会对关键帧做基础 schema 校验；坏 JSON、超大帧、缺关键字段会生成 `protocol_error`。

### 上位机恢复与遥控授权（2026-11-08）

以下为第一批v1客户端历史行为；当前生产v2规则以上文和控制操作指南为准。

- TCP 建连时限 10s；成功后立即发心跳，之后每 1s 发送。只接受当前连接中匹配待处理序号且未超时的 `heartbeat_ack`；服务端单向 heartbeat 不作为双向通信正常的依据。
- 首个 ACK 未到或最近有效 ACK 超过 3s，客户端主动丢弃旧连接与发送缓冲，进入持续重试。重试间隔从约 3s、6s 增长至最多 10s，含少量抖动；首次连接失败同样重试。主动断开会取消正在连接和等待重试。
- 重连清除解析残留、待处理心跳和命令 ACK，不重放旧命令。内部 RTT、ACK 时限和心跳检测使用本地单调时间；线上 `timestamp_ms` 和对外统计仍为 Unix 毫秒时间。
- 每次连接均默认暂停遥控。需收到对应 `sync_request` 序号的 `system_snapshot`（包含布尔 `emergency.active`）、有效心跳、未激活急停，并采集到最近 300ms 内的中立输入，才可点击“允许遥控”。中立要求键盘和按钮释放、已连接手柄各轴绝对值不超过 0.04。
- 暂停期间仅发送中立输入；关节、电机、末端和命名位姿运动入口受本地授权限制。急停、halt、disable、shutdown、recover、watchdog 或输入拒收会撤销遥控授权。此授权不替代下位机的 init/enable/resume、控制租约或硬件运行许可。
- 发送缓冲与单次输出合计上限 64KiB，超限关闭连接而不继续积压旧输入；待处理关键请求上限 128，急停可替换一个待跟踪项。关键 ACK 等待 2s，类型不匹配不消耗请求；超时表示结果不确定，不自动重试有副作用的命令。
- 桥接未在连接后 10s 内完成有效状态同步时重连。该实现尚未完成新协议的 session/freshness 协商；跨机器时钟偏差、桥接 TTL 和下位机 watchdog 仍需要下位机配合修复及联调。

---

## 一、上位机 → 下位机（发送）

### 1. motor_command — 电机控制命令

发送6关节+执行器的控制数据。

```json
{
  "type": "motor_command",
  "joints": [
    { "position": 1.0, "current": 0.5 },
    { "position": 2.0, "current": 0.6 },
    { "position": 1.5, "current": 0.4 },
    { "position": 0.8, "current": 0.3 },
    { "position": 1.2, "current": 0.7 },
    { "position": 0.9, "current": 0.5 }
  ],
  "executor_position": 0.5,
  "executor_torque": 1.0,
  "executor_flags": 1,
  "reserved": 0
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| joints | array[6] | 6个关节数据 |
| joints[].position | float | 关节位置（实际值，非×1000） |
| joints[].current | float | 关节电流（实际值） |
| executor_position | float | 执行器位置 |
| executor_torque | float | 执行器扭矩 |
| executor_flags | int | 执行器标志位 |
| reserved | int | 保留字段 |

---

### 2. joint_control — 单关节控制命令

控制单个关节的位置和速度。

```json
{
  "type": "joint_control",
  "joint_id": 0,
  "position": 1.5,
  "velocity": 0.8,
  "timestamp": 1709971200000
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| joint_id | int | 关节ID（0-5） |
| position | float | 目标位置 |
| velocity | float | 目标速度 |
| timestamp | int64 | 毫秒级时间戳 |

---

### 3. operator_input — 操作者输入快照

承载键盘与手柄当前输入状态。该帧不逐条 ACK，靠 `seq`、`ttl_ms` 和下位机 watchdog 保证高频输入安全。

```json
{
  "type": "operator_input",
  "protocol_version": 1,
  "seq": 12345,
  "timestamp_ms": 1709971200000,
  "ttl_ms": 300,
  "mode": "vehicle",
  "keyboard": {
    "pressed_keys": ["w", "d", "shift"]
  },
  "gamepad": {
    "connected": true,
    "buttons": {
      "a": false,
      "b": false,
      "x": false,
      "y": false,
      "start": false,
      "back": false,
      "lb": false,
      "rb": false,
      "l3": false,
      "r3": false,
      "dpad_up": false,
      "dpad_down": false,
      "dpad_left": false,
      "dpad_right": false
    },
    "axes": {
      "left_x": 0.0,
      "left_y": 0.82,
      "right_x": -0.31,
      "right_y": 0.0,
      "lt": 0.0,
      "rt": 0.5
    }
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| protocol_version | int | 协议版本，当前为 1 |
| seq | int64 | 上位机单调递增序号 |
| timestamp_ms | int64 | 毫秒级时间戳 |
| ttl_ms | int | 输入快照有效期 |
| mode | string | `vehicle` / `arm` |
| keyboard.pressed_keys | array[string] | 当前按下集合，未出现的键视为未按下 |
| gamepad.connected | bool | 手柄是否在线 |
| gamepad.buttons | object | 手柄按钮完整快照，摇杆按下使用 `l3` / `r3` |
| gamepad.axes | object | 摇杆范围 `[-1.0, 1.0]`，扳机范围 `[0.0, 1.0]` |

---

### 4. emergency_stop — 急停命令

紧急停止所有运动。

```json
{
  "type": "emergency_stop",
  "protocol_version": 1,
  "seq": 2001,
  "timestamp_ms": 1709971200000,
  "params": {
    "source": "button"
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| protocol_version | int | 协议版本，当前为 1 |
| seq | int64 | 上位机单调递增序号 |
| timestamp_ms | int64 | 毫秒级时间戳 |
| params.source | string | 触发来源，例如 `button`、`keyboard_space`、`gamepad_l3_r3` |

---

### 5. system_command — 系统命令

通用系统命令，可携带任意参数。当前 UI 直接使用它触发生命周期服务和控制域切换。

```json
{
  "type": "system_command",
  "protocol_version": 1,
  "seq": 42,
  "command": "enable",
  "params": {},
  "timestamp_ms": 1709971200000
}
```

生命周期命令由 ROS2 `host_bridge` 映射到 `/robot_runtime/{command}` 的 `std_srvs/srv/Trigger` 服务（现场可由配置调整）：
`init`、`enable`、`disable`、`halt`、`resume`、`recover`、`shutdown`。

控制域只表示上位机输入转发所处的业务域，不表示底层关节控制模式：

```json
{
  "type": "system_command",
  "protocol_version": 1,
  "seq": 43,
  "command": "set_control_mode",
  "params": {
    "mode": "arm"
  },
  "timestamp_ms": 1709971200000
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| command | string | 命令名称 |
| params | object | 命令参数（任意键值对） |
| params.mode | string | `set_control_mode` 使用，取值 `vehicle` / `arm` |
| timestamp_ms | int64 | 毫秒级时间戳 |

---

### 6. heartbeat — 心跳包

每秒自动发送一次，用于保活检测。

```json
{
  "type": "heartbeat",
  "protocol_version": 1,
  "seq": 3000,
  "timestamp_ms": 1709971200000
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| protocol_version | int | 协议版本，当前为 1 |
| seq | int64 | 上位机单调递增序号 |
| timestamp_ms | int64 | 毫秒级时间戳 |

下位机应返回：

```json
{
  "type": "heartbeat_ack",
  "protocol_version": 1,
  "seq": 3000,
  "timestamp_ms": 1709971200100,
  "server_time_ms": 1709971200100
}
```

---

### 7. ack — 关键命令确认

关键离散命令必须返回 ACK。上位机会在发送后登记 pending，超时、断线、迟到 ACK 都会进入 UI 日志。

```json
{
  "type": "ack",
  "protocol_version": 1,
  "ack_type": "emergency_stop",
  "seq": 2001,
  "ok": true,
  "code": 0,
  "message": "emergency active",
  "timestamp_ms": 1709971200100
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| ack_type | string | 被确认的命令类型 |
| seq | int64 | 对应原命令的 `seq` |
| ok | bool | 是否成功 |
| code | int | 0 成功，非 0 为错误码 |
| message | string | 人类可读信息 |

mock server 可用环境变量模拟 ACK 行为：

- `MOCK_ACK_MODE=ok`：默认成功 ACK。
- `MOCK_ACK_MODE=fail`：返回失败 ACK。
- `MOCK_ACK_MODE=drop`：不返回 ACK，用于测试超时。
- `MOCK_ACK_DELAY_SEC=3`：延迟 ACK，用于测试迟到 ACK 或超时。

---

### 7.1 service_call_result — ROS service 调用结果

当下位机为了执行关键命令调用 ROS service 时，应额外回传一次简略结果。它不替代 `ack`：`ack` 仍用于命令链路确认，`service_call_result` 用于展示真实 service 执行结果。

```json
{
  "type": "service_call_result",
  "protocol_version": 1,
  "seq": 2002,
  "timestamp_ms": 1709971200100,
  "request_type": "system_command",
  "command": "enable",
  "service": "/robot_runtime/enable",
  "ok": true,
  "code": 0,
  "message": "enabled",
  "duration_ms": 18
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| seq | int64 | 对应原 TCP 命令的 `seq` |
| request_type | string | 原 TCP 消息类型，例如 `system_command` / `emergency_stop` |
| command | string | 触发的命令名，例如 `enable` / `set_control_mode` |
| service | string | ROS service 名称 |
| ok | bool | service 调用是否成功 |
| code | int | 0 成功，非 0 为错误码 |
| message | string | 简略结果说明 |
| duration_ms | int | service 调用耗时，毫秒 |

当前 ROS2 桥接的服务路径、模式切换及结果语义应按下位机部署配置和 [ROS2接口清单](docs/ROS2上位机接口对照与联调清单.md) 核对。上位机仅发送 TCP 业务请求，不在本仓库启动或配置旧桥接。

管理服务启停是独立管理协议，不使用此消息；管理请求成功仅表示任务已接受，实际结果需查询 operation/snapshot，见 [文档39](ros2_ws_car/src/docs/39_下位机管理协议与Qt对接.md)。

---

### 8. sync_request — 状态同步请求

上位机连接或重连后自动发送，请求下位机返回当前系统快照。

```json
{
  "type": "sync_request",
  "protocol_version": 1,
  "seq": 3010,
  "timestamp_ms": 1709971200000,
  "params": {
    "reason": "connected"
  }
}
```

### 9. camera_list_request — 摄像头列表请求

上位机连接或重连后自动发送，请求下位机返回全部摄像头状态。

```json
{
  "type": "camera_list_request",
  "protocol_version": 1,
  "seq": 3020,
  "timestamp_ms": 1709971200000
}
```

---

## 二、下位机 → 上位机（接收）

### 0. hello / capabilities — bridge 握手

下位机 bridge 建立连接后可以主动发送 `hello` 和 `capabilities`，用于说明 bridge 身份和支持的协议能力。

```json
{
  "type": "hello",
  "protocol_version": 1,
  "seq": 0,
  "timestamp_ms": 1709971200000,
  "bridge_name": "host_bridge_node",
  "bridge_version": "1.0.0",
  "robot_id": "robot_001"
}
```

```json
{
  "type": "capabilities",
  "protocol_version": 1,
  "seq": 0,
  "timestamp_ms": 1709971200000,
  "supports": [
    "operator_input",
    "heartbeat_ack",
    "critical_ack",
    "sync_request",
    "camera_list_request",
    "joint_runtime_states",
    "hybrid_lifecycle_services"
  ],
  "max_frame_bytes": 1048576
}
```

### 0.1 system_snapshot / camera_list_response — 同步响应

```json
{
  "type": "system_snapshot",
  "protocol_version": 1,
  "seq": 3010,
  "timestamp_ms": 1709971200100,
  "control_mode": "vehicle",
  "emergency": { "active": false, "source": "" },
  "motor": { "initialized": true, "enabled": true },
  "modules": {
    "base": "online",
    "arm": "online",
    "camera": "degraded"
  },
  "last_error": { "code": 0, "message": "" }
}
```

```json
{
  "type": "camera_list_response",
  "protocol_version": 1,
  "seq": 3020,
  "timestamp_ms": 1709971200100,
  "cameras": [
    {
      "camera_id": 0,
      "name": "front",
      "online": true,
      "rtsp_url": "rtsp://192.168.1.50:8554/front",
      "codec": "h264",
      "width": 1280,
      "height": 720,
      "fps": 25,
      "bitrate_kbps": 2500
    }
  ]
}
```

### 1. motor_state — 电机状态数据

下位机上报6关节+执行器的实时状态。

```json
{
  "type": "motor_state",
  "joints": [
    { "position": 1.0, "current": 0.5 },
    { "position": 2.0, "current": 0.6 },
    { "position": 1.5, "current": 0.4 },
    { "position": 0.8, "current": 0.3 },
    { "position": 1.2, "current": 0.7 },
    { "position": 0.9, "current": 0.5 }
  ],
  "executor_position": 0.5,
  "executor_torque": 1.0,
  "executor_flags": 1,
  "reserved": 0
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| joints | array[6] | 6个关节状态 |
| joints[].position | float | 关节位置（上位机内部×1000转int16存储） |
| joints[].current | float | 关节电流（上位机内部×1000转int16存储） |
| executor_position | float | 执行器位置 |
| executor_torque | float | 执行器扭矩 |
| executor_flags | int | 执行器标志位 |
| reserved | int | 保留字段 |

---

### 1.1 joint_runtime_states — 关节生命周期状态

ROS2 桥接从 `/robot_runtime/state` 的运行状态 JSON 提取关节状态，编码为此 TCP 消息供上位机显示。Qt 不依赖 ROS 消息包；实际来源及未就绪/fault 映射见 [ROS2接口清单](docs/ROS2上位机接口对照与联调清单.md)。

```json
{
  "type": "joint_runtime_states",
  "protocol_version": 1,
  "seq": 0,
  "timestamp_ms": 1709971200000,
  "states": [
    {
      "joint_name": "left_front_arm_joint",
      "backend": "canopen",
      "lifecycle_state": "Running",
      "online": true,
      "enabled": true,
      "fault": false
    }
  ]
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| states | array | 关节运行态列表 |
| states[].joint_name | string | 关节名 |
| states[].backend | string | 后端，例如 `canopen` / `can_driver` |
| states[].lifecycle_state | string | 统一生命周期状态 |
| states[].online | bool | 当前反馈是否在线/新鲜 |
| states[].enabled | bool | 关节是否使能 |
| states[].fault | bool | 是否处于故障状态 |

---

### 2. joint_data — 单关节数据

下位机上报单个关节的详细数据。

```json
{
  "type": "joint_data",
  "joint_id": 0,
  "position": 1.5,
  "current": 0.5,
  "torque": 0.8
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| joint_id | int | 关节ID（0-5） |
| position | float | 关节位置 |
| current | float | 关节电流 |
| torque | float | 关节扭矩 |

---

### 3. environment / co2 — 环境传感器数据

下位机上报CO2浓度数据。支持两种 `type` 值：`"environment"` 或 `"co2"`。

**格式一（嵌套 data）：**
```json
{
  "type": "environment",
  "data": {
    "co2_ppm": 420.5,
    "timestamp": 1709971200
  }
}
```

**格式二（扁平）：**
```json
{
  "type": "co2",
  "co2_ppm": 420.5,
  "timestamp": 1709971200
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| co2_ppm | float | CO2浓度（ppm） |
| timestamp | int | 秒级时间戳 |

**UI显示阈值：**
| 浓度范围 | 颜色 | 状态 |
|----------|------|------|
| < 800 ppm | 绿色 | 空气良好 |
| 800 - 1500 ppm | 橙色 | 注意通风 |
| > 1500 ppm | 红色 | 浓度过高 |

---

### 4. system_status — 系统状态

下位机上报系统运行状态，内容为任意键值对。

```json
{
  "type": "system_status",
  "cpu_usage": 45,
  "memory_usage": 60,
  "uptime": 3600,
  "ros_status": "running"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| (任意) | any | 由下位机自行定���的状态字段 |

---

### 5. camera_info — 摄像头信息推送

下位机上报摄像头的 RTSP 地址及参数信息，上位机收到后自动在对应格子启动 RTSP 播放。

```json
{
  "type": "camera_info",
  "camera_id": 0,
  "online": true,
  "codec": "h265",
  "width": 1280,
  "height": 720,
  "fps": 30,
  "bitrate_kbps": 2000,
  "rtsp_url": "rtsp://192.168.1.100:8554/cam0"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| camera_id | int | 摄像头ID（0-4），对应上位机 2x3 网格的前5个格子 |
| online | bool | true=上线自动播放，false=离线停止播放 |
| codec | string | 编码格式（如 "h264"、"h265"） |
| width | int | 视频宽度（像素） |
| height | int | 视频高度（像素） |
| fps | int | 帧率 |
| bitrate_kbps | int | 码率（kbps） |
| rtsp_url | string | RTSP 流地址 |

---

## 三、协议约定

1. **传输格式**：每条消息为一行紧凑 JSON，以 `\n` 分隔
2. **必要字段**：所有消息必须包含 `"type"` 字段
3. **时间戳**：v1 使用 `timestamp_ms`（Unix 毫秒）；本地超时使用单调时间，不直接使用跨机器时间差
4. **心跳机制**：每 1 秒发送 heartbeat，有效 ACK 超时主动重建连接，持续退避重试直至主动断开；详见上文恢复与遥控授权约定
5. **数值精度**：motor_state 中的 position/current 为浮点值，上位机内部乘以 1000 转为 int16 存储


## 四、独立管理协议与控制桥兼容

管理连接使用独立 TCP 9091，与本文件的控制桥协议分开；请求字段为 version/request_id/token/method/params，不使用控制桥 type/seq。完整契约见 [文档39](ros2_ws_car/src/docs/39_下位机管理协议与Qt对接.md)，Qt 使用方式见 [运行管理说明](docs/Qt运行管理使用与联调.md)。

管理路径连接控制桥时额外核对 sync_request、readonly_monitor、monotonic_watchdog、connection_control_owner 四项 capabilities.supports，能力未确认时不能允许遥控，缺失时关闭连接。心跳与匹配状态快照仍须分别通过。服务端 protocol_error/code=2110 表示另一连接持有控制权，客户端撤销本地遥控；不自动抢占或重新授权。

这些 v1 能力不包含显式 session_id/freshness token 或完整租约；旧输入积压与慢服务可靠性仍待双方升级协议并验收。管理操作 accepted 仅代表接收，只有查询任务及实际快照才能确认软件结果；软件 ready 不能代替运动授权。

## 五、2026-11-14 控制 v2 联动增量

当前联动使用管理 v1、控制 v2。`control_status.result.global_stop_reset_ready` 为可选布尔字段，反映下位机对新鲜诊断、所有者心跳、C++ 禁止、组状态/操作和普通 halt 清理完成情况的判断。字段为 false 时，上位机等待后再明确提交清除急停或复位；它不表示机械停止已确认。省略字段的旧控制 v2 服务沿用既有门槛，不静默升级安全声明。`reset_global_stop` 在清理未结束时返回 `RUNTIME_NOT_SETTLED`，不会自动重发或恢复运动。

完整安装栈与本地 Mock 启动方法见 [联动交付记录](docs/上下位机联动交付记录.md)。
