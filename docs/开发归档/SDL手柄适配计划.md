> 历史归档（2026-11-17 整理）。本文保留当时的设计、接口交接或验证记录；版本、路径、命令和临时授权仅对应原记录，不作为当前操作依据。当前说明见 [文档入口](../README.md)。

# SDL 手柄适配计划

## 1. 目标

将上位机手柄输入从 Windows XInput-only 扩展为基于 vendored SDL3 的跨平台输入后端，使 Linux 和 Windows 均可使用常见手柄进行机器人控制。

当前版本只适配基础控制能力：

- 连接状态检测
- A/B/X/Y 等标准按钮
- Start / Back
- D-Pad
- L1/R1、L3/R3
- 左右摇杆
- LT/RT 普通扳机轴
- UI 显示、急停关键命令和 `operator_input` 协议发送

当前版本不纳入：

- DS4/DS5 触摸板坐标
- DS4/DS5 陀螺仪/加速度计
- DualSense 自适应扳机
- 多手柄选择 UI
- 手柄震动、灯条、音频等输出能力

## 2. 设计原则

1. 上层接口保持稳定：继续使用 `ControllerState`、`HandleKey::getHandleKey`、`HandleKey::connectionChanged`。
2. 协议保持稳定：当前版本不修改 `operator_input` JSON 字段。
3. SDL3 优先：项目通过 `third_party/SDL` 自带 SDL3，并在构建时启用 SDL3 后端。
4. 保留 Windows fallback：Windows 下 SDL 读取失败时仍可回退到 XInput。
5. 减少系统依赖：Linux 不依赖发行版自带 SDL 包，避免被旧版 SDL2 卡住。

## 3. 当前版本实施范围

### 3.1 构建系统

文件：`src/controller/CMakeLists.txt`

任务：

- 通过 git submodule 固定 `third_party/SDL`。
- 顶层 CMake 使用 `add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)` 构建 vendored SDL3。
- 只启用当前和后续手柄能力需要的 SDL3 子系统：Joystick、HIDAPI、Sensor、Haptic。
- 禁用 SDL3 的 Audio、Video、Render、GPU、Camera、Dialog、Tray、Tests、Examples。
- 找到 `SDL3::SDL3` 时链接 SDL3，并定义 `USE_SDL3_GAMEPAD`。
- 找不到 vendored SDL3 时输出提示，并使用平台 fallback。

验收标准：

- Linux 不安装 `libsdl2-dev` 也可以启用 SDL3 后端。
- CMake 输出 `Vendored SDL3 enabled` 和 `SDL3 gamepad backend enabled`。
- Windows 构建时同样使用 vendored SDL3；SDL3 不可用时仍可保留 XInput 编译路径。

### 3.2 输入后端

文件：`src/controller/handlekey.h`、`src/controller/handlekey.cpp`

任务：

- 使用 SDL3 Gamepad API 打开第一个可用标准手柄。
- 每 20ms 轮询一次，保持现有 50Hz 频率。
- 读取按钮、D-Pad、摇杆和扳机。
- 将 SDL 输入映射到现有 `ControllerState`。
- 断开时释放 SDL gamepad，发出 `connectionChanged(false)`。
- 未连接时持续尝试打开第一个可用手柄，支持基础热插拔。

验收标准：

- 插入支持 SDL3 Gamepad 映射的手柄后 UI 显示已连接。
- 拔出手柄后 UI 显示未连接。
- 摇杆、LT/RT、按钮可在 `GamepadDisplayWidget` 中变化。
- A 键和 D-Pad 只作为普通按钮快照转发，不在上位机解析语意。
- L3+R3 同时按下时，上位机额外发送 `emergency_stop`。
- TCP 已连接时 `operator_input.gamepad.connected=true`，轴值归一化后发送。

## 4. 手柄兼容预期

当前版本目标支持 SDL3 能识别为 Gamepad 的设备：

- Xbox / XInput 手柄
- DualShock 4
- DualSense / PS5 手柄
- Switch Pro 手柄
- 大多数标准 HID 游戏手柄

边界说明：

- 不是任意 HID 设备都保证可用。
- 杂牌手柄如果没有 SDL Gamepad mapping，可能需要补 mapping。
- Windows 上如果 DS4/DS5 被 DS4Windows 或 Steam Input 伪装成 Xbox 手柄，基础输入可用，但程序看到的是虚拟 Xbox 设备。
- 要使用 DS4/DS5 高级能力，应避免只走虚拟 Xbox 路径。

## 5. Linux 部署要求

SDL3 已通过 `third_party/SDL` 随项目提供，不需要安装 `libsdl2-dev`。

```bash
git submodule update --init --recursive
```

重新配置构建：

```bash
cmake -S . -B build-smoke
cmake --build build-smoke -j2
```

确认 CMake 输出：

```text
Vendored SDL3 enabled
SDL3 gamepad backend enabled
```

如果插入手柄后无响应，排查顺序：

