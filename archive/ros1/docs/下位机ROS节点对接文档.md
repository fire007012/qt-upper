> **已退役：ROS1 历史资料，不适用于当前 ROS2 系统。** 当前入口见 [ROS2 对接说明](../../../下位机ROS节点对接文档.md) 和 [ROS2 视频联调](../../../docs/ROS2视频联调说明.md)。原路径和命令仅保留供追溯，不要用于当前部署。

# 下位机 ROS 节点对接文档

本文档面向下位机开发者（Intel NUC / Ubuntu 20.04 + ROS1），说明如何编写 ROS 节点与上位机 TCP 通信。

> 当前协议已升级到 host bridge protocol v1。上位机不再发送 `cmd_vel` JSON 帧，而是发送 `operator_input` 输入快照；下位机 `host_bridge_node` 负责根据模式、映射表和安全策略解析输入，再发布 ROS `/cmd_vel` 或机械臂控制话题。

---

## 一、通信概述

| 项目 | 说明 |
|------|------|
| 协议 | TCP 长连接 + JSON |
| 端口 | **9090**（下位机作为 Server 监听） |
| 消息格式 | 每条紧凑 JSON 以 `\n` 结尾 |
| 编码 | UTF-8 |
| 心跳 | 上位机每 1 秒发送 `heartbeat`，5 次无响应后重连 |

**角色分工**：
- **下位机**：TCP Server，监听 9090 端口，等待上位机连接
- **上位机**：TCP Client，主动连接下位机 IP:9090

---

## 二、ROS 节点参考架构

