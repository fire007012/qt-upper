# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

基于Qt6的C++桌面上位机应用，用于机器人电机控制、实时监控和多摄像头显示。通过TCP/JSON与Intel NUC下位机（ROS2，部署版本以文档39为准）通信，支持RTSP视频流、手柄/键盘控制、IMU姿态显示、CO2传感器等功能。

```
Windows PC (上位机 Qt6)  ◄──TCP/JSON Port 9090──►  Intel NUC (下位机 ROS2)
                         ◄──RTSP视频流──────────►  摄像头模组 (5路)
```

当前工作边界以 `AGENTS.md` 为准：`ros2_ws_car/` 只读，禁止修改及写入测试/构建产物。旧桥接与部署资料已移至 `archive/ros1/`，不用于当前运行。管理服务使用独立 TCP9091，见 `39_下位机管理协议与Qt对接.md`。

## 构建命令

```bash
# 在项目根目录下（使用Qt Creator配置的构建目录）
cd build/Desktop_Qt_6_8_3_MinGW_64_bit-Debug

# 增量编译
cmake --build .
# 或直接
ninja

# 清理重建
ninja clean && ninja

# 从头配置（如修改了CMakeLists.txt）
cmake ../.. -G "Ninja"
ninja
```

**重要**：项目使用 `-O0` 全局禁用优化，绕过 GCC 13.1 汇编器 Segmentation fault 的已知bug。不要添加优化标志。

## 分层架构

```
┌─────────────────────────────────────────────────┐
│  MainWindow (mainwindow.h/cpp/ui)               │ ← UI层：主窗口、菜单、状态栏
│  + QML (RobotView.qml + RobotViewModel)         │
├─────────────────────────────────────────────────┤
│  Controller (src/controller/)                    │ ← 业务逻辑层
│  + UI控件: RtspPlayerWidget, CO2DisplayWidget,   │
│    GamepadDisplayWidget, DisplayLayoutManager    │
│  + 输入: KeyboardController, HandleKey(SDL3)     │
├─────────────────────────────────────────────────┤
│  Communication (src/communication/)              │ ← 通信层：仅TCP/JSON
│  RobotTcpClient + SharedStructs                   │
├─────────────────────────────────────────────────┤
│  Utils (src/utils/)                              │ ← 工具层
│  Logger + ErrorHandler                           │
└─────────────────────────────────────────────────┘
```

每层编译为独立静态库。依赖方向：上层 → 下层，通过Qt信号槽解耦。

### 层级规则
- **Communication层**：只负责TCP连接和原始JSON收发，不含业务逻辑。通过signal将数据向上传递
- **Controller层**：协调通信层，提供统一业务接口（`sendOperatorInput`, `sendJointControl`, `sendEmergencyStop` 等）给UI层。也包含自定义UI控件
- **Utils层**：Logger日志和ErrorHandler，被所有层依赖
- **UI层**（根目录）：MainWindow组装所有组件，处理键盘/手柄事件分发

### 关键数据流

下位机状态 → `RobotTcpClient`(TCP JSON接收) → `Controller`(信号转发) → `MainWindow`(更新UI)

用户操作 → `MainWindow`(事件捕获) → `KeyboardController`/`HandleKey` → `Controller`(命令封装) → `RobotTcpClient`(JSON发送) → 下位机

## 核心组件

### 通信 (src/communication/)
- **`RobotTcpClient`**：异步TCP客户端，JSON格式数据交换，自动重连，心跳检测。主要信号：`motorStateReceived`, `co2DataReceived`, `imuDataReceived`, `cameraInfoReceived`
- **`SharedStructs.h`**：定义`Communication::MotorState`, `Communication::Command`等跨层数据结构

### 控制 (src/controller/)
- **`Controller`**：业务逻辑核心，封装所有与下位机交互的命令接口
- **`KeyboardController`**：采集键盘输入并输出协议层按键名（车体/机械臂双模式）
- **`HandleKey`**：SDL3 Gamepad手柄驱动，Windows下保留XInput fallback，轮询手柄状态并发出`ControllerState`信号
- **`RtspPlayerWidget`**：基于 FFmpeg 子进程低缓冲拉流的 RTSP 视频播放控件，5 路并发
- **`DisplayLayoutManager`**：管理2×3视频网格布局
- **`RobotViewModel`**：QML与C++桥接，暴露Roll/Pitch/Yaw属性给3D姿态视图

### 控制模式
- **Vehicle模式**：键盘WASD/手柄摇杆控制车体运动（linearX/Y, angularZ）
- **Arm模式**：控制机械臂关节和末端执行器

## 技术栈

- Qt 6.8.3 (MinGW 64-bit), C++20, CMake 3.16+
- Qt模块：Core, Widgets, Network, Quick, QuickWidgets
- 运行时工具：FFmpeg 可执行文件用于 RTSP 低延迟播放和下位机推流
- 开发机 FFmpeg 安装说明见 `docs/开发归档/FFmpeg开发环境安装指南.md`
- Windows XInput（手柄支持）
- QML用于3D机器人姿态渲染（`resources/qml/RobotView.qml`）

## 通讯协议

### TCP JSON协议 (Port 9090)

接收：`motor_state`（关节位置/电流）、`co2_data`、`imu_data`、`camera_info`（含RTSP URL）
发送：`operator_input`（高频键盘/手柄输入快照，不逐条 ACK）、`emergency_stop` / `system_command`（关键命令，必须 ACK），以及保留的低频机械臂/电机控制帧。

摄像头信息推送触发RTSP自动播放：`camera_id` 0-4对应2×3网格前5格，`online: true`时启动播放。

## 开发注意事项

- 修改CMakeLists.txt后务必验证编译通过，不要随意添加/删除依赖
- GCC 13.1已知bug：不要对`mainwindow.cpp`、`handlekey.cpp`、`ErrorHandler.cpp`启用优化
- 代码修改后立即编译验证，不要批量修改后再编译
- UI布局修改前先确认布局方案，避免反复迭代

## 测试与调试

```bash
# 独立TCP/JSON协议mock（不启动ROS或硬件）
python scripts/mock_robot_server.py
```

## 对话要求
对我的称呼为爸爸，每次对话必须以喵字结尾