1. 确认系统能看到设备：`ls /dev/input/`
2. 测试输入事件：`jstest /dev/input/js0` 或 `evtest`
3. 确认用户权限，不建议长期用 `sudo` 启动上位机
4. 确认 SDL3 是否识别该设备为 Gamepad

## 6. Windows 部署要求

推荐使用 vendored SDL3 submodule，不依赖系统或 vcpkg 的 SDL 包。

运行期需要确保程序能找到 vendored 构建出的 SDL3 动态库。Windows 打包时需要携带 `SDL3.dll`。

Windows 兼容策略：

- SDL 可用：优先用 SDL 读取 DS4/DS5/Xbox/通用手柄。
- SDL 不可用：继续使用 XInput fallback，只支持 XInput 类设备。
- DS4/DS5 被虚拟成 Xbox：基础输入仍可用，高级能力不可用或不可区分。

## 7. 后续阶段计划

### 阶段 2：输入质量增强

- 摇杆死区过滤。
- 手柄名称显示。
- 连接日志显示 SDL 设备名。
- 多手柄选择。
- SDL3 Gamepad mapping 补充入口。

### 阶段 3：DS4/DS5 触摸板与运动传感器

候选能力：

- 触摸板按下/触点坐标。
- 陀螺仪。
- 加速度计。

需要改动：

- 扩展 `ControllerState`。
- 扩展 UI 显示。
- 扩展 `operator_input` 协议字段。
- 下位机 bridge 增加字段解析。

版本策略：

- 当前 vendored SDL3 固定为 `release-3.4.4`。
- 后续升级 SDL3 时需要重新验证基础手柄输入和 DS4/DS5 高级能力。

### 阶段 4：DualSense 自适应扳机

说明：

- 自适应扳机属于 DualSense 设备特定输出能力。
- SDL3 没有 `setAdaptiveTrigger` 这类高层 API，也不直接抽象 DualSense 扳机阻尼/武器感/扳机震动模式。
- SDL3 提供 `SDL_SendGamepadEffect()` 这种“发送设备特定 effect packet”的底层通道，具体 packet 格式需要项目自行维护。
- 当前项目优先支持 DualSense 有线 USB 模式；蓝牙模式暂不作为阶段 4 验收目标。
- 当前阶段仅记录为研究/实验项，不承诺 SDL3 原生支持；实现前需要先验证 USB output report。

建议实现方式：

- 新增 DualSense 专用输出模块，不混入普通 Gamepad 输入逻辑。
- 优先查验 SDL3 源码中 PS5 HIDAPI driver 的 effects packet 封装能力；如果公开 API 不足，再评估直接 HIDAPI/libusb 路径。
- 仅在确认当前设备为真实 DualSense 时启用。
- 避免在虚拟 Xbox 设备路径下暴露该能力。
- 所有效果设置都必须提供 `Off`/恢复默认动作，并在上位机退出或手柄断开前尽量清理扳机状态。

阶段 4.1：USB 探针工具

目标：

- 写一个独立命令行探针，不接入主 UI 和机器人控制链路。
- 使用当前 vendored SDL3 打开真实 DualSense。
- 确认连接方式为 USB，记录 vendor/product、name、path、real type。
- 尝试通过 `SDL_SendGamepadEffect()` 发送最小 USB effect packet。

验收标准：

- 探针能识别真实 DualSense：`vendor=054c`，`product=0ce6` 或后续兼容型号。
- 探针能执行 `Off`，确保扳机回到默认状态。
- 探针能让左/右扳机至少一种固定阻尼效果生效。

阶段 4.2：最小效果集

先实现 3 个动作：

- `Off`：关闭左右自适应扳机效果。
- `LeftResistance`：左扳机固定阻尼。
- `RightResistance`：右扳机固定阻尼。

暂不实现：

- 蓝牙 CRC/report。
- 完整武器感模式库。
- 随机器人状态自动变化的控制策略。
- 多手柄同时输出。

阶段 4.3：接入上位机

接入前提：

- USB 探针稳定。
- `Off` 清理动作可靠。
- 已确认不会影响基础输入、急停、普通控制链路。

接入方式：

- 新增 `DualSenseEffects` 模块。
- `HandleKey` 仍只负责输入读取。
- UI 只暴露实验开关或调试入口，不默认启用自适应扳机。
- 急停、断开手柄、退出程序时调用 `Off`。

候选接口：

```cpp
enum class AdaptiveTriggerMode {
    Off,
    Resistance,
    Weapon,
    Vibration
};

void setAdaptiveTrigger(TriggerSide side,
                        AdaptiveTriggerMode mode,
                        int start,
                        int end,
                        int strength);
```

## 8. 当前版本完成定义

当前版本完成的标准是：

- Linux 初始化 submodule 后可启用 vendored SDL3 手柄后端。
- Windows 保留 XInput fallback。
- 基础输入不修改现有协议即可发送到下位机。
- DS4/DS5/Xbox 等常见手柄在基础控制层面可用。
- 高级 DualSense 能力仅记录为后续计划，不影响当前版本交付。