```
┌──────────────────────────────────────────────────────────────┐
│                    ROS 节点: tcp_bridge                      │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  TCP Server (Port 9090)                                      │
│       ↕ JSON + '\n'                                          │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ 订阅 ROS Topic（发给上位机）          JSON type         │ │
│  │─────────────────────────────────────────────────────────│ │
│  │ /joint_states   (sensor_msgs/JointState)  motor_state  │ │
│  │ /joint_data     (自定义)                  joint_data   │ │
│  │ /imu/data       (sensor_msgs/Imu)         imu_data     │ │
│  │ /co2_sensor     (std_msgs/Float32)        co2_data     │ │
│  │ /camera_info    (自定义)                  camera_info  │ │
│  │ /system_status  (自定义)                  system_status│ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │ 发布 ROS Topic（来自上位机）          JSON type         │ │
│  │─────────────────────────────────────────────────────────│ │
│  │ /operator_input   (自定义)            operator_input   │ │
│  │ /emergency_stop   (std_msgs/Bool)        emergency_stop│ │
│  │ /joint_command    (自定义)               joint_control │ │
│  │ /cartesian_command(自定义)            cartesian_control│ │
│  │ /motor_command    (自定义)               motor_command │ │
│  │ /control_command  (自定义)            control_command  │ │
│  │ /system_command   (自定义)            system_command   │ │
│  └─────────────────────────────────────────────────────────┘ │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 话题汇总表

#### 需订阅的 ROS Topic（下位机 → 上位机）

| ROS Topic | 消息类型 | JSON type | 建议频率 | 必须 |
|-----------|---------|-----------|---------|------|
| `/joint_states` | `sensor_msgs/JointState` | `motor_state` | 10-50Hz | ✅ |
| `/joint_data` | 自定义 | `joint_data` | 按需 | |
| `/imu/data` | `sensor_msgs/Imu` | `imu_data` | 10-50Hz | |
| `/co2_sensor` | `std_msgs/Float32` | `co2_data` | 1Hz | |
| `/camera_info` | 自定义 | `camera_info` | 事件触发 | ✅ |
| `/system_status` | 自定义 | `system_status` | 1Hz | |

#### 需发布的 ROS Topic（上位机 → 下位机）

| ROS Topic | 消息类型 | JSON type | 说明 | 必须 |
|-----------|---------|-----------|------|------|
| `/operator_input` | 自定义 | `operator_input` | 键盘/手柄输入快照，由 bridge 解析为 `/cmd_vel` 或机械臂控制 | ✅ |
| `/emergency_stop` | `std_msgs/Bool` | `emergency_stop` | 急停，优先级最高 | ✅ |
| `/joint_command` | 自定义 | `joint_control` | 单关节位置控制 | ✅ |
| `/cartesian_command` | 自定义 | `cartesian_control` | 末端笛卡尔控制 | ✅ |
| `/motor_command` | 自定义 | `motor_command` | 6关节+执行器批量控制 | ✅ |
| `/control_command` | 自定义 | `control_command` | 综合控制（IMU+摆臂+机械臂末端） | ✅ |
| `/system_command` | 自定义 | `system_command` | 系统命令（复位/模式切换等） | |

---

## 三、下位机需要发送的消息（Server → Client）

### 3.1 motor_state — 电机状态（必须）

周期性上报 6 关节 + 执行器的实时状态，建议 10-50Hz。

```json
{"type":"motor_state","joints":[{"position":1.0,"current":0.5},{"position":2.0,"current":0.6},{"position":1.5,"current":0.4},{"position":0.8,"current":0.3},{"position":1.2,"current":0.7},{"position":0.9,"current":0.5}],"executor_position":0.5,"executor_torque":1.0,"executor_flags":1,"reserved":0}\n
```

| 字段 | 类型 | 说明 |
|------|------|------|
| joints | array[6] | 6 个关节，每个包含 position 和 current |
| joints[i].position | float | 关节位置（弧度或自定义单位） |
| joints[i].current | float | 关节电流（安培） |
| executor_position | float | 末端执行器位置 |
| executor_torque | float | 末端执行器扭矩 |
| executor_flags | int | 标志位（自定义） |
| reserved | int | 保留，填 0 |

---

### 3.2 joint_data — 单关节数据

单独上报某个关节的详细数据，适用于调试或高频单关节监控场景。

```json
{"type":"joint_data","joint_id":0,"position":1.5,"current":0.5,"torque":0.8}\n
```

| 字段 | 类型 | 说明 |
|------|------|------|
| joint_id | int | 关节 ID（0-5） |
| position | float | 关节位置 |
| current | float | 关节电流（A） |
| torque | float | 关节扭矩（Nm） |

---

### 3.4 imu_data — IMU 姿态数据

上报车体姿态，建议 10-50Hz。

```json
{"type":"imu_data","roll":0.1,"pitch":-0.05,"yaw":1.57,"accel_x":0.01,"accel_y":0.02,"accel_z":9.81}\n
```

| 字段 | 类型 | 说明 |
|------|------|------|
| roll | float | 横滚角（rad） |
| pitch | float | 俯仰角（rad） |
| yaw | float | 偏航角（rad） |
| accel_x | float | X 轴加速度（m/s²） |
| accel_y | float | Y 轴加速度（m/s²） |
| accel_z | float | Z 轴加速度（m/s²） |

---

### 3.5 co2_data — CO2 浓度

环境传感器数据，建议 1Hz。

```json
{"type":"co2_data","ppm":420.5}\n
```

| 字段 | 类型 | 说明 |
|------|------|------|
| ppm | float | CO2 浓度（ppm） |

---

### 3.6 camera_info — 摄像头信息推送（必须）

每个摄像头上线/离线时发送一次，上位机收到后自动启动/停止 RTSP 播放。

```json
{"type":"camera_info","camera_id":0,"online":true,"codec":"h265","width":1280,"height":720,"fps":30,"bitrate_kbps":2000,"rtsp_url":"rtsp://192.168.1.100:8554/cam0"}\n
```

| 字段 | 类型 | 说明 |
|------|------|------|
| camera_id | int | 摄像头 ID，**0-4**（对应上位机 2×3 网格前 5 格） |
| online | bool | true = 上线自动播放，false = 离线停止 |
| codec | string | 编码格式："h264" / "h265" |
| width | int | 视频宽度（像素） |
| height | int | 视频高度（像素） |
| fps | int | 帧率 |
| bitrate_kbps | int | 码率（kbps） |
| rtsp_url | string | RTSP 流地址 |

**使用场景**：
- 节点启动后，枚举所有摄像头，为每个在线的摄像头发送一条 `camera_info`
- 摄像头掉线时，发送 `online: false`
- 上位机重连时，应重新发送所有摄像头的 `camera_info`

---

### 3.7 system_status — 系统状态（可选）

```json
{"type":"system_status","cpu_usage":45,"memory_usage":60,"uptime":3600,"ros_status":"running"}\n
```

内容自定义，上位机会原样显示在命令窗口。

---

## 四、下位机需要接收的消息（Client → Server）

### 4.1 operator_input — 操作者输入快照

上位机通过键盘/手柄发送输入快照，不直接发送速度语义。下位机应根据当前模式、输入映射、安全状态和 watchdog 解析为 `/cmd_vel`、机械臂控制或其他内部指令。

```json
{"type":"operator_input","protocol_version":1,"seq":12345,"timestamp_ms":1709971200000,"ttl_ms":300,"mode":"vehicle","keyboard":{"pressed_keys":["w","d"]},"gamepad":{"connected":true,"buttons":{"a":false,"b":false,"x":false,"y":false,"start":false,"back":false,"lb":false,"rb":false,"l3":false,"r3":false,"dpad_up":false,"dpad_down":false,"dpad_left":false,"dpad_right":false},"axes":{"left_x":0.0,"left_y":0.82,"right_x":-0.31,"right_y":0.0,"lt":0.0,"rt":0.5}}}\n
```

**ROS 解析示例**：
```python
from geometry_msgs.msg import Twist

def handle_operator_input(data):
    if is_expired(data['timestamp_ms'], data.get('ttl_ms', 300)):
        publish_stop()
        return

    keys = set(data.get('keyboard', {}).get('pressed_keys', []))
    gamepad = data.get('gamepad', {})

    msg = Twist()
    # 这里的映射表属于下位机，不属于 Qt 上位机。
    if 'w' in keys:
        msg.linear.x += vehicle_params.max_linear_speed
    if 's' in keys:
        msg.linear.x -= vehicle_params.max_linear_speed
    msg.angular.z += gamepad.get('axes', {}).get('right_x', 0.0) * vehicle_params.max_angular_speed
    cmd_vel_pub.publish(msg)
```

---

### 4.2 emergency_stop — 急停

收到后应**立即**停止所有电机，优先级最高。

```json
{"type":"emergency_stop","timestamp":1709971200000}\n
```

**处理建议**：
- 发布零速度到 `/cmd_vel`
- 触发电机驱动器的急停功能
- 锁定机械臂关节

---

### 4.3 joint_control — 单关节控制

键盘机械臂模式下逐关节控制。

```json
{"type":"joint_control","joint_id":0,"position":1.5,"velocity":0.8,"timestamp":1709971200000}\n
```

| 字段 | 说明 |
|------|------|
| joint_id | 关节 ID（0-5） |
| position | 目标位置增量 |
| velocity | 运动速度 |

---

### 4.4 cartesian_control — 末端笛卡尔控制

手柄机械臂模式下的末端位姿增量控制。

```json
{"type":"cartesian_control","x":0.01,"y":0.0,"z":0.005,"roll":0.0,"pitch":0.0,"yaw":0.05,"timestamp":1709971200000}\n
```

| 字段 | 说明 |
|------|------|
| x, y, z | 末端位置增量（米） |
| roll, pitch, yaw | 末端姿态增量（弧度） |

---

### 4.5 heartbeat — 心跳

上位机每秒发送一次，下位机可据此检测上位机是否在线。

```json
{"type":"heartbeat","timestamp":1709971200000}\n
```

---

### 4.6 motor_command — 电机控制命令

完整的 6 关节 + 执行器批量控制数据。

```json
{"type":"motor_command","joints":[{"position":1.0,"current":0.5},{"position":2.0,"current":0.6},{"position":1.5,"current":0.4},{"position":0.8,"current":0.3},{"position":1.2,"current":0.7},{"position":0.9,"current":0.5}],"executor_position":0.5,"executor_torque":1.0,"executor_flags":1,"reserved":0}\n
```

| 字段 | 类型 | 说明 |
|------|------|------|
| joints | array[6] | 6 个关节，每个包含 position 和 current |
| joints[i].position | float | 关节目标位置 |
| joints[i].current | float | 关节目标电流 |
| executor_position | float | 执行器目标位置 |
| executor_torque | float | 执行器目标扭矩 |
| executor_flags | int | 标志位 |
| reserved | int | 保留字段 |

---

### 4.7 control_command — 综合控制命令（必须）

上位机发送的综合控制数据，包含 IMU 期望姿态、4 路摆臂电流、机械臂末端位姿。

```json
{"type":"control_command","timestamp":1709971200000,"imu":{"yaw":0.0,"roll":0.0,"pitch":0.0},"swing_arm_current":[0.5,0.5,0.5,0.5],"arm_end_position":{"x":0.3,"y":0.0,"z":0.2,"roll":0.0,"pitch":0.0,"yaw":0.0},"command_flags":1}\n
```

| 字段 | 类型 | 说明 |
|------|------|------|
| imu.yaw | float | 期望偏航角（度） |
| imu.roll | float | 期望横滚角（度） |
| imu.pitch | float | 期望俯仰角（度） |
| swing_arm_current | array[4] | 4 路摆臂目标电流（A） |
| arm_end_position.x | float | 机械臂末端 X 目标位置（m） |
| arm_end_position.y | float | 机械臂末端 Y 目标位置（m） |
| arm_end_position.z | float | 机械臂末端 Z 目标位置（m） |
| arm_end_position.roll | float | 机械臂末端横滚角（度） |
| arm_end_position.pitch | float | 机械臂末端俯仰角（度） |
| arm_end_position.yaw | float | 机械臂末端偏航角（度） |
| command_flags | int | 命令标志位（自定义） |

**ROS 转发建议**：
```python
def handle_control_command(data):
    # 1. IMU 期望姿态 → 底盘姿态控制 Topic
    # 2. 摆臂电流 → 4 路摆臂驱动器 Topic
    # 3. 末端位姿 → MoveIt / 逆运动学 Topic
    pass
```

---

### 4.8 system_command — 系统命令

通用命令通道，用于复位、模式切换等。

```json
{"type":"system_command","command":"reset","params":{"mode":"soft"},"timestamp":1709971200000}\n
```

---

## 五、Python 参考实现

以下为最小可用的 ROS 节点 TCP Server 示例：

```python
#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""tcp_bridge.py - 上位机 TCP 通信桥接节点"""

import rospy
import socket
import json
import threading
from geometry_msgs.msg import Twist
from sensor_msgs.msg import JointState, Imu

TCP_PORT = 9090
client_socket = None
client_lock = threading.Lock()


def send_json(data):
    """向上位机发送 JSON 消息"""
    with client_lock:
        if client_socket:
            try:
                msg = json.dumps(data, separators=(',', ':')) + '\n'
                client_socket.sendall(msg.encode('utf-8'))
            except Exception as e:
                rospy.logwarn("TCP send error: %s" % e)


# === 订阅 ROS Topic，转发给上位机 ===

def joint_state_cb(msg):
    """关节状态 → motor_state"""
    joints = []
    for i in range(min(6, len(msg.position))):
        joints.append({
            'position': msg.position[i],
            'current': msg.effort[i] if i < len(msg.effort) else 0.0
        })
    # 补齐6个
    while len(joints) < 6:
        joints.append({'position': 0.0, 'current': 0.0})

    send_json({
        'type': 'motor_state',
        'joints': joints,
        'executor_position': 0.0,
        'executor_torque': 0.0,
        'executor_flags': 0,
        'reserved': 0
    })


def imu_cb(msg):
    """IMU → imu_data"""
    # 从四元数转欧拉角（简化）
    import tf.transformations as tf_trans
    q = [msg.orientation.x, msg.orientation.y,
         msg.orientation.z, msg.orientation.w]
    euler = tf_trans.euler_from_quaternion(q)
    send_json({
        'type': 'imu_data',
        'roll': euler[0],
        'pitch': euler[1],
        'yaw': euler[2],
        'accel_x': msg.linear_acceleration.x,
        'accel_y': msg.linear_acceleration.y,
        'accel_z': msg.linear_acceleration.z
    })


def send_camera_info():
    """启动时上报所有摄像头信息"""
    cameras = [
        {'id': 0, 'url': 'rtsp://192.168.1.100:8554/cam0'},
        {'id': 1, 'url': 'rtsp://192.168.1.100:8554/cam1'},
        # 按需添加 cam2-cam4
    ]
    for cam in cameras:
        send_json({
            'type': 'camera_info',
            'camera_id': cam['id'],
            'online': True,
            'codec': 'h265',
            'width': 1280,
            'height': 720,
            'fps': 30,
            'bitrate_kbps': 2000,
            'rtsp_url': cam['url']
        })


# === 接收上位机消息，发布到 ROS Topic ===

cmd_vel_pub = None
emergency_pub = None


def handle_message(data):
    """处理上位机发来的 JSON 消息"""
    msg_type = data.get('type', '')

    if msg_type == 'operator_input':
        handle_operator_input(data)

    elif msg_type == 'emergency_stop':
        rospy.logwarn("EMERGENCY STOP received!")
        # 立即发布零速度
        twist = Twist()
        cmd_vel_pub.publish(twist)
        # TODO: 触发电机驱动急停

    elif msg_type == 'joint_control':
        joint_id = data.get('joint_id', 0)
        position = data.get('position', 0.0)
        velocity = data.get('velocity', 0.0)
        rospy.loginfo("Joint %d control: pos=%.3f vel=%.3f" %
                      (joint_id, position, velocity))
        # TODO: 发布到关节控制 Topic

    elif msg_type == 'cartesian_control':
        rospy.loginfo("Cartesian: x=%.3f y=%.3f z=%.3f r=%.3f p=%.3f y=%.3f" %
                      (data.get('x', 0), data.get('y', 0), data.get('z', 0),
                       data.get('roll', 0), data.get('pitch', 0), data.get('yaw', 0)))
        # TODO: 发布到 MoveIt 或逆运动学 Topic

    elif msg_type == 'heartbeat':
        pass  # 心跳，可用于检测上位机在线

    else:
        rospy.loginfo("Unknown message type: %s" % msg_type)


def client_thread(sock):
    """处理单个上位机连接"""
    global client_socket
    with client_lock:
        client_socket = sock
    rospy.loginfo("上位机已连接")

    # 连接后立即上报摄像头信息
    send_camera_info()

    buffer = ''
    try:
        while not rospy.is_shutdown():
            data = sock.recv(4096)
            if not data:
                break
            buffer += data.decode('utf-8')
            while '\n' in buffer:
                line, buffer = buffer.split('\n', 1)
                if line.strip():
                    try:
                        msg = json.loads(line)
                        handle_message(msg)
                    except json.JSONDecodeError as e:
                        rospy.logwarn("JSON parse error: %s" % e)
    except Exception as e:
        rospy.logwarn("Connection error: %s" % e)
    finally:
        with client_lock:
            client_socket = None
        sock.close()
        rospy.loginfo("上位机已断开")


def main():
    global cmd_vel_pub
    rospy.init_node('tcp_bridge', anonymous=False)

    # 发布者
    cmd_vel_pub = rospy.Publisher('/cmd_vel', Twist, queue_size=1)

    # 订阅者
    rospy.Subscriber('/joint_states', JointState, joint_state_cb)
    rospy.Subscriber('/imu/data', Imu, imu_cb)

    # TCP Server
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', TCP_PORT))
    server.listen(1)
    server.settimeout(1.0)
    rospy.loginfo("TCP Server listening on port %d" % TCP_PORT)

    while not rospy.is_shutdown():
        try:
            conn, addr = server.accept()
            rospy.loginfo("Connection from %s:%d" % addr)
            t = threading.Thread(target=client_thread, args=(conn,))
            t.daemon = True
            t.start()
        except socket.timeout:
            continue

    server.close()


if __name__ == '__main__':
    main()
```

---

## 六、快速验证

### 6.1 用 netcat 模拟下位机

```bash
# 监听 9090 端口
nc -l -p 9090

# 上位机连接后，手动输入 JSON 测试：
{"type":"motor_state","joints":[{"position":1.0,"current":0.5},{"position":2.0,"current":0.6},{"position":1.5,"current":0.4},{"position":0.8,"current":0.3},{"position":1.2,"current":0.7},{"position":0.9,"current":0.5}],"executor_position":0.5,"executor_torque":1.0,"executor_flags":1,"reserved":0}

{"type":"camera_info","camera_id":0,"online":true,"codec":"h265","width":1280,"height":720,"fps":30,"bitrate_kbps":2000,"rtsp_url":"rtsp://192.168.1.100:8554/cam0"}

{"type":"imu_data","roll":0.1,"pitch":-0.05,"yaw":1.57,"accel_x":0.01,"accel_y":0.02,"accel_z":9.81}

{"type":"co2_data","ppm":650.0}
```

### 6.2 用 Python 模拟下位机

```python
import socket, json, time

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('0.0.0.0', 9090))
server.listen(1)
print("等待上位机连接...")
conn, addr = server.accept()
print(f"已连接: {addr}")

# 发送摄像头信息
for i in range(5):
    conn.sendall((json.dumps({
        "type": "camera_info",
        "camera_id": i,
        "online": True,
        "codec": "h265",
        "width": 1280,
        "height": 720,
        "fps": 30,
        "bitrate_kbps": 2000,
        "rtsp_url": f"rtsp://192.168.1.100:8554/cam{i}"
    }, separators=(',', ':')) + '\n').encode())

# 周期发送电机状态
while True:
    conn.sendall((json.dumps({
        "type": "motor_state",
        "joints": [{"position": 0.0, "current": 0.0}] * 6,
        "executor_position": 0.0,
        "executor_torque": 0.0,
        "executor_flags": 0,
        "reserved": 0
    }, separators=(',', ':')) + '\n').encode())
    time.sleep(0.1)
```

---

## 七、注意事项

1. **JSON 必须以 `\n` 结尾**，上位机按行解析
2. **JSON 用紧凑格式**（`separators=(',', ':')`），不要格式化换行
3. **type 字段必须存在**，上位机根据此字段分发消息
4. **camera_id 范围 0-4**，对应 UI 网格前 5 个格子，超出范围会被忽略
5. **急停必须立即处理**，不要排队等待
6. **上位机重连后**，应重新发送所有 `camera_info` 和当前状态
